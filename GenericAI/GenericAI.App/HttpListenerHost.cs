using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;

namespace GenericAI.App
{
    // HTTP control plane. Endpoints match
    // spark.recorder/modules/AIService/Generic/AStreamPerimeter_GenericAI.cpp
    // exactly so the recorder side stays unchanged:
    //   GET  /Alive
    //   GET  /GetLicense
    //   POST /SetParameters (v1.2 JSON: {analytics_event_api_url, image_width,
    //                                    image_height, jpg_compress,
    //                                    rois: [{sensitivity, threshold,
    //                                            rects: [{x,y}...]}]})
    internal sealed class HttpListenerHost
    {
        private const long MaxRequestBodyBytes = 1L * 1024 * 1024;

        // Cap on concurrent in-flight handlers per listener. /SetParameters
        // and /Alive are both very low frequency in normal operation; this
        // exists purely so a misbehaving recorder cannot flood the thread
        // pool via a retry storm.
        private const int MaxConcurrentHandlers = 8;

        private readonly int _port;
        private readonly ParameterStore _params;
        private readonly HttpListener _listener;
        private readonly SemaphoreSlim _handlerSemaphore =
            new SemaphoreSlim(MaxConcurrentHandlers, MaxConcurrentHandlers);

        public HttpListenerHost(int port, ParameterStore parameters)
        {
            _port = port;
            _params = parameters;
            _listener = new HttpListener();
            _listener.Prefixes.Add($"http://127.0.0.1:{port}/");
        }

        public bool Start()
        {
            try
            {
                _listener.Start();
                Console.WriteLine("HTTP Server started.");
                FileLogger.Info($"HTTP listener started on port {_port}");
                return true;
            }
            catch (HttpListenerException ex)
            {
                FileLogger.Error($"HttpListener.Start failed on port {_port}", ex);
                return false;
            }
        }

        public void Stop()
        {
            try { _listener.Stop(); } catch { }
            try { _listener.Close(); } catch { }
            Console.WriteLine("HTTP Server stopped.");
        }

        public async Task RunAsync(CancellationToken ct)
        {
            using (ct.Register(() => { try { _listener.Stop(); } catch { } }))
            {
                while (!ct.IsCancellationRequested)
                {
                    HttpListenerContext context;
                    try
                    {
                        context = await _listener.GetContextAsync().ConfigureAwait(false);
                    }
                    catch (HttpListenerException)
                    {
                        break;
                    }
                    catch (ObjectDisposedException)
                    {
                        break;
                    }
                    catch (Exception ex)
                    {
                        FileLogger.Error("HTTP listener loop error", ex);
                        await Task.Delay(500, ct).ConfigureAwait(false);
                        continue;
                    }

                    // Fire and forget — handler manages its own response lifetime.
                    _ = HandleAsync(context);
                }
            }
        }

        private async Task HandleAsync(HttpListenerContext context)
        {
            await _handlerSemaphore.WaitAsync().ConfigureAwait(false);
            try
            {
                HttpListenerRequest req = context.Request;
                HttpListenerResponse resp = context.Response;
                string remote = req.RemoteEndPoint != null ? req.RemoteEndPoint.ToString() : "?";

                try
                {
                    if (req.HttpMethod == "POST" && req.Url.AbsolutePath == "/SetParameters")
                    {
                        await HandleSetParametersAsync(req, resp, remote);
                    }
                    else if (req.HttpMethod == "GET" && req.Url.AbsolutePath == "/Alive")
                    {
                        await Write(resp, 200, "text/plain", "");
                        FileLogger.Info($"Alive received from {remote}");
                    }
                    else if (req.HttpMethod == "GET" && req.Url.AbsolutePath == "/GetLicense")
                    {
                        await Write(resp, 200, "text/plain", "");
                        FileLogger.Info($"GetLicense received from {remote}");
                    }
                    else
                    {
                        await Write(resp, 404, "text/plain", "Not Found");
                    }
                }
                catch (Exception ex)
                {
                    FileLogger.Error($"HTTP handler error from {remote}", ex);
                    try { await Write(resp, 500, "text/plain", "Server Error"); } catch { }
                }
                finally
                {
                    try { resp.OutputStream.Close(); } catch { }
                }
            }
            finally
            {
                _handlerSemaphore.Release();
            }
        }

        private async Task HandleSetParametersAsync(HttpListenerRequest req, HttpListenerResponse resp, string remote)
        {
            if (req.ContentLength64 > MaxRequestBodyBytes)
            {
                FileLogger.Warn($"SetParameters from {remote} rejected: body {req.ContentLength64} > {MaxRequestBodyBytes}");
                await Write(resp, 413, "text/plain", "Payload Too Large");
                return;
            }

            string body;
            try
            {
                // Bounded read: the ContentLength64 check above is only a fast
                // path — a chunked request reports -1 and would bypass it, so
                // the cap has to be enforced while streaming too.
                body = await ReadBodyBoundedAsync(req);
            }
            catch (Exception ex)
            {
                FileLogger.Error($"SetParameters from {remote} body read failed", ex);
                await Write(resp, 400, "text/plain", "Bad Request: failed to read body");
                return;
            }
            if (body == null)
            {
                FileLogger.Warn($"SetParameters from {remote} rejected: streamed body exceeds {MaxRequestBodyBytes}");
                await Write(resp, 413, "text/plain", "Payload Too Large");
                return;
            }

            Console.WriteLine($"Received SetParameters request: {body}");

            NativeInterop.SettingParameters settings;
            int roiGroups;
            try
            {
                settings = ParseSettings(body, out roiGroups);
            }
            catch (Exception ex)
            {
                FileLogger.Error($"SetParameters from {remote} parse failed", ex);
                await Write(resp, 400, "text/plain", "Bad Request: " + ex.Message);
                return;
            }

            try
            {
                _params.Update(settings.analytics_event_api_url, settings.jpg_compress);
                NativeInterop.GAI_SetChannelParameters(_port, ref settings);
            }
            catch (Exception ex)
            {
                FileLogger.Error($"SetParameters from {remote} failed", ex);
                await Write(resp, 500, "text/plain", "SetParameters failed: " + ex.Message);
                return;
            }

            string okJson = JsonConvert.SerializeObject(new { message = "Parameters set successfully" });
            await Write(resp, 200, "application/json", okJson);

            FileLogger.Info($"SetParameters received from {remote} (url={settings.analytics_event_api_url}, " +
                            $"w={settings.image_width}, h={settings.image_height}, " +
                            $"jpg={settings.jpg_compress}, roi_groups={roiGroups})");
        }

