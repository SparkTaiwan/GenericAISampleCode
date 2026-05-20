using System;
using System.Collections.Concurrent;
using System.Net;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;

namespace SampleWrapper
{
    /// <summary>
    /// Single-mode HTTP server.
    ///
    /// Base port  — accepts:  POST /OpenChannel, POST /CloseChannel, GET /Alive, GET /GetLicense
    /// Per-channel — accepts: POST /SetParameters  (added dynamically when a channel is opened)
    /// </summary>
    public class SimpleHttpServer
    {
        private readonly int       _basePort;
        private HttpListener       _baseListener;
        private bool               _running = false;

        // Per-channel listeners  key = channel port
        private readonly ConcurrentDictionary<int, HttpListener> _channelListeners
            = new ConcurrentDictionary<int, HttpListener>();

        // Per-channel dedicated threads (one per channel, owns its HttpListener.GetContext call)
        private readonly ConcurrentDictionary<int, Thread> _channelThreads
            = new ConcurrentDictionary<int, Thread>();

        // Per-channel SetParameters callback  key = channel port
        private readonly ConcurrentDictionary<int, Action<int, SettingParameters>> _channelHandlers
            = new ConcurrentDictionary<int, Action<int, SettingParameters>>();

        // Events fired when ARGO sends management messages
        public event Action<int, string>                    OnOpenChannel;   // (channelPort, mmfName)
        public event Action<int>                            OnCloseChannel;  // (channelPort)

        public SimpleHttpServer(int basePort)
        {
            _basePort = basePort;
        }

        // ── Public channel lifecycle ──────────────────────────────────────────────────────────

        /// <summary>
        /// Adds a per-channel HTTP listener on <paramref name="channelPort"/>.
        /// A dedicated background thread owns the blocking GetContext() call so this
        /// channel's accept loop is completely independent of every other channel.
        /// </summary>
        public void AddChannelPort(int channelPort,
            Action<int, SettingParameters> onSetParameters)
        {
            // Always register the handler — HandleBaseRequest uses it for channel 0
            // (whose portnum == _basePort) and HandleChannelRequest uses it for others.
            _channelHandlers[channelPort] = onSetParameters;

            // Channel 0's portnum == base port: the base listener already owns that port.
            // Creating a second HttpListener on the same port would throw.
            if (channelPort == _basePort)
            {
                Console.WriteLine($"[Server] Ch{channelPort} == base port: handler registered, base listener handles /SetParameters");
                return;
            }

            if (_channelListeners.ContainsKey(channelPort))
            {
                Console.WriteLine($"[Server] Channel port {channelPort} already registered");
                return;
            }

            var listener = new HttpListener();
            listener.Prefixes.Add($"http://localhost:{channelPort}/");
            listener.Prefixes.Add($"http://127.0.0.1:{channelPort}/");
            Console.WriteLine($"[Server] Ch{channelPort} starting listener on prefixes: http://localhost:{channelPort}/ + http://127.0.0.1:{channelPort}/");
            listener.Start();
            Console.WriteLine($"[Server] Ch{channelPort} listener.Start() OK — IsListening={listener.IsListening}");
            _channelListeners[channelPort] = listener;

            // Each channel gets its own dedicated thread — channels never block each other.
            var thread = new Thread(() => ChannelListenThreadProc(channelPort, listener))
            {
                IsBackground = true,
                Name = $"ChannelListener_{channelPort}"
            };
            _channelThreads[channelPort] = thread;
            thread.Start();

            Console.WriteLine($"[Server] Ch{channelPort} per-channel listener started");
        }

        /// <summary>Stops and removes the per-channel HTTP listener and its thread.</summary>
        public void RemoveChannelPort(int channelPort)
        {
            if (_channelListeners.TryRemove(channelPort, out HttpListener listener))
            {
                try { listener.Stop(); listener.Close(); } catch { }
            }
            _channelHandlers.TryRemove(channelPort, out _);
            _channelThreads.TryRemove(channelPort, out _);   // thread exits when listener stops
            Console.WriteLine($"[Server] Removed per-channel listener on port {channelPort}");
        }

        // ── Base port listener ────────────────────────────────────────────────────────────────

        public async Task StartAsync()
        {
            _baseListener = new HttpListener();
            _baseListener.Prefixes.Add($"http://localhost:{_basePort}/");
            _baseListener.Prefixes.Add($"http://127.0.0.1:{_basePort}/");
            _baseListener.Start();
            _running = true;
            Console.WriteLine($"[Server] Base listener started — prefixes: http://localhost:{_basePort}/ + http://127.0.0.1:{_basePort}/");

            while (_running)
            {
                try
                {
                    HttpListenerContext ctx = await _baseListener.GetContextAsync();
                    _ = HandleBaseRequest(ctx);
                }
                catch (HttpListenerException) { break; }
                catch (ObjectDisposedException) { break; }
                catch (Exception ex)
                {
                    Console.WriteLine($"[Server] Base listener error: {ex.Message}");
                }
            }
        }

