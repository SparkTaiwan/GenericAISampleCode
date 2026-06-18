using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;

namespace GenericAI.App
{
    public static class Program
    {
        public const int ExitOk        = 0;
        public const int ExitGeneric   = 1;
        public const int ExitBadArgs   = 2;
        public const int ExitPortInUse = 3;

        private static int s_cleanupRun;

        // Pinned for the lifetime of the registration: native worker threads
        // hold the function pointer until GAI_RegisterCallback(null).
        private static NativeInterop.CallBackFunction _callbackDelegate;
        private static GCHandle _callbackHandle;
        private static bool _callbackRegistered;

        private static List<ChannelHandle> _channels;
        private static Dictionary<int, ChannelHandle> _byChannelId;
        private static CancellationTokenSource _shutdownCts;

        // Send side: native callback threads enqueue finished envelopes, a
        // timer drains the queue every 500 ms and POSTs fire-and-forget.
        private static readonly Queue<HttpEnvelope> _sendQueue = new Queue<HttpEnvelope>();
        private static readonly object _sendLock = new object();
        private static Timer _sendTimer;
        private static readonly HttpPostClient _postClient = new HttpPostClient();
        private static ZmqResultSender _zmqResultSender;  // null => HTTP results

        public static async Task<int> Main(string[] args)
        {
            Console.WriteLine(CommandLineArgs.Usage());

            if (!CommandLineArgs.TryParse(args, out CommandLineArgs parsed, out string err))
            {
                Console.Error.WriteLine($"Bad args: {err}");
                Console.Error.WriteLine(CommandLineArgs.Usage());
                return ExitBadArgs;
            }

            if (parsed.PortFromArgs)
                Console.WriteLine($"Port number: {parsed.Port}");
            else
                Console.WriteLine($"Invalid Input. Use default {CommandLineArgs.DefaultPort}");

            int n = parsed.ChannelCount;
            int[] ports = new int[n];
            // Consecutive sample ports (aligns with spark.recorder's getExeServerPort() + index scheme).
            for (int k = 0; k < n; k++) ports[k] = parsed.Port + k;

            Console.Title = $"GenericAI ports:{string.Join(",", ports)}";

            AppDomain.CurrentDomain.ProcessExit += (s, e) => Cleanup();
            Console.CancelKeyPress += (s, e) =>
            {
                // Suppress runtime auto-terminate so Cleanup's native Deinit
                // finishes; the main loop unwinds on its own once the CTS
                // cancels all listener.RunAsync tasks.
                e.Cancel = true;
                Cleanup();
            };

            try
            {
                _channels = new List<ChannelHandle>(n);
                _byChannelId = new Dictionary<int, ChannelHandle>(n);
                foreach (int p in ports)
                {
                    ChannelHandle h = new ChannelHandle(p, parsed.HttpHost);
                    _channels.Add(h);
                    _byChannelId[p] = h;
                    Console.WriteLine($"httpServerUrl: http://{parsed.HttpHost}:{p}/");
                }

                foreach (ChannelHandle h in _channels)
                {
                    if (!h.StartListener())
                    {
                        Console.Error.WriteLine($"Port {h.Port} is in use; exiting");
                        return ExitPortInUse;
                    }
                }

                int rc = NativeInterop.GAI_InitializeChannels(ports, n);
                if (rc != 0)
                {
                    Console.Error.WriteLine($"GAI_InitializeChannels returned {rc}");
                    return ExitGeneric;
                }

                // Pin the delegate so the GC cannot move/collect it while the
                // native side holds a function pointer to it.
                _callbackDelegate = OnNativeCallback;
                _callbackHandle = GCHandle.Alloc(_callbackDelegate);
                NativeInterop.GAI_RegisterCallback(_callbackDelegate);
                _callbackRegistered = true;
                Console.WriteLine("register Callback");

                // Result plane: ZMQ PUSH (connect to the recorder's bound PULL) when
                // result_endpoint is given, else HTTP POST (default).
                if (parsed.UseZmqResults)
                {
                    try
                    {
                        _zmqResultSender = new ZmqResultSender(parsed.ResultEndpoint, parsed.ChannelCount);
                        Console.WriteLine($"ZMQ result sink: connect PUSH -> {parsed.ResultEndpoint}");
                    }
                    catch (Exception ex)
                    {
                        Console.Error.WriteLine($"ZMQ result sender init failed: {ex.Message}");
                        return ExitGeneric;
                    }
                }

                // Frame plane: ZMQ (NAL, decoded internally) when frame_endpoint is
                // given, else MMF (default). Start BEFORE the listeners serve
                // /SetParameters so channels are in ZMQ mode (they skip the MMF poll).
                // GAI_Deinitialize stops the receiver first on shutdown.
                if (parsed.UseZmqFrames)
                {
                    int zrc = NativeInterop.GAI_StartZmqReceiver(parsed.FrameEndpoint);
                    if (zrc != 0)
                    {
                        Console.Error.WriteLine($"GAI_StartZmqReceiver({parsed.FrameEndpoint}) returned {zrc}");
                        return ExitGeneric;
                    }
                    Console.WriteLine($"ZMQ frame source: connect PULL -> {parsed.FrameEndpoint}");
                }

                _sendTimer = new Timer(ProcessSendQueue, null,
                    TimeSpan.Zero, TimeSpan.FromMilliseconds(500));

                _shutdownCts = new CancellationTokenSource();
                await Task.WhenAll(_channels.Select(c => c.Listener.RunAsync(_shutdownCts.Token))).ConfigureAwait(false);

                return ExitOk;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"Main fatal: {ex}");
                return ExitGeneric;
            }
            finally
            {
                Cleanup();
            }
        }

