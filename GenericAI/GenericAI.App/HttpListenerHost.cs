using System;
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

        private readonly int _port;
        private readonly ParameterStore _params;
        private readonly HttpListener _listener;

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
                using (StreamReader sr = new StreamReader(req.InputStream, req.ContentEncoding))
                {
                    body = await sr.ReadToEndAsync();
                }
            }
            catch (Exception ex)
            {
                FileLogger.Error($"SetParameters from {remote} body read failed", ex);
                await Write(resp, 400, "text/plain", "Bad Request: failed to read body");
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

        private static NativeInterop.SettingParameters ParseSettings(string body, out int roiGroups)
        {
            dynamic data = JsonConvert.DeserializeObject<dynamic>(body);

            NativeInterop.SettingParameters s = new NativeInterop.SettingParameters
            {
                version = "1.2",
                analytics_event_api_url = (string)data.analytics_event_api_url ?? "",
                image_width = (int)data.image_width,
                image_height = (int)data.image_height,
                jpg_compress = (int)data.jpg_compress,
                sensitivity = new int[10],
                threshold = new int[10],
                rois = InitializeRoisArray(100),
            };

            var rois = data.rois;
            roiGroups = 0;
            if (rois != null)
            {
                for (int i = 0; i < rois.Count && i < 10; i++)
                {
                    s.sensitivity[i] = (int)rois[i].sensitivity;
                    s.threshold[i] = (int)rois[i].threshold;

                    NativeInterop.ROI[] rects = rois[i].rects.ToObject<NativeInterop.ROI[]>();
                    for (int j = 0; j < rects.Length && j < 10; j++)
                    {
                        s.rois[i * 10 + j] = rects[j];
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