        private async Task HandleBaseRequest(HttpListenerContext ctx)
        {
            string method = ctx.Request.HttpMethod.ToUpper();
            string path   = ctx.Request.Url.AbsolutePath.TrimEnd('/');
            Console.WriteLine($"[Server] BASE  {method} {path}  from {ctx.Request.RemoteEndPoint}  host={ctx.Request.Headers["Host"]}");

            try
            {
                // ── Heartbeat / license ────────────────────────────────────────────────────
                if (method == "GET" && (path == "/Alive" || path == "/alive"))
                {
                    await Respond(ctx, 200, "OK");
                    return;
                }

                if (method == "GET" && (path == "/GetLicense" || path == "/getlicense"))
                {
                    await Respond(ctx, 200, "{\"license\":\"valid\"}");
                    return;
                }

                // ── Channel management ─────────────────────────────────────────────────────
                if (method == "POST" && (path == "/OpenChannel" || path == "/openchannel"))
                {
                    string body = await ReadBody(ctx);
                    Console.WriteLine($"[Server] /OpenChannel body: {body}");
                    dynamic json = JsonConvert.DeserializeObject<dynamic>(body);

                    int    channelPort = (int)json.port_num;
                    string mmfName     = json.mmf_name != null ? (string)json.mmf_name
                                        : $"ChannelFrame_{channelPort}";

                    Console.WriteLine($"[Server] /OpenChannel  port={channelPort} mmf={mmfName} — invoking handler");
                    OnOpenChannel?.Invoke(channelPort, mmfName);
                    Console.WriteLine($"[Server] /OpenChannel  port={channelPort} — handler done, sending 200");
                    await Respond(ctx, 200, "{\"result\":\"ok\"}");
                    return;
                }

                // Channel 0's portnum == base port, so /SetParameters for channel 0
                // arrives at this listener instead of a per-channel listener.
                if (method == "POST" && (path == "/SetParameters" || path == "/setparameters"))
                {
                    await ApplySetParameters(ctx, _basePort);
                    return;
                }

                if (method == "POST" && (path == "/CloseChannel" || path == "/closechannel"))
                {
                    string body = await ReadBody(ctx);
                    dynamic json = JsonConvert.DeserializeObject<dynamic>(body);

                    // channel_id may come as a string "51001" or as an int 51001
                    int channelPort = int.Parse((string)json.channel_id.ToString());

                    Console.WriteLine($"[Server] /CloseChannel port={channelPort}");
                    OnCloseChannel?.Invoke(channelPort);
                    await Respond(ctx, 200, "{\"result\":\"ok\"}");
                    return;
                }

                await Respond(ctx, 404, "Not found");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Server] ERROR handling base request {path}: {ex}");
                try { await Respond(ctx, 500, "Internal server error"); } catch { }
            }
        }

        // ── Per-channel dedicated-thread listener ─────────────────────────────────────────────
        //
        // Runs on its own Thread (not the async thread pool), so:
        //   - GetContext() blocks only this thread, not any other channel's thread.
        //   - Each incoming request is dispatched to the thread pool via fire-and-forget Task
        //     so the accept loop can immediately accept the next request.

        private void ChannelListenThreadProc(int channelPort, HttpListener listener)
        {
            Console.WriteLine($"[Server] Ch{channelPort} listen thread started — IsListening={listener.IsListening}");
            while (listener.IsListening)
            {
                try
                {
                    HttpListenerContext ctx = listener.GetContext();   // blocking — this thread only
                    _ = HandleChannelRequest(ctx, channelPort);        // handle async on thread pool
                }
                catch (HttpListenerException hex) { Console.WriteLine($"[Server] Ch{channelPort} HttpListenerException: {hex.ErrorCode} {hex.Message}"); break; }
                catch (ObjectDisposedException) { break; }
                catch (Exception ex)
                {
                    Console.WriteLine($"[Server] Ch{channelPort} listener error: {ex}");
                }
            }
            Console.WriteLine($"[Server] Channel {channelPort} listener thread exited");
        }

        private async Task HandleChannelRequest(HttpListenerContext ctx, int channelPort)
        {
            string method = ctx.Request.HttpMethod.ToUpper();
            string path   = ctx.Request.Url.AbsolutePath.TrimEnd('/');
            Console.WriteLine($"[Server] CH{channelPort} {method} {path}  from {ctx.Request.RemoteEndPoint}  host={ctx.Request.Headers["Host"]}");

            try
            {
                if (method == "POST" && (path == "/SetParameters" || path == "/setparameters"))
                {
                    await ApplySetParameters(ctx, channelPort);
                    return;
                }

                if (method == "GET" && (path == "/Alive" || path == "/alive"))
                {
                    await Respond(ctx, 200, "OK");
                    return;
                }

                await Respond(ctx, 404, "Not found");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[Server] ERROR on channel {channelPort} request {path}: {ex}");
                try { await Respond(ctx, 500, "Internal server error"); } catch { }
            }
        }

