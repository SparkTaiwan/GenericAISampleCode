using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace GenericAI.App
{
    // HTTP control plane. Endpoints match
    // spark.recorder/modules/AIService/Generic/AStreamPerimeter_GenericAI.cpp
    // exactly so the recorder side stays unchanged:
    //   GET  /Alive              -> {"status":"ok","version":"1.3"}
    //   GET  /GetLicense
    //   GET  /GetSettingsSchema  -> the motion settings schema (spec §5)
    //   POST /SetParameters      -> v1.3 JSON: {version, analytics_event_api_url,
    //                                   image_width, image_height,
    //                                   ai_settings: {jpg_compress, sensitivity,
    //                                       threshold, trigger_interval},
    //                                   rois: [{sensitivity, threshold, rects:[{x,y}]}]}
    //                              (v1.2 legacy: jpg_compress/sensitivity/threshold at
    //                               top level / in rois; both accepted)
    internal sealed class HttpListenerHost
    {
        private const long MaxRequestBodyBytes = 1L * 1024 * 1024;

        // Cap on concurrent in-flight handlers per listener. /SetParameters
        // and /Alive are both very low frequency in normal operation; this
        // exists purely so a misbehaving recorder cannot flood the thread
        // pool via a retry storm.
        private const int MaxConcurrentHandlers = 8;

        private readonly int _port;
        private readonly string _host;
        private readonly ParameterStore _params;
        private readonly HttpListener _listener;
        private readonly SemaphoreSlim _handlerSemaphore =
            new SemaphoreSlim(MaxConcurrentHandlers, MaxConcurrentHandlers);

        public HttpListenerHost(int port, ParameterStore parameters, string host = "127.0.0.1")
        {
            _port = port;
            _host = string.IsNullOrEmpty(host) ? "127.0.0.1" : host;
            _params = parameters;
            _listener = new HttpListener();
            _listener.Prefixes.Add($"http://{_host}:{port}/");
        }

        public bool Start()
        {
            try
            {
                _listener.Start();
                Console.WriteLine($"HTTP Server started: http://{_host}:{_port}/");
                return true;
            }
            catch (HttpListenerException ex)
            {
                // code 5 = Access denied (needs admin or a URL reservation for a
                // non-loopback host); code 32/183 = address already in use.
                Console.Error.WriteLine($"HttpListener.Start failed on http://{_host}:{_port}/: {ex.Message} (code {ex.ErrorCode})");
                if (ex.ErrorCode == 5)
                    Console.Error.WriteLine($"  -> run as Administrator, or: netsh http add urlacl url=http://{_host}:{_port}/ user=Everyone");
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
                        Console.Error.WriteLine($"HTTP listener loop error: {ex.Message}");
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
                        // The process is alive (HTTP 200); the body reports health:
                        //   {"status":"ok","version":"1.3"}                         -> functional
                        //   {"status":"error","version":"1.3","message":"<reason>"} -> degraded
                        // The recorder reads `status` to surface errors instead of
                        // restarting us, and `version` to learn which protocol level to
                        // speak when it POSTs /SetParameters. Always present so it is
                        // available even in the degraded state.
                        string versionField = "\"version\":" + JsonConvert.SerializeObject(Protocol.Version);
                        string aliveBody = HealthState.IsHealthy
                            ? "{\"status\":\"ok\"," + versionField + "}"
                            : "{\"status\":\"error\"," + versionField + ",\"message\":" + JsonConvert.SerializeObject(HealthState.Error) + "}";
                        await Write(resp, 200, "application/json", aliveBody);
                    }
                    else if (req.HttpMethod == "GET" && req.Url.AbsolutePath == "/GetLicense")
                    {
                        await Write(resp, 200, "text/plain", "");
                    }
                    else if (req.HttpMethod == "GET" && req.Url.AbsolutePath == "/GetSettingsSchema")
                    {
                        // Self-describing AI-settings schema (spec §5). The ConfigClient
                        // GETs this to render the dynamic settings UI; the filled values
                        // come back verbatim in SetParameters.ai_settings.
                        string schema = SettingsSchema.Json;
                        await Write(resp, 200, "application/json", schema);
                        Console.WriteLine($"/GetSettingsSchema from {remote} -> 200 ({schema.Length} bytes)");
                    }
                    else
                    {
                        await Write(resp, 404, "text/plain", "Not Found");
                    }
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine($"HTTP handler error from {remote}: {ex.Message}");
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
                Console.WriteLine($"SetParameters from {remote} rejected: body {req.ContentLength64} > {MaxRequestBodyBytes}");
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
                Console.WriteLine($"SetParameters from {remote} body read failed: {ex.Message}");
                await Write(resp, 400, "text/plain", "Bad Request: failed to read body");
                return;
            }
            if (body == null)
            {
                Console.WriteLine($"SetParameters from {remote} rejected: streamed body exceeds {MaxRequestBodyBytes}");
                await Write(resp, 413, "text/plain", "Payload Too Large");
                return;
            }

            Console.WriteLine($"Received SetParameters request: {body}");

            // v1.3 carries the dynamic settings (filled from the /GetSettingsSchema UI)
            // in ai_settings. Parse them out so we can apply the motion tunables and read
            // jpg_compress / trigger_interval from the schema. v1.2 has no ai_settings.
            string version = null;
            int? jpgFromSchema = null, triggerIntervalFromSchema = null;
            int? sensitivityFromSchema = null, thresholdFromSchema = null;
            try
            {
                var root = JObject.Parse(body);
                version = (string)root["version"];
                var aiSettings = root["ai_settings"];
                if (aiSettings != null)
                {
                    Console.WriteLine($"ai_settings received from {remote}: {aiSettings.ToString(Formatting.None)}");
                    jpgFromSchema             = ExtractInt(aiSettings, "jpg_compress");
                    triggerIntervalFromSchema = ExtractInt(aiSettings, "trigger_interval");
                    sensitivityFromSchema     = ExtractInt(aiSettings, "sensitivity");
                    thresholdFromSchema       = ExtractInt(aiSettings, "threshold");
                }
            }
            catch { /* body already logged above; ignore */ }

            NativeInterop.SettingParameters settings;
            try
            {
                settings = ParseSettings(body, version);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"SetParameters from {remote} parse failed: {ex.Message}");
                await Write(resp, 400, "text/plain", "Bad Request: " + ex.Message);
                return;
            }

            // v1.3 sources jpg_compress from the schema; v1.2 keeps the legacy top-level
            // value already parsed into settings.jpg_compress.
            if (version == "1.3" && jpgFromSchema.HasValue)
                settings.jpg_compress = jpgFromSchema.Value;

            // Motion tunables from the schema take precedence over the legacy rois[]
            // values -- the schema is the single global sensitivity/threshold for this
            // motion-only wrapper, applied to every ROI group.
            if (sensitivityFromSchema.HasValue)
                for (int i = 0; i < settings.sensitivity.Length; i++) settings.sensitivity[i] = sensitivityFromSchema.Value;
            if (thresholdFromSchema.HasValue)
                for (int i = 0; i < settings.threshold.Length; i++) settings.threshold[i] = thresholdFromSchema.Value;

            try
            {
                _params.Update(settings.analytics_event_api_url, settings.jpg_compress, triggerIntervalFromSchema);
                NativeInterop.GAI_SetChannelParameters(_port, ref settings);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"SetParameters from {remote} failed: {ex.Message}");
                await Write(resp, 500, "text/plain", "SetParameters failed: " + ex.Message);
                return;
            }

            string okJson = JsonConvert.SerializeObject(new { message = "Parameters set successfully" });
            await Write(resp, 200, "application/json", okJson);
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

        // A scalar int field by key from ai_settings. Accepts flat ({"sensitivity":50})
        // or schema-shaped ({"fields":[{"key":"sensitivity","value":50}]}). null if absent.
        private static int? ExtractInt(JToken aiSettings, string key)
        {
            try
            {
                JToken flat = aiSettings[key];
                if (flat != null && flat.Type != JTokenType.Object && flat.Type != JTokenType.Array)
                    return (int)flat;

                if (aiSettings["fields"] is JArray fields)
                {
                    foreach (JToken f in fields)
                    {
                        if ((string)f["key"] == key)
                        {
                            JToken v = f["value"] ?? f["default"];
                            if (v != null && v.Type != JTokenType.Object && v.Type != JTokenType.Array)
                                return (int)v;
                        }
                    }
                }
            }
            catch { }
            return null;
        }

        private static NativeInterop.SettingParameters ParseSettings(string body, string version)
        {
            SetParametersDto data = JsonConvert.DeserializeObject<SetParametersDto>(body);
            if (data == null) throw new JsonException("request body is empty or null");
            if (data.image_width  == null) throw new JsonException("image_width is required");
            if (data.image_height == null) throw new JsonException("image_height is required");
            // v1.3 carries jpg_compress inside ai_settings (schema); only the legacy
            // v1.2 top-level field is required here.
            if (version != "1.3" && data.jpg_compress == null)
                throw new JsonException("jpg_compress is required");

            NativeInterop.SettingParameters s = new NativeInterop.SettingParameters
            {
                version = Protocol.Version,
                analytics_event_api_url = data.analytics_event_api_url ?? "",
                image_width = data.image_width.Value,
                image_height = data.image_height.Value,
                jpg_compress = data.jpg_compress ?? 0,
                sensitivity = new int[10],
                threshold = new int[10],
                rois = InitializeRoisArray(100),
            };

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
        // Nullable ints so a missing field deserializes to null (and we can
        // 400 on it) rather than silently defaulting to 0.
        private sealed class SetParametersDto
        {
            public string version { get; set; }
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
