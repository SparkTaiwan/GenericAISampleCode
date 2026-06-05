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
        private const int QueueCapacity = 100;
        private static readonly int RoiSize = Marshal.SizeOf<ROI>();

        private static int m_portnum = 0;
        // volatile: written under s_paramLock by ApplyParameters on an HTTP listener thread,
        // read without the lock by the native callback and the queue worker. volatile guarantees
        // the new value is visible to readers without coordinating on the lock.
        private static volatile string m_url = "";
        private static volatile int m_jpg_compress = 50;

        // Bounded so a stalled downstream can't grow the queue (and base64 JPEG payload) without
        // bound. drop-newest policy: if the queue is full when a detection arrives, we drop the
        // incoming one. Newer detections are more valuable than backlog for live monitoring,
        // but the simpler drop-newest avoids extra contention with the worker side.
        private static BlockingCollection<AnalyticsResult> httpRequestQueue =
            new BlockingCollection<AnalyticsResult>(QueueCapacity);

        // Counters for periodic summary logging so we don't spam one WARN per drop.
        private static long s_droppedTotal = 0;
        private static long s_droppedSinceReport = 0;
        private static DateTime s_lastDropReport = DateTime.UtcNow;

        private static Task queueWorkerTask;
        private static CallBackFunction callbackDelegate;
        private static readonly SimpleHttpClient httpClient = new SimpleHttpClient();

        // Lifecycle state - guards Cleanup against double execution and against calling
        // Deinitialize() when Initialize() never ran (or threw).
        private static readonly object s_cleanupLock = new object();
        private static readonly object s_paramLock = new object();
        private static bool s_initialized = false;
        private static bool s_timeBeginPeriodSet = false;
        private static bool s_cleanedUp = false;

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

        [DllImport("winmm.dll", EntryPoint = "timeBeginPeriod")]
        private static extern uint TimeBeginPeriod(uint period);
        [DllImport("winmm.dll", EntryPoint = "timeEndPeriod")]
        private static extern uint TimeEndPeriod(uint period);

        //Convert YUV420 to Bitmap
        // Unused reference impl kept commented per repo convention.
        //public static Bitmap ConvertYUV420ToBitmap(byte[] yuvBytes, int width, int height)
        //{
        //    int frameSize = width * height;
        //    int chromaSize = frameSize / 4;
        //
        //    if (yuvBytes.Length != frameSize + 2 * chromaSize)
        //    {
        //        throw new ArgumentException("Invalid YUV420 data size.");
        //    }
        //
        //    Bitmap bmp = new Bitmap(width, height, PixelFormat.Format24bppRgb);
        //
        //    for (int y = 0; y < height; y++)
        //    {
        //        for (int x = 0; x < width; x++)
        //        {
        //            int yIndex = y * width + x;
        //            int uIndex = (y / 2) * (width / 2) + (x / 2) + frameSize;
        //            int vIndex = (y / 2) * (width / 2) + (x / 2) + frameSize + chromaSize;
        //
        //            if (yIndex >= frameSize || uIndex >= frameSize + chromaSize || vIndex >= frameSize + chromaSize * 2)
        //            {
        //                throw new IndexOutOfRangeException("Index out of range for YUV data.");
        //            }
        //
        //            int Y = yuvBytes[yIndex] & 0xFF;
        //            int U = yuvBytes[uIndex] & 0xFF;
        //            int V = yuvBytes[vIndex] & 0xFF;
        //
        //            int C = Y - 16;
        //            int D = U - 128;
        //            int E = V - 128;
        //
        //            int R = Clip((298 * C + 409 * E + 128) >> 8);
        //            int G = Clip((298 * C - 100 * D - 208 * E + 128) >> 8);
        //            int B = Clip((298 * C + 516 * D + 128) >> 8);
        //
        //            bmp.SetPixel(x, y, Color.FromArgb(Clip(R), Clip(G), Clip(B)));
        //        }
        //    }
        //
        //    return bmp;
        //}

        //private static int Clip(int value)
        //{
        //    if (value < 0) return 0;
        //    if (value > 255) return 255;
        //    return value;
        //}

        // Helper function to get the JPEG encoder
        // Unused; also buggy - looked up encoders via GetImageDecoders(). Kept commented.
        //private static ImageCodecInfo GetEncoder(ImageFormat format)
        //{
        //    ImageCodecInfo[] codecs = ImageCodecInfo.GetImageDecoders();
        //    foreach (ImageCodecInfo codec in codecs)
        //    {
        //        if (codec.FormatID == format.Guid)
        //        {
        //            return codec;
        //        }
        //    }
        //    return null;
        //}


        // Convert YUV420 directly to Base64 JPEG
        public static string ConvertYUV420ToBase64Jpeg(IntPtr yuvFrame, int image_size, int width, int height, long quality = 50L)
        {
            byte[] jpegBytes = TurboJpegInterop.EncodeI420(yuvFrame, image_size, width, height, (int)quality);
            return Convert.ToBase64String(jpegBytes);
        }

        //C++ Event callback, post imageframe when Analytics detected.
        public static void Callback(int channelid, int width, int height, IntPtr imageframe, int image_size, ulong timestamp, IntPtr rois_rects, int rois_count, int node_count)
        {
            try
            {
                // Snapshot volatile fields once per callback so a mid-callback ApplyParameters
                // doesn't make us encode with one quality but log with another.
                int jpgQuality = m_jpg_compress;

                string base64JpegString = ConvertYUV420ToBase64Jpeg(imageframe, image_size, width, height, jpgQuality);

                // Convert ROI array (2D array flattened to 1D in C++ side)
                List<List<ROI>> rois = new List<List<ROI>>();

                if (rois_count > 0 && node_count > 0)
                {
                    for (int i = 0; i < rois_count; i++)
                    {
                        List<ROI> roiNodes = new List<ROI>();

                        for (int j = 0; j < node_count; j++)
                        {
                            int offset = (i * node_count + j) * RoiSize;
                            IntPtr roiPtr = IntPtr.Add(rois_rects, offset);
                            ROI roi = Marshal.PtrToStructure<ROI>(roiPtr);

                            ROI copiedROI = new ROI { x = roi.x, y = roi.y };
                            roiNodes.Add(copiedROI);
                        }

                        rois.Add(roiNodes);
                    }
                }

                var analyticsResult = new AnalyticsResult
                {
                    version = "1.2",
                    port_num = m_portnum,
                    keyframe = base64JpegString,
                    timestamp = timestamp,
                    rois_rects = rois
                };

                // Drop-newest on full queue: keep the native callback non-blocking and let the
                // periodic summary log surface sustained drops without flooding error.log.
                if (httpRequestQueue.IsAddingCompleted || !httpRequestQueue.TryAdd(analyticsResult))
                {
                    Interlocked.Increment(ref s_droppedTotal);
                    Interlocked.Increment(ref s_droppedSinceReport);
                }

                FileLogger.Info($"Detection callback: ch={channelid} size={image_size} w={width} h={height} rois={rois_count} nodes={node_count}");
            }
            catch (Exception ex)
            {
                // Never let an exception propagate back into native code - it would crash the process.
                FileLogger.Error($"Callback failed (ch={channelid}, size={image_size}, rois={rois_count})", ex);
            }
        }

        // Function to deep copy the rois structure
        //private static List<List<ROI>> DeepCopyRois(List<List<ROI>> originalRois)
        //{
        //    List<List<ROI>> deepCopiedRois = new List<List<ROI>>();
        //
        //    foreach (var roiList in originalRois)
        //    {
        //        List<ROI> copiedRoiList = new List<ROI>();
        //
        //        foreach (var roi in roiList)
        //        {
        //            ROI copiedROI = new ROI
        //            {
        //                x = roi.x,
        //                y = roi.y
        //            };
        //
        //            copiedRoiList.Add(copiedROI);
        //        }
        //
        //        deepCopiedRois.Add(copiedRoiList);
        //    }
        //
        //    return deepCopiedRois;
        //}

        public static ROI[] InitializeRoisArray(int size)
        {
            ROI[] rois = new ROI[size];
            for (int i = 0; i < size; i++)
            {
                rois[i] = new ROI { x = -1, y = -1 }; // Default values
            }
            return rois;
        }

        // Called by the HTTP listener thread when /SetParameters arrives. Holding s_paramLock
        // serializes the native call and guarantees m_url, m_jpg_compress and the DLL state all
        // reflect the same request (no torn struct, no lost update from a racing handler).
        public static void ApplyParameters(SettingParameters incoming)
        {
            lock (s_paramLock)
            {
                if (incoming.jpg_compress > 0)
                    m_jpg_compress = incoming.jpg_compress;
                m_url = incoming.analytics_event_api_url ?? "";

                SettingParameters local = incoming;
                SettingParameters(ref local);
            }
        }

        // Single worker drains the queue strictly in order: one request completes (success or
        // failure) before the next starts, and items arriving while a send is in flight simply
        // wait in the queue. GetConsumingEnumerable blocks when empty, so there is no busy loop.
        private static async Task RunQueueWorker()
        {
            foreach (AnalyticsResult result in httpRequestQueue.GetConsumingEnumerable())
            {
                await SendHttpRequestAsync(result);
                ReportDropsIfDue();
            }
        }

        private static void ReportDropsIfDue()
        {
            // One-line summary per minute of any drops observed in that window. Cheap clock check
            // avoids per-iteration logging while still surfacing sustained backpressure.
            if ((DateTime.UtcNow - s_lastDropReport).TotalSeconds < 60)
                return;

            long since = Interlocked.Exchange(ref s_droppedSinceReport, 0);
            s_lastDropReport = DateTime.UtcNow;
            if (since > 0)
            {
                FileLogger.Warn($"Queue full: dropped {since} detection(s) in last minute (total dropped={Interlocked.Read(ref s_droppedTotal)})");
            }
        }

        private static async Task SendHttpRequestAsync(AnalyticsResult result)
        {
            int portCaptured = result.port_num;
            string urlCaptured = m_url;
            // Skip until /SetParameters has supplied a target URL. Otherwise HttpClient.PostAsync
            // would throw InvalidOperationException ("absolute URI required") for every queued
            // detection that arrived during startup and flood error.log.
            if (string.IsNullOrEmpty(urlCaptured))
                return;
            try
            {
                string response = await httpClient.PostAnalyticsResultAsync(urlCaptured, result);
                if (response == "") // normally no response, only success code 200
                {
                    Console.WriteLine("Detected!! send analytics result to server!!");
                    FileLogger.Info($"Analytics result posted ok (ch={portCaptured})");
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Response: {ex.Message}");
                FileLogger.Error($"Analytics result post failed (ch={portCaptured}, url={urlCaptured})", ex);
            }
        }

        // Idempotent shutdown. Safe to call from ProcessExit, CancelKeyPress, finally, or fatal
        // catch. TerminateProcess from the recorder watchdog still bypasses everything - that's
        // a known un-fixable case on this side.
        public static void Cleanup()
        {
            lock (s_cleanupLock)
            {
                if (s_cleanedUp) return;
                s_cleanedUp = true;

                FileLogger.Info("Cleanup begin");

                try { httpRequestQueue.CompleteAdding(); } catch (Exception ex) { FileLogger.Error("Cleanup: CompleteAdding failed", ex); }

                try { queueWorkerTask?.Wait(TimeSpan.FromSeconds(5)); }
                catch (Exception ex) { FileLogger.Error("Cleanup: worker wait failed", ex); }

                if (callbackDelegate != null)
                {
                    try { unregisterCallback(); }
                    catch (Exception ex) { FileLogger.Error("Cleanup: unregisterCallback failed", ex); }
                    callbackDelegate = null;
                }

                if (s_initialized)
                {
                    try { Deinitialize(); }
                    catch (Exception ex) { FileLogger.Error("Cleanup: Deinitialize failed", ex); }
                    s_initialized = false;
                }

                if (s_timeBeginPeriodSet)
                {
                    try { TimeEndPeriod(1); } catch { }
                    s_timeBeginPeriodSet = false;
                }

                FileLogger.Info("Cleanup end");
            }
        }

        static async Task Main(string[] args)
        {
            TimeBeginPeriod(1);
            s_timeBeginPeriodSet = true;

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

            // Route logs to D:\SLog-<port>\ so concurrent instances on different ports don't share a file
            // and we stay out of Program Files (which UAC-virtualizes unprivileged writes).
            FileLogger.Init(httpServerPort);
            FileLogger.Info($"SampleWrapper starting (port={httpServerPort})");

            // Hook normal exit and Ctrl+C so Cleanup runs even when Main never returns via the
            // happy path. TerminateProcess still bypasses these.
            AppDomain.CurrentDomain.ProcessExit += (s, e) => Cleanup();
            Console.CancelKeyPress += (s, e) => { Cleanup(); /* let default Ctrl+C terminate */ };

            try
            {
                string httpServerUrl = $"http://127.0.0.1:{httpServerPort}/";
                Console.WriteLine($"httpServerUrl: {httpServerUrl}");

                // The bounded "starting/HTTP Server started" gap previously left us blind to
                // which native call hung. Emit a before/after for each step so a stuck instance
                // points at the offending stage in the log.
                FileLogger.Info($"Initialize before (port={httpServerPort})");
                Initialize(httpServerPort);
                s_initialized = true;
                FileLogger.Info($"Initialize after (port={httpServerPort})");

                // Single worker drains the queue in order, blocking when empty.
                queueWorkerTask = Task.Run((Func<Task>)RunQueueWorker);

                // Register callback
                callbackDelegate = new CallBackFunction(Callback);

                FileLogger.Info("registerCallback before");
                registerCallback(callbackDelegate);
                FileLogger.Info("registerCallback after");
                Console.WriteLine("register Callback");

                // Create HTTP server
                FileLogger.Info($"SimpleHttpServer ctor before (url={httpServerUrl})");
                SimpleHttpServer server = new SimpleHttpServer(new[] { httpServerUrl });
                FileLogger.Info("SimpleHttpServer ctor after");

                // Start the HTTP server. SetParameters now applies inline on the listener thread
                // via Program.ApplyParameters, so there's no separate polling task to coordinate.
                await server.StartAsync();
            }
            catch (Exception ex)
            {
                FileLogger.Error("Main fatal", ex);
                throw;
            }
            finally
            {
                Cleanup();
            }
        }
    }
}
