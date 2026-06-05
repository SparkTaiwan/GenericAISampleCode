using System;
using System.Net;
using System.Text;
using System.Threading.Tasks;
using System.IO;
using Newtonsoft.Json;

namespace SampleWrapper
{

    public class SimpleHttpServer
    {
        private const long MaxRequestBodyBytes = 1L * 1024 * 1024;

        private readonly HttpListener _listener;

        public SimpleHttpServer(string[] prefixes)
        {
            _listener = new HttpListener();
            foreach (string prefix in prefixes)
            {
                _listener.Prefixes.Add(prefix);
            }
        }

        public async Task StartAsync()
        {
            FileLogger.Info($"HttpListener.Start before (prefixes={string.Join(",", _listener.Prefixes)})");
            _listener.Start();
            Console.WriteLine("HTTP Server started.");
            FileLogger.Info("HTTP Server started");

            while (true)
            {
                HttpListenerContext context = null;
                try
                {
                    context = await _listener.GetContextAsync();
                    await HandleRequestAsync(context);
                }
                catch (Exception ex)
                {
                    FileLogger.Error("HTTP server loop error", ex);
                    try { context?.Response?.OutputStream?.Close(); } catch { }
                    // Back off so a listener that's stuck in an unrecoverable state doesn't tight-loop
                    // and drown out real errors in error.log.
                    await Task.Delay(500);
                }
            }
        }

        private async Task HandleRequestAsync(HttpListenerContext context)
        {
            HttpListenerRequest request = context.Request;
            HttpListenerResponse response = context.Response;
            string remote = request.RemoteEndPoint != null ? request.RemoteEndPoint.ToString() : "?";

            try
            {
                if (request.HttpMethod == "POST" && request.Url.AbsolutePath == "/SetParameters")
                {
                    await HandleSetParametersAsync(request, response, remote);
                }
                else if (request.HttpMethod == "GET" && request.Url.AbsolutePath == "/Alive")
                {
                    await WriteResponseAsync(response, 200, "text/plain", "");
                    FileLogger.Info($"Alive received from {remote}");
                }
                else if (request.HttpMethod == "GET" && request.Url.AbsolutePath == "/GetLicense")
                {
                    // should add code to check license is exist.
                    await WriteResponseAsync(response, 200, "text/plain", "");
                    FileLogger.Info($"GetLicense received from {remote}");
                }
                else
                {
                    await WriteResponseAsync(response, 404, "text/plain", "Not Found");
                }
            }
            finally
            {
                try { response.OutputStream.Close(); } catch { }
            }
        }

        private async Task HandleSetParametersAsync(HttpListenerRequest request, HttpListenerResponse response, string remote)
        {
            // Reject oversized bodies before reading them - this is a localhost endpoint but a
            // misbehaving local client could still drive us OOM with a multi-GB body.
            if (request.ContentLength64 > MaxRequestBodyBytes)
            {
                FileLogger.Warn($"SetParameters from {remote} rejected: body {request.ContentLength64} bytes > limit {MaxRequestBodyBytes}");
                await WriteResponseAsync(response, 413, "text/plain", "Payload Too Large");
                return;
            }

            string requestBody;
            try
            {
                using (StreamReader reader = new StreamReader(request.InputStream, request.ContentEncoding))
                {
                    requestBody = await reader.ReadToEndAsync();
                }
            }
            catch (Exception ex)
            {
                FileLogger.Error($"SetParameters from {remote} body read failed", ex);
                await WriteResponseAsync(response, 400, "text/plain", "Bad Request: failed to read body");
                return;
            }

            Console.WriteLine($"Received SetParameters request: {requestBody}");

            SettingParameters settings;
            int roiGroups;
            try
            {
                settings = ParseSettings(requestBody, out roiGroups);
            }
            catch (Exception ex)
            {
                FileLogger.Error($"SetParameters from {remote} parse failed", ex);
                await WriteResponseAsync(response, 400, "text/plain", "Bad Request: " + ex.Message);
                return;
            }

            try
            {
                Program.ApplyParameters(settings);
            }
            catch (Exception ex)
            {
                FileLogger.Error($"SetParameters from {remote} apply failed", ex);
                await WriteResponseAsync(response, 500, "text/plain", "Apply failed: " + ex.Message);
                return;
            }

            string okJson = JsonConvert.SerializeObject(new { message = "Parameters set successfully" });
            await WriteResponseAsync(response, 200, "application/json", okJson);

            FileLogger.Info($"SetParameters received from {remote} (url={settings.analytics_event_api_url}, " +
                            $"w={settings.image_width}, h={settings.image_height}, " +
                            $"jpg={settings.jpg_compress}, roi_groups={roiGroups})");
        }

        private static SettingParameters ParseSettings(string requestBody, out int roiGroups)
        {
            dynamic jsonData = JsonConvert.DeserializeObject<dynamic>(requestBody);

            SettingParameters settings = new SettingParameters
            {
                analytics_event_api_url = (string)jsonData.analytics_event_api_url,
                image_width = (int)jsonData.image_width,
                image_height = (int)jsonData.image_height,
                jpg_compress = (int)jsonData.jpg_compress,
                sensitivity = new int[10],
                threshold = new int[10],
                rois = Program.InitializeRoisArray(100)
            };

            var jsonRois = jsonData.rois;
            roiGroups = 0;
            for (int i = 0; i < jsonRois.Count && i < 10; i++) // Max 10 groups
            {
                settings.sensitivity[i] = (int)jsonRois[i].sensitivity;
                settings.threshold[i] = (int)jsonRois[i].threshold;

                var rects = jsonRois[i].rects.ToObject<ROI[]>();
                for (int j = 0; j < rects.Length && j < 10; j++) // Max 10 rects per group
                {
                    settings.rois[i * 10 + j] = rects[j]; // Flatten into 1D array
                }
                roiGroups++;
            }

            return settings;
        }

        // Builds the entire response body in memory before committing status/headers, so an
        // exception mid-write can't leave the client with a half-sent body and a successful
        // status line.
        private static async Task WriteResponseAsync(HttpListenerResponse response, int statusCode, string contentType, string body)
        {
            byte[] buffer = Encoding.UTF8.GetBytes(body ?? "");
            response.StatusCode = statusCode;
            response.ContentType = contentType;
            response.ContentLength64 = buffer.Length;
            await response.OutputStream.WriteAsync(buffer, 0, buffer.Length);
        }

        public void Stop()
        {
            _listener.Stop();
            Console.WriteLine("HTTP Server stopped.");
        }
    }
}
