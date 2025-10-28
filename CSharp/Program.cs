using System;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using Newtonsoft.Json;
using System.Collections.Generic;

namespace SampleWrapper
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct ROI
    {
        public int x;
        public int y;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct SettingParameters
    {

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string version;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string analytics_event_api_url;
        public int image_width;
        public int image_height;
        public int jpg_compress;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 10)]
        public int[] sensitivity;    // Array for 10 sensitivity values

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 10)]
        public int[] threshold;      // Array for 10 threshold values

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 100)]
        public ROI[] rois;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct MMF_Data
    {
        public long header;
        public int image_status;
        public int image_width;
        public int image_height;
        public int image_size;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 1920 * 1080 * 3)]
        public byte[] image_data;

        public ulong timestamp;
        public long footer;
    }
    
    public struct AnalyticsResult
    {
        public string version;
        public int port_num;
        public string keyframe; 
        public ulong timestamp;
        public List<List<ROI>> rois_rects; // Pointer to ROI array
        //public int rois_count;    // Number of ROIs
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    public delegate void CallBackFunction(int channelid , int width, int height, IntPtr imageframe, int image_size, ulong timestamp, IntPtr rois_rects, int rois_count, int node_count);


    public class Program
    {
        private static int m_portnum = 0;
        private static string m_url = "";
        private static int m_jpg_compress = 50;
        private static Queue<AnalyticsResult> httpRequestQueue = new Queue<AnalyticsResult>();
        private static readonly object queueLock = new object();
        private static Timer queueProcessorTimer;
        private static CallBackFunction callbackDelegate;

        [DllImport("SampleDLL.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Initialize(int PortNumber);

        [DllImport("SampleDLL.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void SettingParameters(ref SettingParameters parameters);

        [DllImport("SampleDLL.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void registerCallback(CallBackFunction callbaback);

        [DllImport("SampleDLL.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void unregisterCallback();

        [DllImport("SampleDLL.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Deinitialize();

        //Convert YUV420 to Bitmap
        public static Bitmap ConvertYUV420ToBitmap(byte[] yuvBytes, int width, int height)
        {
            int frameSize = width * height;
            int chromaSize = frameSize / 4;

            if (yuvBytes.Length != frameSize + 2 * chromaSize)
            {
                throw new ArgumentException("Invalid YUV420 data size.");
            }

            Bitmap bmp = new Bitmap(width, height, PixelFormat.Format24bppRgb);

            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                {
                    int yIndex = y * width + x;
                    int uIndex = (y / 2) * (width / 2) + (x / 2) + frameSize;
                    int vIndex = (y / 2) * (width / 2) + (x / 2) + frameSize + chromaSize;

                    if (yIndex >= frameSize || uIndex >= frameSize + chromaSize || vIndex >= frameSize + chromaSize * 2)
                    {
                        throw new IndexOutOfRangeException("Index out of range for YUV data.");
                    }

                    int Y = yuvBytes[yIndex] & 0xFF;
                    int U = yuvBytes[uIndex] & 0xFF;
                    int V = yuvBytes[vIndex] & 0xFF;

                    int C = Y - 16;
                    int D = U - 128;
                    int E = V - 128;

                    int R = Clip((298 * C + 409 * E + 128) >> 8);
                    int G = Clip((298 * C - 100 * D - 208 * E + 128) >> 8);
                    int B = Clip((298 * C + 516 * D + 128) >> 8);

                    bmp.SetPixel(x, y, Color.FromArgb(Clip(R), Clip(G), Clip(B)));
                }
            }

            return bmp;
        }
        
        private static int Clip(int value)
        {
            if (value < 0) return 0;
            if (value > 255) return 255;
            return value;
        }
        // Helper function to get the JPEG encoder
        private static ImageCodecInfo GetEncoder(ImageFormat format)
        {
            ImageCodecInfo[] codecs = ImageCodecInfo.GetImageDecoders();
            foreach (ImageCodecInfo codec in codecs)
            {
                if (codec.FormatID == format.Guid)
                {
                    return codec;
                }
            }
            return null;
        }


        // Convert YUV420 directly to Base64 JPEG
        public static string ConvertYUV420ToBase64Jpeg(IntPtr yuvFrame, int image_size, int width, int height, long quality = 50L)
        {
            byte[] yuvBytes = new byte[image_size];
            Marshal.Copy(yuvFrame, yuvBytes, 0, image_size);

            using (Bitmap bmp = ConvertYUV420ToBitmap(yuvBytes, width, height))
            {
                using (MemoryStream ms = new MemoryStream())
                {
                    // Get a JPEG encoder
                    ImageCodecInfo jpgEncoder = GetEncoder(ImageFormat.Jpeg);

                    // Create an Encoder object based on the Quality parameter category
                    System.Drawing.Imaging.Encoder myEncoder = System.Drawing.Imaging.Encoder.Quality;

                    // Create an EncoderParameters object
                    EncoderParameters encoderParameters = new EncoderParameters(1);

                    // Set the quality parameter (0-100)
                    EncoderParameter encoderParameter = new EncoderParameter(myEncoder, quality);
                    encoderParameters.Param[0] = encoderParameter;

                    // Save the bitmap as a JPEG file with the quality setting
                    bmp.Save(ms, jpgEncoder, encoderParameters);

                    byte[] jpegBytes = ms.ToArray();
                    return Convert.ToBase64String(jpegBytes);
                }
            }
            yuvBytes = null;
        }

        //C++ Event callback, post imageframe when Analytics detected.
        public static void Callback(int channelid, int width, int height, IntPtr imageframe, int image_size, ulong timestamp, IntPtr rois_rects, int rois_count, int node_count)
        {
            // Convert YUV420 to JPEG and then to Base64
            string base64JpegString = ConvertYUV420ToBase64Jpeg(imageframe, image_size, width, height, m_jpg_compress);

            // Convert ROI array (2D array flattened to 1D in C++ side)
            List<List<ROI>> rois = new List<List<ROI>>();

            if (rois_count > 0 && node_count > 0)
            {
                for (int i = 0; i < rois_count; i++)
                {
                    List<ROI> roiNodes = new List<ROI>();

                    for (int j = 0; j < node_count; j++)
                    {
                        int offset = (i * node_count + j) * Marshal.SizeOf<ROI>();
                        IntPtr roiPtr = IntPtr.Add(rois_rects, offset);
                        ROI roi = Marshal.PtrToStructure<ROI>(roiPtr);

                        // Add a deep copy of each ROI
                        ROI copiedROI = new ROI
                        {
                            x = roi.x,
                            y = roi.y
                        };

                        roiNodes.Add(copiedROI);
                    }

                    rois.Add(roiNodes);
                }
            }

            // Deep copy of rois into analyticsResult
            List<List<ROI>> deepCopiedRois = DeepCopyRois(rois);

            // Deep copy of all data including timestamp and rois
            var analyticsResult = new AnalyticsResult
            {
                version = "1.2",
                port_num = m_portnum,       // Value types are automatically copied
                keyframe = string.Copy(base64JpegString), // Ensure the string is copied
                timestamp = timestamp,      // Value types are automatically copied
                rois_rects = deepCopiedRois // Deep copy of rois list
            };

            // Enqueue the analytics result for processing
            lock (queueLock)
            {
                httpRequestQueue.Enqueue(analyticsResult);
            }

            rois.Clear();
        }

        // Function to deep copy the rois structure
        private static List<List<ROI>> DeepCopyRois(List<List<ROI>> originalRois)
        {
            List<List<ROI>> deepCopiedRois = new List<List<ROI>>();

            foreach (var roiList in originalRois)
            {
                List<ROI> copiedRoiList = new List<ROI>();

                foreach (var roi in roiList)
                {
                    ROI copiedROI = new ROI
                    {
                        x = roi.x,
                        y = roi.y
                    };

                    copiedRoiList.Add(copiedROI);
                }

                deepCopiedRois.Add(copiedRoiList);
            }

            return deepCopiedRois;
        }

        public static ROI[] InitializeRoisArray(int size)
        {
            ROI[] rois = new ROI[size];
            for (int i = 0; i < size; i++)
            {
                rois[i] = new ROI { x = -1, y = -1 }; // Default values
            }
            return rois;
        }

        private static void ProcessHttpRequestQueue(object state)
        {
            lock (queueLock)
            {
                while (httpRequestQueue.Count > 0)
                {
                    AnalyticsResult result = httpRequestQueue.Dequeue();
                    // Send the HTTP request
                    SendHttpRequest(result);
                }
            }
        }

        private static void SendHttpRequest(AnalyticsResult result)
        {
            // Example HTTP client code (ensure it's properly implemented)
            SimpleHttpClient client = new SimpleHttpClient();
            Task<string> responseTask = client.PostAnalyticsResultAsync(m_url, result);
            responseTask.ContinueWith(task =>
            {
                try
                {
                    string response = task.Result;
                    if (response == "") // normally not response , only get success code 200
                        Console.WriteLine("Detected!! send analytics result to server!!");
                }
                catch (Exception e)
                {
                    Console.WriteLine($"Response: {e}");
                }
            });
        }

        public static void StopQueueProcessor()
        {
            queueProcessorTimer?.Dispose();
        }

        static async Task Main(string[] args)
        {

            Console.WriteLine("Usage: SampleWrapper port=<httpPort>");
            int httpServerPort = 51000;

            if (args.Length > 0)
            {                
                string portArgument = null;

                foreach (string arg in args)
                {
                    if (arg.StartsWith("port="))
                    {
                        portArgument = arg.Substring("port=".Length);
                        break;
                    }
                }

                if (portArgument != null && int.TryParse(portArgument, out int port))
                {
                    httpServerPort = port;
                    Console.WriteLine($"Port number: {httpServerPort}");
                    
                    // You can add more logic here to start a server or handle the port number as needed
                }
                else
                {
                    Console.WriteLine("Invalid Input. Use default 51000");
                }
            }
            m_portnum = httpServerPort;

            string httpServerUrl = $"http://127.0.0.1:{httpServerPort}/";
            Console.WriteLine($"httpServerUrl: {httpServerUrl}");
            // Initialize the DLL
            Initialize(httpServerPort);

            // Set up a timer to process the queue periodically
            queueProcessorTimer = new Timer(ProcessHttpRequestQueue, null, TimeSpan.Zero, TimeSpan.FromMilliseconds(500));

            // Register callback
            callbackDelegate = new CallBackFunction(Callback);

            registerCallback(callbackDelegate);
            Console.WriteLine("register Callback");
            // Create HTTP server
            SimpleHttpServer server = new SimpleHttpServer(new[] { httpServerUrl });

            // Start the HTTP server
            Task serverTask = server.StartAsync();
            Console.WriteLine("start http server");
            // Start a task to send analytics results
            Task sendAnalyticsTask = Task.Run(async () =>
            {
                while (true)
                {
                    if (server.IsUpdateParam())
                    {
                        var parameters = server.GetParameters();
                        if (!parameters.Equals(default(SettingParameters)))
                        {
                            // Fixed-size ROI array for 10x10 grid
                            ROI[] roisArray = InitializeRoisArray(100);

                            // Populate the ROI array from parameters, defaulting to -1 for unused entries
                            int roiCount = Math.Min(parameters.rois.Length, 100); // Limit to 100 to avoid overflow

                            for (int i = 0; i < roiCount; i++)
                            {
                                roisArray[i] = new ROI { x = parameters.rois[i].x, y = parameters.rois[i].y };
                            }

                            // Prepare SetParameters struct
                            SettingParameters setParameters = new SettingParameters
                            {
                                analytics_event_api_url = parameters.analytics_event_api_url,
                                image_width = parameters.image_width,
                                image_height = parameters.image_height,
                                jpg_compress = parameters.jpg_compress,
                                sensitivity = new int[10], // Initialize sensitivity array
                                threshold = new int[10],   // Initialize threshold array
                                rois = roisArray // Assign dynamically created ROI array
                            };

                            // Copy the sensitivity array values
                            for (int i = 0; i < Math.Min(10, parameters.sensitivity.Length); i++)
                            {
                                setParameters.sensitivity[i] = parameters.sensitivity[i];
                            }

                            // Copy the threshold array values
                            for (int i = 0; i < Math.Min(10, parameters.threshold.Length); i++)
                            {
                                setParameters.threshold[i] = parameters.threshold[i];
                            }

                            // has send jpg compress , set it 
                            if(setParameters.jpg_compress > 0)
                                m_jpg_compress = setParameters.jpg_compress;

                            m_url = setParameters.analytics_event_api_url;
                            // Pass parameters to DLL
                            SettingParameters(ref setParameters);
                        }
                    }
                    await Task.Delay(1000); // Adjust the delay as necessary
                }
            });


            // Keep the server running and reading memory
            await Task.WhenAll(serverTask, sendAnalyticsTask);

            // StopHttp Event post Queue
            StopQueueProcessor();

            unregisterCallback();
            // Deinitialize the DLL
            Deinitialize();
            // release callback delegate
            callbackDelegate = null;
        }
    }
}