        // Native callback target. Runs on a C++ channel worker thread; must
        // finish with the frame before returning (the native side reuses the
        // buffer) and must never throw — exceptions across the P/Invoke
        // boundary are undefined behaviour.
        private static void OnNativeCallback(
            int channelId, int width, int height,
            IntPtr frameI420, int frameSize,
            ulong timestamp,
            IntPtr roisFlat, int roisCount, int nodeCount)
        {
            try
            {
                if (frameSize <= 0 || frameI420 == IntPtr.Zero || roisCount <= 0 || nodeCount <= 0)
                    return;

                ChannelHandle channel;
                if (!_byChannelId.TryGetValue(channelId, out channel))
                {
                    Console.WriteLine($"Detection callback: unknown channel_id={channelId}");
                    return;
                }

                Console.WriteLine($"Detection callback: ch={channelId} {width}x{height} size={frameSize} rois={roisCount}");

                byte[] jpeg = ConvertYUV420ToJpeg(frameI420, frameSize, width, height,
                    channel.Parameters.JpgQuality);

                List<List<NativeInterop.ROI>> rois = new List<List<NativeInterop.ROI>>(roisCount);
                int roiSize = Marshal.SizeOf<NativeInterop.ROI>();
                for (int i = 0; i < roisCount; i++)
                {
                    List<NativeInterop.ROI> nodes = new List<NativeInterop.ROI>(nodeCount);
                    for (int j = 0; j < nodeCount; j++)
                    {
                        int offset = (i * nodeCount + j) * roiSize;
                        nodes.Add(Marshal.PtrToStructure<NativeInterop.ROI>(IntPtr.Add(roisFlat, offset)));
                    }
                    rois.Add(nodes);
                }

                HttpEnvelope env = new HttpEnvelope
                {
                    version = "1.2",
                    port_num = channelId,
                    keyframe = jpeg,
                    timestamp = timestamp,
                    rois_rects = rois,
                };

                lock (_sendLock)
                    _sendQueue.Enqueue(env);
            }
            catch (Exception ex)
            {
                // Never let an exception unwind into the native caller.
                try { Console.Error.WriteLine($"Callback error: {ex.Message}"); } catch { }
            }
        }

        private static void ProcessSendQueue(object state)
        {
            while (true)
            {
                HttpEnvelope env;
                lock (_sendLock)
                {
                    if (_sendQueue.Count == 0) return;
                    env = _sendQueue.Dequeue();
                }
                SendAnalyticsResult(env);
            }
        }

