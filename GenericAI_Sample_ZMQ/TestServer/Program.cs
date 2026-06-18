using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace GenericAI.TestServer
{
    // Connectivity / smoke-test server for the ZMQ frame plane of GenericAI_ZMQ.
    // Plays the recorder / AIServiceModule side:
    //   1. HTTP POST /SetParameters to the AI (so the channel builds its pool).
    //   2. BIND a ZMQ PUSH socket; GenericAI.exe CONNECTs a PULL to it.
    //   3. Frame source:
    //        rtsp=... -> spawn ffmpeg, pull RTSP, stream Annex-B NAL, real FILETIME ts.
    //        file=... -> read a .h264 file, synthetic ts (debug fallback).
    //      Each access unit is PUSHed as 2 parts [ZmqFrameHeader | NAL], channel_id tagged.
    //   4. Best-effort HTTP listener for PostAnalyticsResult so detections print.
    //
    // No NuGet: ZMQ is P/Invoked into the libzmq the wrapper already ships.
    internal static class Program
    {
        // ---- libzmq P/Invoke (C API) -----------------------------------------
        private const string ZMQ = "libzmq-v143-mt-4_2_0.dll";  // match the wrapper's libzmq
        private const int ZMQ_PUSH = 8;
        private const int ZMQ_PULL = 7;
        private const int ZMQ_SNDMORE = 2;
        private const int ZMQ_SNDHWM = 23;
        private const int ZMQ_SNDTIMEO = 28;
        private const int ZMQ_RCVTIMEO = 27;
        private const int ZMQ_LINGER = 17;

        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr zmq_ctx_new();
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern int zmq_ctx_term(IntPtr ctx);
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr zmq_socket(IntPtr ctx, int type);
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern int zmq_close(IntPtr s);
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)] private static extern int zmq_bind(IntPtr s, string addr);
        // size_t params (len) are 8 bytes on x64 -> marshal as UIntPtr, not int.
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern int zmq_setsockopt(IntPtr s, int opt, ref int val, UIntPtr len);
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern int zmq_send(IntPtr s, byte[] buf, UIntPtr len, int flags);
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern int zmq_errno();
        // zmq_msg_t recv path for the result PULL (opaque 64-byte struct, heap-held).
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern int zmq_msg_init(IntPtr msg);
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern int zmq_msg_recv(IntPtr msg, IntPtr s, int flags);
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr zmq_msg_data(IntPtr msg);
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern UIntPtr zmq_msg_size(IntPtr msg);
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern int zmq_msg_close(IntPtr msg);

        // ---- ZmqFrameHeader wire format (must match zmq_frame_header.h, 28 bytes) --
        private const uint  kZmqFrameMagic   = 0x5A4D4600u;
        private const ushort kZmqFrameVersion = 1;

        private static volatile bool s_running = true;
        private static long s_sent = 0;

        private static int Main(string[] args)
        {
            Config cfg = Config.Parse(args);
            if (cfg == null) { Console.WriteLine(Config.Usage); return 2; }
            bool useRtsp = !string.IsNullOrEmpty(cfg.Rtsp);
            if (!useRtsp && !File.Exists(cfg.File))
            {
                Console.WriteLine("Provide rtsp=<url> or file=<clip.h264>. " + Config.Usage);
                return 2;
            }

            Console.CancelKeyPress += (s, e) => { e.Cancel = true; s_running = false; };

            Console.WriteLine($"AI         : http://{cfg.AiHost}:{cfg.AiPort}");
            Console.WriteLine($"ZMQ bind   : {cfg.ZmqBind}  (AI connects PULL here)");
            Console.WriteLine($"channel_id : {cfg.ChannelId}");
            Console.WriteLine(useRtsp ? $"source     : RTSP {cfg.Rtsp} ({cfg.Transport})  {cfg.Width}x{cfg.Height}"
                                      : $"source     : file {cfg.File}  {cfg.Width}x{cfg.Height} @ {cfg.Fps}fps loop={cfg.Loop}");

            if (cfg.UseResultZmq) StartZmqResultListener(cfg.ResultZmq);
            else StartResultListener(cfg.ResultPort);

            if (!SendSetParameters(cfg))
                Console.WriteLine("WARN: SetParameters failed - is GenericAI.exe running? Frames may be dropped.");

            IntPtr ctx = zmq_ctx_new();
            IntPtr sock = zmq_socket(ctx, ZMQ_PUSH);
            int hwm = 16, sndtimeo = 2000;
            zmq_setsockopt(sock, ZMQ_SNDHWM, ref hwm, (UIntPtr)(uint)sizeof(int));
            zmq_setsockopt(sock, ZMQ_SNDTIMEO, ref sndtimeo, (UIntPtr)(uint)sizeof(int));
            if (zmq_bind(sock, cfg.ZmqBind) != 0)
            {
                Console.WriteLine($"zmq_bind({cfg.ZmqBind}) failed, errno={zmq_errno()}");
                return 1;
            }
            Console.WriteLine("PUSH bound. Streaming frames... (Ctrl+C to stop)");

            try
            {
                if (useRtsp) RunRtsp(cfg, sock);
                else RunFile(cfg, sock);
            }
            catch (Exception ex) { Console.WriteLine("Streaming error: " + ex.Message); }

            Console.WriteLine($"Done. total sent={s_sent}");
            zmq_close(sock);
            zmq_ctx_term(ctx);
            return 0;
        }

        // ---- RTSP source (ffmpeg subprocess) ---------------------------------
        private static void RunRtsp(Config cfg, IntPtr sock)
        {
            // -c:v copy keeps the camera's H.264; h264_mp4toannexb ensures Annex-B
            // start codes; -f h264 pipe:1 streams raw NAL to stdout.
            string args = $"-rtsp_transport {cfg.Transport} -i \"{cfg.Rtsp}\" -an -c:v copy " +
                          "-bsf:v h264_mp4toannexb -fflags nobuffer -f h264 pipe:1";
            var psi = new ProcessStartInfo
            {
                FileName = cfg.Ffmpeg,
                Arguments = args,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
            };
            Console.WriteLine($"ffmpeg {args}");

            Process proc;
            try { proc = Process.Start(psi); }
            catch (Exception ex)
            {
                Console.WriteLine($"Cannot start ffmpeg ('{cfg.Ffmpeg}'): {ex.Message}. " +
                                  "Put ffmpeg.exe on PATH or pass ffmpeg=<path>.");
                return;
            }

            // Drain ffmpeg stderr (connection/stream info) on a side thread.
            var errThread = new Thread(() =>
            {
                try
                {
                    string line;
                    while ((line = proc.StandardError.ReadLine()) != null)
                        Console.WriteLine("[ffmpeg] " + line);
                }
                catch { }
            }) { IsBackground = true };
            errThread.Start();

            // ts = real Windows FILETIME (100ns ticks, UTC) at send time - this is
            // what the recorder would stamp; it round-trips back in the result.
            var asm = new AuAssembler((au, key) =>
                SendAu(sock, cfg, au, key, (ulong)DateTime.UtcNow.ToFileTimeUtc()));

            Stream stdout = proc.StandardOutput.BaseStream;
            byte[] chunk = new byte[65536];
            var acc = new List<byte>(1 << 20);
            int read;
            while (s_running && (read = stdout.Read(chunk, 0, chunk.Length)) > 0)
            {
                for (int i = 0; i < read; i++) acc.Add(chunk[i]);
                DrainCompleteNals(acc, asm);
            }
            asm.Flush();
            try { if (!proc.HasExited) proc.Kill(); } catch { }
        }

        // ---- file source (debug fallback) ------------------------------------
        private static void RunFile(Config cfg, IntPtr sock)
        {
            byte[] file = File.ReadAllBytes(cfg.File);
            int frameDelayMs = Math.Max(1, 1000 / Math.Max(1, cfg.Fps));
            ulong ts = 0;
            ulong tsStep = (ulong)(10000000 / Math.Max(1, cfg.Fps));

            var asm = new AuAssembler((au, key) =>
            {
                SendAu(sock, cfg, au, key, ts);
                ts += tsStep;
                Thread.Sleep(frameDelayMs);
            });

            do
            {
                foreach (NalRange nal in IterateNals(file))
                {
                    if (!s_running) break;
                    byte[] one = new byte[nal.Len];
                    Array.Copy(file, nal.Start, one, 0, nal.Len);
                    asm.Feed(one);
                }
                asm.Flush();
            } while (s_running && cfg.Loop);
        }

        // ---- send one access unit --------------------------------------------
        private static void SendAu(IntPtr sock, Config cfg, byte[] au, bool key, ulong ts)
        {
            if (au == null || au.Length == 0) return;
            byte[] hdr = BuildHeader(cfg, au.Length, key, ts);
            int r1 = zmq_send(sock, hdr, (UIntPtr)(uint)hdr.Length, ZMQ_SNDMORE);
            int r2 = (r1 >= 0) ? zmq_send(sock, au, (UIntPtr)(uint)au.Length, 0) : -1;
            long n = ++s_sent;
            if (r1 < 0 || r2 < 0)
            {
                if ((n % 30) == 0) Console.WriteLine($"(send pending - AI not consuming yet, errno={zmq_errno()})");
            }
            else if ((n % 30) == 0)
            {
                Console.WriteLine($"sent {n} frames (last {(key ? "KEY " : "")}{au.Length} bytes, ts={ts})");
            }
        }

        // ---- access-unit assembler -------------------------------------------
        // Groups NAL units into access units (one VCL slice ends an AU; SPS/PPS/SEI
        // ride with the next VCL). Caches SPS/PPS and prepends them to keyframe AUs
        // so a decoder that joins mid-stream still recovers.
        private sealed class AuAssembler
        {
            private readonly Action<byte[], bool> _onAu;
            private readonly List<byte> _pending = new List<byte>();
            private bool _hasVcl;
            private byte[] _sps, _pps;

            public AuAssembler(Action<byte[], bool> onAu) { _onAu = onAu; }

            public void Feed(byte[] nal)
            {
                int type = NalType(nal);
                if (type == 7) _sps = nal;
                else if (type == 8) _pps = nal;

                bool isVcl = type >= 1 && type <= 5;
                if (isVcl && _hasVcl) Flush();
                _pending.AddRange(nal);
                if (isVcl) _hasVcl = true;
            }

            public void Flush()
            {
                if (_pending.Count == 0) return;
                byte[] au = _pending.ToArray();
                _pending.Clear();
                _hasVcl = false;

                bool key = AuHasType(au, 5);
                if (key && !AuHasType(au, 7) && _sps != null && _pps != null)
                {
                    // Prepend cached SPS+PPS so the keyframe is self-contained.
                    byte[] merged = new byte[_sps.Length + _pps.Length + au.Length];
                    Buffer.BlockCopy(_sps, 0, merged, 0, _sps.Length);
                    Buffer.BlockCopy(_pps, 0, merged, _sps.Length, _pps.Length);
                    Buffer.BlockCopy(au, 0, merged, _sps.Length + _pps.Length, au.Length);
                    au = merged;
                }
                _onAu(au, key);
            }
        }

        // ---- ZmqFrameHeader builder (28 bytes, little-endian) ----------------
        private static byte[] BuildHeader(Config cfg, int payloadSz, bool keyframe, ulong ts)
        {
            using (var ms = new MemoryStream(28))
            using (var w = new BinaryWriter(ms))
            {
                w.Write(kZmqFrameMagic);
                w.Write(kZmqFrameVersion);
                w.Write((byte)1);                  // codec 1=H264
                w.Write((byte)(keyframe ? 1 : 0));
                w.Write((ushort)cfg.Width);
                w.Write((ushort)cfg.Height);
                w.Write((uint)cfg.ChannelId);
                w.Write(ts);
                w.Write((uint)payloadSz);
                w.Flush();
                return ms.ToArray();
            }
        }

        // ---- Annex-B helpers --------------------------------------------------
        private struct NalRange { public int Start; public int Len; }

        // Drains complete NAL units (between consecutive start codes) from the
        // streaming accumulator, keeping the trailing incomplete NAL.
        private static void DrainCompleteNals(List<byte> acc, AuAssembler asm)
        {
            var pos = new List<int>();
            for (int i = 0; i + 2 < acc.Count; i++)
            {
                if (acc[i] == 0 && acc[i + 1] == 0 && acc[i + 2] == 1)
                {
                    pos.Add((i > 0 && acc[i - 1] == 0) ? i - 1 : i);
                    i += 2;
                }
            }
            if (pos.Count < 2) return;
            for (int p = 0; p < pos.Count - 1; p++)
            {
                int start = pos[p], end = pos[p + 1];
                byte[] nal = new byte[end - start];
                acc.CopyTo(start, nal, 0, end - start);
                asm.Feed(nal);
            }
            acc.RemoveRange(0, pos[pos.Count - 1]);
        }

        private static IEnumerable<NalRange> IterateNals(byte[] d)
        {
            int i = FindStartCode(d, 0);
            while (i >= 0)
            {
                int next = FindStartCode(d, i + 3);
                int end = (next >= 0) ? next : d.Length;
                yield return new NalRange { Start = i, Len = end - i };
                i = next;
            }
        }

        private static int FindStartCode(byte[] d, int from)
        {
            for (int i = from; i + 2 < d.Length; i++)
                if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
                    return (i > 0 && d[i - 1] == 0) ? i - 1 : i;
            return -1;
        }

        private static int NalType(byte[] nal)
        {
            int off = (nal.Length >= 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1) ? 4 : 3;
            return off < nal.Length ? (nal[off] & 0x1F) : 0;
        }

        private static bool AuHasType(byte[] au, int type)
        {
            for (int i = 0; i + 3 < au.Length; i++)
                if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1 && (au[i + 3] & 0x1F) == type)
                    return true;
            return false;
        }

        // ---- HTTP: SetParameters ---------------------------------------------
        private static bool SendSetParameters(Config cfg)
        {
            int w = cfg.Width, h = cfg.Height;
            // ROI MUST carry >= 3 points: param_snapshot.cpp drops <3-point ROIs and
            // takes the diagonal from points[0]/points[2]. A 4-corner full-frame
            // polygon keeps every person detection (full-frame ROI) and supplies the
            // (threshold, sensitivity) the detector uses.
            string body =
                "{\"version\":\"1.3\"," +
                "\"analytics_event_api_url\":\"http://127.0.0.1:" + cfg.ResultPort + "/PostAnalyticsResult\"," +
                "\"image_width\":" + w + ",\"image_height\":" + h + ",\"jpg_compress\":80," +
                "\"rois\":[{\"sensitivity\":50,\"threshold\":50,\"rects\":[" +
                "{\"x\":0,\"y\":0},{\"x\":" + w + ",\"y\":0}," +
                "{\"x\":" + w + ",\"y\":" + h + "},{\"x\":0,\"y\":" + h + "}]}]}";
            try
            {
                var req = (HttpWebRequest)WebRequest.Create($"http://{cfg.AiHost}:{cfg.AiPort}/SetParameters");
                req.Method = "POST";
                req.ContentType = "application/json";
                byte[] data = Encoding.UTF8.GetBytes(body);
                req.ContentLength = data.Length;
                using (var s = req.GetRequestStream()) s.Write(data, 0, data.Length);
                using (var resp = (HttpWebResponse)req.GetResponse())
                {
                    Console.WriteLine($"SetParameters -> {(int)resp.StatusCode} {resp.StatusCode}");
                    return resp.StatusCode == HttpStatusCode.OK;
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"SetParameters error: {ex.Message}");
                return false;
            }
        }

        // ---- HTTP: result listener (best effort) -----------------------------
        private static void StartResultListener(int port)
        {
            HttpListener listener;
            try
            {
                listener = new HttpListener();
                listener.Prefixes.Add($"http://127.0.0.1:{port}/");
                listener.Start();
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Result listener disabled ({ex.Message}). Frame streaming still works.");
                return;
            }
            Console.WriteLine($"Result listener: http://127.0.0.1:{port}/PostAnalyticsResult");
            var t = new Thread(() =>
            {
                while (s_running)
                {
                    HttpListenerContext ctx;
                    try { ctx = listener.GetContext(); }
                    catch { break; }
                    try
                    {
                        string bodyTxt;
                        using (var r = new StreamReader(ctx.Request.InputStream, ctx.Request.ContentEncoding))
                            bodyTxt = r.ReadToEnd();
                        string ts = ExtractJsonNumber(bodyTxt, "timestamp");
                        string ch = ExtractJsonNumber(bodyTxt, "port_num");
                        Console.WriteLine($"[RESULT] channel={ch} timestamp={ts}");
                        byte[] ok = Encoding.UTF8.GetBytes("OK");
                        ctx.Response.StatusCode = 200;
                        ctx.Response.OutputStream.Write(ok, 0, ok.Length);
                        ctx.Response.OutputStream.Close();
                    }
                    catch { }
                }
            }) { IsBackground = true };
            t.Start();
        }

        // ---- ZMQ: result listener (PULL bind; AI connects PUSH) --------------
        private static void StartZmqResultListener(string endpoint)
        {
            IntPtr ctx = zmq_ctx_new();
            IntPtr sock = zmq_socket(ctx, ZMQ_PULL);
            int rcvtimeo = 200, linger = 0;
            zmq_setsockopt(sock, ZMQ_RCVTIMEO, ref rcvtimeo, (UIntPtr)(uint)sizeof(int));
            zmq_setsockopt(sock, ZMQ_LINGER, ref linger, (UIntPtr)(uint)sizeof(int));
            if (zmq_bind(sock, endpoint) != 0)
            {
                Console.WriteLine($"result PULL bind({endpoint}) failed, errno={zmq_errno()}");
                return;
            }
            Console.WriteLine($"Result PULL bound: {endpoint}  (AI connects PUSH here)");

            var t = new Thread(() =>
            {
                IntPtr msg = Marshal.AllocHGlobal(64);  // zmq_msg_t is 64 bytes
                while (s_running)
                {
                    zmq_msg_init(msg);
                    int n = zmq_msg_recv(msg, sock, 0);  // rcvtimeo -> polls s_running
                    if (n >= 0)
                    {
                        int size = (int)zmq_msg_size(msg);
                        byte[] buf = new byte[size];
                        if (size > 0) Marshal.Copy(zmq_msg_data(msg), buf, 0, size);
                        string json = Encoding.UTF8.GetString(buf);
                        string ts = ExtractJsonNumber(json, "timestamp");
                        string ch = ExtractJsonNumber(json, "port_num");
                        Console.WriteLine($"[RESULT] channel={ch} timestamp={ts}");
                    }
                    zmq_msg_close(msg);
                }
                Marshal.FreeHGlobal(msg);
                zmq_close(sock);
                zmq_ctx_term(ctx);
            }) { IsBackground = true };
            t.Start();
        }

        private static string ExtractJsonNumber(string json, string key)
        {
            if (string.IsNullOrEmpty(json)) return "?";
            int i = json.IndexOf("\"" + key + "\"");
            if (i < 0) return "?";
            i = json.IndexOf(':', i);
            if (i < 0) return "?";
            i++;
            while (i < json.Length && (json[i] == ' ' || json[i] == '"')) i++;
            int start = i;
            while (i < json.Length && (char.IsDigit(json[i]) || json[i] == '-')) i++;
            return i > start ? json.Substring(start, i - start) : "?";
        }

        // ---- config -----------------------------------------------------------
        private sealed class Config
        {
            public string AiHost = "127.0.0.1";
            public int    AiPort = 51000;
            public string ZmqBind = "tcp://*:5556";
            public int    ChannelId = 51000;
            public string Rtsp = "";
            public string Ffmpeg = "ffmpeg";
            public string Transport = "tcp";
            public string File = "clip.h264";
            public int    Width = 1920;
            public int    Height = 1080;
            public int    Fps = 15;
            public bool   Loop = true;
            public int    ResultPort = 9999;
            public string ResultZmq = "";   // tcp://*:5557 -> receive results over ZMQ (PULL bind)
            public bool   UseResultZmq { get { return !string.IsNullOrEmpty(ResultZmq); } }

            public const string Usage =
                "Usage: GenericAI.TestServer.exe (rtsp=<url> | file=<clip.h264>) width=<W> height=<H>\n" +
                "         [ai_host=127.0.0.1] [ai_port=51000] [channel=51000] [zmq=tcp://*:5556]\n" +
                "         [transport=tcp|udp] [ffmpeg=<path>] [fps=15] [loop=true]\n" +
                "         [result_port=9999 | result_zmq=tcp://*:5557]\n" +
                "channel must equal the AI channel's port (ai_port + index).\n" +
                "result_zmq receives results over ZMQ (PULL bind); launch the AI with" +
                " result_endpoint=tcp://<this-host>:5557.";

            public static Config Parse(string[] args)
            {
                var c = new Config();
                foreach (var a in args)
                {
                    int eq = a.IndexOf('=');
                    if (eq <= 0) return null;
                    string k = a.Substring(0, eq).Trim().ToLowerInvariant();
                    string v = a.Substring(eq + 1).Trim();
                    switch (k)
                    {
                        case "rtsp": c.Rtsp = v; break;
                        case "ffmpeg": c.Ffmpeg = v; break;
                        case "transport": c.Transport = v; break;
                        case "file": c.File = v; break;
                        case "width": c.Width = int.Parse(v); break;
                        case "height": c.Height = int.Parse(v); break;
                        case "ai_host": c.AiHost = v; break;
                        case "ai_port": c.AiPort = int.Parse(v); break;
                        case "channel": c.ChannelId = int.Parse(v); break;
                        case "zmq": c.ZmqBind = v; break;
                        case "fps": c.Fps = int.Parse(v); break;
                        case "loop": c.Loop = v != "0" && !v.Equals("false", StringComparison.OrdinalIgnoreCase); break;
                        case "result_port": c.ResultPort = int.Parse(v); break;
                        case "result_zmq": c.ResultZmq = v; break;
                        default: return null;
                    }
                }
                return c;
            }
        }
    }
}
