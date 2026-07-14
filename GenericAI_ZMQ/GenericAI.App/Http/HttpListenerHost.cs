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
        private readonly string _host;
        private readonly ParameterStore _params;
        private readonly HttpListener _listener;
        private readonly SemaphoreSlim _handlerSemaphore =
            new SemaphoreSlim(MaxConcurrentHandlers, MaxConcurrentHandlers);

        public HttpListenerHost(int port, ParameterStore parameters, string host)
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
                ConsoleLog.WriteLine($"HTTP Server started: http://{_host}:{_port}/  (reachable for /Alive,/SetParameters)");
                FileLogger.Info($"HTTP listener started on http://{_host}:{_port}/");
                return true;
            }
            catch (HttpListenerException ex)
            {
                // Surface to console -- this is the usual early-exit cause on a
                // fresh machine. code 5 = Access denied (HttpListener needs admin
                // or a URL reservation); code 32/183 = address already in use.
                ConsoleLog.ErrorLine(
                    $"HTTP listener FAILED on http://{_host}:{_port}/ : {ex.Message} (code {ex.ErrorCode})");
                if (ex.ErrorCode == 5)
                {
                    ConsoleLog.ErrorLine(
                        $"  -> Access denied. Run as Administrator, OR reserve the URL once (elevated):");
                    ConsoleLog.ErrorLine(
                        $"     netsh http add urlacl url=http://{_host}:{_port}/ user=Everyone");
                }
                FileLogger.Error($"HttpListener.Start failed on port {_port}", ex);
                return false;
            }
        }

        public void Stop()
        {
            try { _listener.Stop(); } catch { }
            try { _listener.Close(); } catch { }
            ConsoleLog.WriteLine("HTTP Server stopped.");
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
                        // The process is alive (HTTP 200) but the body reports health:
                        //   {"status":"ok","version":"1.3"}                         -> functional
                        //   {"status":"error","version":"1.3","message":"<reason>"} -> degraded
                        //   (e.g. the detector/model failed to load). The recorder reads
                        //   `status` to surface the error instead of pointlessly restarting
                        //   us, and `version` to learn which protocol level to speak when it
                        //   POSTs /SetParameters. Always present so it is available even in
                        //   the degraded state.
                        string versionField = "\"version\":"
                            + JsonConvert.SerializeObject(Protocol.Version);
                        string aliveBody;
                        if (HealthState.IsHealthy)
                        {
                            aliveBody = "{\"status\":\"ok\"," + versionField + "}";
                        }
                        else
                        {
                            aliveBody = "{\"status\":\"error\"," + versionField + ",\"message\":"
                                + JsonConvert.SerializeObject(HealthState.Error) + "}";
                        }
                        await Write(resp, 200, "application/json", aliveBody);
                        ConsoleLog.WriteLine($"/Alive from {remote} -> 200 (healthy={HealthState.IsHealthy})");
                        FileLogger.Info($"Alive received from {remote} (healthy={HealthState.IsHealthy})");
                    }
                    else if (req.HttpMethod == "GET" && req.Url.AbsolutePath == "/GetLicense")
                    {
                        await Write(resp, 200, "text/plain", "");
                        FileLogger.Info($"GetLicense received from {remote}");
                    }
                    else if (req.HttpMethod == "GET" && req.Url.AbsolutePath == "/GetSettingsSchema")
                    {
                        // Self-describing AI-settings schema (spec §5). The ConfigClient
                        // GETs this to render the dynamic settings UI; the filled values
                        // come back verbatim in SetParameters.ai_settings.
                        string schema = SettingsSchema.Json;
                        await Write(resp, 200, "application/json", schema);
                        ConsoleLog.WriteLine($"/GetSettingsSchema from {remote} -> 200 ({schema.Length} bytes)");
                        FileLogger.Info($"GetSettingsSchema served to {remote}");
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

            ConsoleLog.WriteLine($"Received SetParameters request: {body}");

            // Surface ai_settings (dynamic settings from the schema UI) explicitly so
            // we can confirm it arrives. The native detector does not consume it yet —
            // this is the receive side of spec §5.
            // `version` also drives where jpg_compress comes from: v1.3 reads it from
            // ai_settings (schema); v1.2 keeps the legacy top-level field (below).
            string version = null;
            int? jpgFromSchema = null;
            int? triggerIntervalFromSchema = null;
            try
            {
                var root = Newtonsoft.Json.Linq.JObject.Parse(body);
                version = (string)root["version"];
                var aiSettings = root["ai_settings"];
                if (aiSettings != null)
                {
                    string s = aiSettings.ToString(Formatting.None);
                    ConsoleLog.WriteLine($"ai_settings received from {remote}: {s}");
                    FileLogger.Info($"ai_settings received from {remote}: {s}");

                    // Apply the per-channel values to native (spec §5).
                    jpgFromSchema = ExtractInt(aiSettings, "jpg_compress");
                    // Motion-only wrapper-side send throttle (not a native setting): min seconds between
                    // HTTP POSTs. Absent for object detection -> stays null -> ParameterStore unchanged.
                    triggerIntervalFromSchema = ExtractInt(aiSettings, "trigger_interval");
                    ApplyAiSettingsToNative(_port, aiSettings);
                }
            }
            catch { /* body already logged above; ignore */ }

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

            // v1.3 sources jpg_compress from the schema (ai_settings); v1.2 keeps the
            // legacy top-level value already parsed into settings.jpg_compress. When v1.3
            // omits it from the schema, settings.jpg_compress stays 0 and ParameterStore
            // keeps its previous quality.
            if (version == "1.3" && jpgFromSchema.HasValue)
                settings.jpg_compress = jpgFromSchema.Value;

            try
            {
                _params.Update(settings.analytics_event_api_url, settings.jpg_compress, triggerIntervalFromSchema);
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

        // Applies schema-shaped or flat ai_settings to the native detector for one
        // channel (spec §5). Object detection: confidence -> conf_threshold, classes
        // -> supported-class bitmask. Motion: sensitivity + threshold. Keys a detector
        // doesn't use stay absent (passed as <0). Shared by the incoming /SetParameters
        // path and the startup default-seeding (Program.cs), so a channel that never
        // receives ai_settings still follows the built-in default schema.
        public static void ApplyAiSettingsToNative(int port, JToken aiSettings)
        {
            float? conf = ExtractConfidence(aiSettings);
            int? classMask = ExtractClassMask(aiSettings);
            int? sensitivity = ExtractInt(aiSettings, "sensitivity");
            int? threshold = ExtractInt(aiSettings, "threshold");
            // Object detection object-size band (% of frame area, 0..100). Absent -> pass <0
            // (that bound = no filter).
            float? objSizeMin = ExtractFloat(aiSettings, "object_size_min");
            float? objSizeMax = ExtractFloat(aiSettings, "object_size_max");
            if (conf.HasValue || classMask.HasValue || sensitivity.HasValue || threshold.HasValue
                || objSizeMin.HasValue || objSizeMax.HasValue)
            {
                NativeInterop.GAI_SetChannelAiSettings(port,
                    conf ?? -1f, classMask ?? -1, sensitivity ?? -1, threshold ?? -1,
                    objSizeMin ?? -1f, objSizeMax ?? -1f);
                ConsoleLog.WriteLine($"ai_settings applied: ch{port} confidence={(conf.HasValue ? conf.Value.ToString() : "-")}"
                    + $" classMask=0x{(classMask ?? -1):X} sensitivity={(sensitivity?.ToString() ?? "-")} threshold={(threshold?.ToString() ?? "-")}"
                    + $" object_size_min={(objSizeMin?.ToString() ?? "-")} object_size_max={(objSizeMax?.ToString() ?? "-")}");
            }
        }

        // ai_settings may be flat ({"confidence":0.8}) or schema-shaped
        // ({"fields":[{"key":"confidence","value":0.8}]}). Pull confidence either way.
        private static float? ExtractConfidence(JToken aiSettings)
        {
            try
            {
                JToken flat = aiSettings["confidence"];
                if (flat != null && flat.Type != JTokenType.Object && flat.Type != JTokenType.Array)
                    return (float)flat;

                if (aiSettings["fields"] is JArray fields)
                {
                    foreach (JToken f in fields)
                    {
                        if ((string)f["key"] == "confidence")
                        {
                            JToken v = f["value"] ?? f["default"];
                            if (v != null && v.Type != JTokenType.Object && v.Type != JTokenType.Array)
                                return (float)v;
                        }
                    }
                }
            }
            catch { }
            return null;
        }

        // A scalar int field by key. flat ({"sensitivity":50}) or schema-shaped
        // ({"fields":[{"key":"sensitivity","value":50}]}). null if absent.
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

        // A scalar float field by key. flat ({"object_size_min":2.5}) or schema-shaped
        // ({"fields":[{"key":"object_size_min","value":2.5}]}). null if absent.
        private static float? ExtractFloat(JToken aiSettings, string key)
        {
            try
            {
                JToken flat = aiSettings[key];
                if (flat != null && flat.Type != JTokenType.Object && flat.Type != JTokenType.Array)
                    return (float)flat;

                if (aiSettings["fields"] is JArray fields)
                {
                    foreach (JToken f in fields)
                    {
                        if ((string)f["key"] == key)
                        {
                            JToken v = f["value"] ?? f["default"];
                            if (v != null && v.Type != JTokenType.Object && v.Type != JTokenType.Array)
                                return (float)v;
                        }
                    }
                }
            }
            catch { }
            return null;
        }

        // classes string_array -> supported-class bitmask (NativeInterop.SupportedClasses
        // order). flat ({"classes":["person","car"]}) or schema-shaped
        // ({"fields":[{"key":"classes","value":[...]}]}). null if absent.
        private static int? ExtractClassMask(JToken aiSettings)
        {
            try
            {
                JArray arr = aiSettings["classes"] as JArray;
                if (arr == null && aiSettings["fields"] is JArray fields)
                {
                    foreach (JToken f in fields)
                    {
                        if ((string)f["key"] == "classes")
                        {
                            arr = (f["value"] ?? f["default"]) as JArray;
                            break;
                        }
                    }
                }
                if (arr == null) return null;

                int mask = 0;
                foreach (JToken t in arr)
                {
                    string name = (string)t;
                    int idx = Array.IndexOf(NativeInterop.SupportedClasses, name);
                    if (idx >= 0) mask |= (1 << idx);
                }
                return mask;
            }
            catch { }
            return null;
        }

        private static NativeInterop.SettingParameters ParseSettings(string body, out int roiGroups)
        {
            SetParametersDto data = JsonConvert.DeserializeObject<SetParametersDto>(body);
            if (data == null) throw new JsonException("request body is empty or null");
            if (data.image_width  == null) throw new JsonException("image_width is required");
            if (data.image_height == null) throw new JsonException("image_height is required");
            // v1.3 carries jpg_compress inside ai_settings (schema); only the legacy
            // v1.2 top-level field is required here.
            if (data.version != "1.3" && data.jpg_compress == null)
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