        // ── Shared SetParameters parser ───────────────────────────────────────────────────────

        private async Task ApplySetParameters(HttpListenerContext ctx, int channelPort)
        {
            string body = await ReadBody(ctx);
            Console.WriteLine($"[Server] Ch{channelPort} /SetParameters body: {body}");

            dynamic jsonData = JsonConvert.DeserializeObject<dynamic>(body);

            var sp = new SettingParameters
            {
                version                  = jsonData.version       != null ? (string)jsonData.version       : "1.3",
                analytics_event_api_url  = jsonData.analytics_event_api_url != null ? (string)jsonData.analytics_event_api_url : "",
                image_width              = jsonData.image_width    != null ? (int)jsonData.image_width    : 0,
                image_height             = jsonData.image_height   != null ? (int)jsonData.image_height   : 0,
                jpg_compress             = jsonData.jpg_compress   != null ? (int)jsonData.jpg_compress   : 50,
                sensitivity              = new int[10],
                threshold                = new int[10],
                rois                     = Program.InitializeRoisArray(100),
                mode                     = jsonData.mode           != null ? (string)jsonData.mode        : "single",
                channel_id               = jsonData.channel_id     != null ? (string)jsonData.channel_id  : "",
                count_reset              = jsonData.count_reset    != null ? (string)jsonData.count_reset : "session",
                ai_settings_json         = jsonData.ai_settings    != null
                                            ? JsonConvert.SerializeObject(jsonData.ai_settings) : ""
            };

            // ARGO format (v1.2+): rois[i] = { sensitivity, threshold, rects:[{x,y}...] }
            // Legacy format:       top-level sensitivity[] + threshold[] arrays, rois[i].points
            if (jsonData.rois != null)
            {
                int roiIdx = 0;
                foreach (var roi in jsonData.rois)
                {
                    if (roiIdx >= 10) break;
                    if (roi.sensitivity != null) sp.sensitivity[roiIdx] = (int)roi.sensitivity;
                    if (roi.threshold   != null) sp.threshold[roiIdx]   = (int)roi.threshold;
                    var pts = roi.rects ?? roi.points;
                    if (pts != null)
                    {
                        int ptIdx = 0;
                        foreach (var pt in pts)
                        {
                            if (ptIdx >= 10) break;
                            sp.rois[roiIdx * 10 + ptIdx] = new ROI { x = (int)pt.x, y = (int)pt.y };
                            ptIdx++;
                        }
                    }
                    roiIdx++;
                }
            }

            if (jsonData.sensitivity is Newtonsoft.Json.Linq.JArray)
                for (int i = 0; i < 10 && i < jsonData.sensitivity.Count; i++)
                    sp.sensitivity[i] = (int)jsonData.sensitivity[i];

            if (jsonData.threshold is Newtonsoft.Json.Linq.JArray)
                for (int i = 0; i < 10 && i < jsonData.threshold.Count; i++)
                    sp.threshold[i] = (int)jsonData.threshold[i];

            if (_channelHandlers.TryGetValue(channelPort, out Action<int, SettingParameters> handler))
            {
                Console.WriteLine($"[Server] Ch{channelPort} /SetParameters — handler found, invoking");
                handler(channelPort, sp);
                Console.WriteLine($"[Server] Ch{channelPort} /SetParameters — handler done");
            }
            else
            {
                Console.WriteLine($"[Server] Ch{channelPort} /SetParameters — WARNING: no handler! registered: [{string.Join(",", _channelHandlers.Keys)}]");
            }

            await Respond(ctx, 200, "{\"result\":\"ok\"}");
        }

        // ── Helpers ───────────────────────────────────────────────────────────────────────────

        private static async Task<string> ReadBody(HttpListenerContext ctx)
        {
            using (var reader = new System.IO.StreamReader(ctx.Request.InputStream,
                ctx.Request.ContentEncoding ?? Encoding.UTF8))
                return await reader.ReadToEndAsync();
        }

        private static async Task Respond(HttpListenerContext ctx, int statusCode, string body)
        {
            ctx.Response.StatusCode = statusCode;
            ctx.Response.ContentType = "application/json";
            byte[] data = Encoding.UTF8.GetBytes(body);
            ctx.Response.ContentLength64 = data.Length;
            await ctx.Response.OutputStream.WriteAsync(data, 0, data.Length);
            ctx.Response.OutputStream.Close();
        }
    }
}