        // Reads the request body up to MaxRequestBodyBytes; returns null when
        // the cap is exceeded (caller responds 413).
        private static async Task<string> ReadBodyBoundedAsync(HttpListenerRequest req)
        {
            byte[] buffer = new byte[16 * 1024];
            using (MemoryStream ms = new MemoryStream())
            {
                int read;
                while ((read = await req.InputStream.ReadAsync(buffer, 0, buffer.Length)) > 0)
                {
                    ms.Write(buffer, 0, read);
                    if (ms.Length > MaxRequestBodyBytes) return null;
                }
                return req.ContentEncoding.GetString(ms.GetBuffer(), 0, (int)ms.Length);
            }
        }

        private static NativeInterop.SettingParameters ParseSettings(string body, out int roiGroups)
        {
            SetParametersDto data = JsonConvert.DeserializeObject<SetParametersDto>(body);
            if (data == null) throw new JsonException("request body is empty or null");
            if (data.image_width  == null) throw new JsonException("image_width is required");
            if (data.image_height == null) throw new JsonException("image_height is required");
            if (data.jpg_compress == null) throw new JsonException("jpg_compress is required");

            NativeInterop.SettingParameters s = new NativeInterop.SettingParameters
            {
                version = "1.2",
                analytics_event_api_url = data.analytics_event_api_url ?? "",
                image_width = data.image_width.Value,
                image_height = data.image_height.Value,
                jpg_compress = data.jpg_compress.Value,
                sensitivity = new int[10],
                threshold = new int[10],
                rois = InitializeRoisArray(100),
            };

            roiGroups = 0;
            if (data.rois != null)
            {
                for (int i = 0; i < data.rois.Count && i < 10; i++)
                {
                    RoiGroupDto g = data.rois[i];
                    if (g == null) continue;
                    if (g.sensitivity == null) throw new JsonException($"rois[{i}].sensitivity is required");
                    if (g.threshold   == null) throw new JsonException($"rois[{i}].threshold is required");
                    s.sensitivity[i] = g.sensitivity.Value;
                    s.threshold[i] = g.threshold.Value;

                    if (g.rects != null)
                    {
                        for (int j = 0; j < g.rects.Count && j < 10; j++)
                        {
                            RoiDto r = g.rects[j];
                            if (r == null) continue;
                            NativeInterop.ROI roi;
                            roi.x = r.x;
                            roi.y = r.y;
                            s.rois[i * 10 + j] = roi;
                        }
                    }
                    roiGroups++;
                }
            }

            return s;
        }

        private static NativeInterop.ROI[] InitializeRoisArray(int size)
        {
            NativeInterop.ROI[] a = new NativeInterop.ROI[size];
            for (int i = 0; i < size; i++) { a[i].x = -1; a[i].y = -1; }
            return a;
        }

        // DTOs mirror the /SetParameters wire format (v1.2). Kept private to
        // the listener because they are not shared with the rest of the app —
        // ParseSettings copies the values into NativeInterop.SettingParameters /
        // NativeInterop.ROI which is the canonical in-process representation.
        // Using property-based DTOs (rather than reading NativeInterop.ROI's
        // public fields directly) keeps NativeInterop.ROI free of JSON
        // attributes, so HttpEnvelope.rois_rects's outbound serialization is
        // untouched.
        // Nullable ints so a missing field deserializes to null (and we can
        // 400 on it) rather than silently defaulting to 0 — keeps the
        // "missing required field" behaviour of the previous dynamic path.
        private sealed class SetParametersDto
        {
            public string analytics_event_api_url { get; set; }
            public int? image_width { get; set; }
            public int? image_height { get; set; }
            public int? jpg_compress { get; set; }
            public List<RoiGroupDto> rois { get; set; }
        }

        private sealed class RoiGroupDto
        {
            public int? sensitivity { get; set; }
            public int? threshold { get; set; }
            public List<RoiDto> rects { get; set; }
        }

        private sealed class RoiDto
        {
            public int x { get; set; }
            public int y { get; set; }
        }

        private static async Task Write(HttpListenerResponse resp, int status, string contentType, string body)
        {
            byte[] buf = Encoding.UTF8.GetBytes(body ?? "");
            resp.StatusCode = status;
            resp.ContentType = contentType;
            resp.ContentLength64 = buf.Length;
            await resp.OutputStream.WriteAsync(buf, 0, buf.Length);
        }
    }
}
