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
        private readonly HttpListener _listener;
        private SettingParameters _parameters;
        private bool _updateparams = false ;

        private ROI[] InitializeRoisArray(int size)
        {
            ROI[] rois = new ROI[size];
            for (int i = 0; i < size; i++)
            {
                rois[i] = new ROI { x = -1, y = -1 }; // Default values
            }
            return rois;
        }

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
            _listener.Start();
            Console.WriteLine("HTTP Server started.");

            while (true)
            {
                HttpListenerContext context = await _listener.GetContextAsync();
                HttpListenerRequest request = context.Request;
                HttpListenerResponse response = context.Response;

                if (request.HttpMethod == "POST" && request.Url.AbsolutePath == "/SetParameters")
                {
                    using (StreamReader reader = new StreamReader(request.InputStream, request.ContentEncoding))
                    {
                        string requestBody = await reader.ReadToEndAsync();
                        Console.WriteLine($"Received SetParameters request: {requestBody}");

                        // Deserialize JSON into a dynamic object
                        dynamic jsonData = JsonConvert.DeserializeObject<dynamic>(requestBody);

                        // Initialize SettingParameters structure
                        SettingParameters settings = new SettingParameters
                        {
                            analytics_event_api_url = jsonData.analytics_event_api_url,
                            image_width = (int)jsonData.image_width,
                            image_height = (int)jsonData.image_height,
                            jpg_compress = (int)jsonData.jpg_compress,
                            sensitivity = new int[10],  // Initialize sensitivity array
                            threshold = new int[10],    // Initialize threshold array
                            rois = InitializeRoisArray(100)  // Allocate a 1D array of 100 ROI objects (10x10)
                        };

                        // Populate the sensitivity and threshold arrays and rois array
                        var jsonRois = jsonData.rois;
                        for (int i = 0; i < jsonRois.Count && i < 10; i++) // Max 10 groups
                        {
                            settings.sensitivity[i] = (int)jsonRois[i].sensitivity;
                            settings.threshold[i] = (int)jsonRois[i].threshold;

                            var rects = jsonRois[i].rects.ToObject<ROI[]>();
                            for (int j = 0; j < rects.Length && j < 10; j++) // Max 10 rects per group
                            {
                                settings.rois[i * 10 + j] = rects[j]; // Flatten into 1D array
                            }
                        }

                        _parameters = settings;

                        response.StatusCode = 200;
                        response.ContentType = "application/json";
                        var responseString = JsonConvert.SerializeObject(new { message = "Parameters set successfully" });
                        byte[] buffer = Encoding.UTF8.GetBytes(responseString);
                        response.ContentLength64 = buffer.Length;
                        await response.OutputStream.WriteAsync(buffer, 0, buffer.Length);
                        _updateparams = true;
                    }
                }
                else if (request.HttpMethod == "GET" && request.Url.AbsolutePath == "/Alive")
                {
                    response.StatusCode = 200;
                    response.ContentType = "text/plain";
                    byte[] buffer = Encoding.UTF8.GetBytes("");
                    response.ContentLength64 = buffer.Length;
                    await response.OutputStream.WriteAsync(buffer, 0, buffer.Length);
                }
                else if (request.HttpMethod == "GET" && request.Url.AbsolutePath == "/GetLicense")
                {
                    // should add code to check license is exist.
                    response.StatusCode = 200;
                    response.ContentType = "text/plain";
                    byte[] buffer = Encoding.UTF8.GetBytes("");
                    response.ContentLength64 = buffer.Length;
                    await response.OutputStream.WriteAsync(buffer, 0, buffer.Length);
                }
                else
                {
                    response.StatusCode = 404;
                    byte[] buffer = Encoding.UTF8.GetBytes("Not Found");
                    response.ContentLength64 = buffer.Length;
                    await response.OutputStream.WriteAsync(buffer, 0, buffer.Length);
                }

                response.OutputStream.Close();
            }
        }

        public bool IsUpdateParam()
        {
            return _updateparams;
        }

        public SettingParameters GetParameters()
        {
            _updateparams = false;
            return _parameters;
        }

        public void Stop()
        {
            _listener.Stop();
            Console.WriteLine("HTTP Server stopped.");
        }
    }
}