        private static void SendAnalyticsResult(HttpEnvelope env)
        {
            byte[] payload = Encoding.UTF8.GetBytes(JsonConvert.SerializeObject(env));

            // ZMQ result plane: PUSH the same JSON instead of HTTP POST. No
            // per-channel URL needed; the recorder demuxes by port_num in the JSON.
            if (_zmqResultSender != null)
            {
                if (_zmqResultSender.Send(payload))
                    Console.WriteLine("Detected!! send analytics result via ZMQ!!");
                else
                    Console.WriteLine($"ZMQ result send timed out (no consumer?) ch{env.port_num}");
                return;
            }

            ChannelHandle channel;
            if (!_byChannelId.TryGetValue(env.port_num, out channel))
                return;

            string url = channel.Parameters.Url;
            if (string.IsNullOrEmpty(url))
            {
                Console.WriteLine($"No analytics URL for ch{env.port_num} yet - dropping result");
                return;
            }

            _postClient.PostAsync(url, payload).ContinueWith(t =>
            {
                if (t.IsFaulted)
                    Console.WriteLine($"Response: {t.Exception.GetBaseException().Message}");
                else
                    Console.WriteLine("Detected!! send analytics result to server!!");
            });
        }

        // ── GDI+ YUV420 → JPEG ────────────────────────────────────────────────
        // Slow (per-pixel SetPixel) but dependency-free; good enough for the
        // sample's low callback rate. The production wrapper uses turbojpeg
        // instead (GenericAI/GenericAI.App/Pipeline/EncodeWorker.cs).

        public static Bitmap ConvertYUV420ToBitmap(byte[] yuvBytes, int width, int height)
        {
            int frameSize  = width * height;
            int chromaSize = frameSize / 4;
            Bitmap bmp = new Bitmap(width, height, PixelFormat.Format24bppRgb);

            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                {
                    int yIndex = y * width + x;
                    int uIndex = (y / 2) * (width / 2) + (x / 2) + frameSize;
                    int vIndex = (y / 2) * (width / 2) + (x / 2) + frameSize + chromaSize;

                    int Y = yuvBytes[yIndex] & 0xFF;
                    int U = yuvBytes[uIndex] & 0xFF;
                    int V = yuvBytes[vIndex] & 0xFF;

                    int C = Y - 16, D = U - 128, E = V - 128;
                    int R = Clip((298 * C + 409 * E + 128) >> 8);
                    int G = Clip((298 * C - 100 * D - 208 * E + 128) >> 8);
                    int B = Clip((298 * C + 516 * D + 128) >> 8);

                    bmp.SetPixel(x, y, Color.FromArgb(R, G, B));
                }
            }
            return bmp;
        }

        private static int Clip(int v) => v < 0 ? 0 : v > 255 ? 255 : v;

        private static ImageCodecInfo GetEncoder(ImageFormat fmt)
        {
            foreach (ImageCodecInfo c in ImageCodecInfo.GetImageDecoders())
                if (c.FormatID == fmt.Guid) return c;
            return null;
        }

        public static byte[] ConvertYUV420ToJpeg(IntPtr yuvFrame, int imageSize,
            int width, int height, long quality)
        {
            byte[] yuvBytes = new byte[imageSize];
            Marshal.Copy(yuvFrame, yuvBytes, 0, imageSize);

            using (Bitmap bmp = ConvertYUV420ToBitmap(yuvBytes, width, height))
            using (MemoryStream ms = new MemoryStream())
            {
                ImageCodecInfo jpgEncoder = GetEncoder(ImageFormat.Jpeg);
                EncoderParameters ep = new EncoderParameters(1);
                ep.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, quality);
                bmp.Save(ms, jpgEncoder, ep);
                return ms.ToArray();
            }
        }

        private static void Cleanup()
        {
            if (Interlocked.Exchange(ref s_cleanupRun, 1) != 0) return;
            try
            {
                try { _shutdownCts?.Cancel(); } catch { }

                if (_channels != null)
                {
                    foreach (ChannelHandle h in _channels)
                    {
                        try { h.StopListener(); } catch { }
                    }
                }

                try { _sendTimer?.Dispose(); } catch { }
                // Send timer stopped -> no more result sends; close the ZMQ socket
                // (LINGER=0, so this never blocks).
                try { _zmqResultSender?.Dispose(); } catch { }

                // Deinitialize stops the ZMQ frame receiver first, then joins the
                // native worker threads, so no more callbacks can arrive once the
                // pointer is cleared below.
                try { NativeInterop.GAI_Deinitialize(); } catch { }

                if (_callbackRegistered)
                {
                    try { NativeInterop.GAI_RegisterCallback(null); } catch { }
                    _callbackRegistered = false;
                }
                try { if (_callbackHandle.IsAllocated) _callbackHandle.Free(); } catch { }
            }
            catch { }
        }
    }
}
