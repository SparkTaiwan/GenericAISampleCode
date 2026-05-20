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
    // ── Shared types (same layout as multi-mode for DLL P/Invoke) ────────────────────────────

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
        public int[] sensitivity;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 10)]
        public int[] threshold;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 100)]
        public ROI[] rois;
        // v1.3 additions
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string mode;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string channel_id;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string count_reset;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 4096)]
        public string ai_settings_json;
    }

    public struct AnalyticsResult
    {
        public string version;
        public int    port_num;
        public string keyframe;
        public ulong  timestamp;
        public List<List<ROI>> rois_rects;
        // v1.3 fields
        public string channel_id;
        public string datastring;
        public List<AnalyticsItem> items;
    }

    public class AnalyticsItem
    {
        public string name;
        public string value;
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    public delegate void CallBackFunction(int channelid, int width, int height,
        IntPtr imageframe, int image_size, ulong timestamp,
        IntPtr rois_rects, int rois_count, int node_count);

    // ── Program ───────────────────────────────────────────────────────────────────────────────

    public class Program
    {
        // Per-channel URL and jpg quality (keyed by channel port number)
        private static ConcurrentDictionary<int, string> m_channelUrls
            = new ConcurrentDictionary<int, string>();
        private static ConcurrentDictionary<int, int> m_channelJpgCompress
            = new ConcurrentDictionary<int, int>();

        private static Queue<AnalyticsResult>   httpRequestQueue = new Queue<AnalyticsResult>();
        private static readonly object          queueLock        = new object();
        private static Timer                    queueProcessorTimer;
        private static CallBackFunction         callbackDelegate;
        private static SimpleHttpServer         m_server;

        // ── DLL imports (single-mode DLL exports) ─────────────────────────────────────────────

        [DllImport("SampleDLL.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Initialize(int basePort);

        [DllImport("SampleDLL.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void OpenChannel(int channelPort,
            [MarshalAs(UnmanagedType.LPStr)] string mmfName);

        [DllImport("SampleDLL.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void CloseChannel(int channelPort);

        [DllImport("SampleDLL.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void SetChannelParameters(int channelPort,
            ref SettingParameters parameters);

        [DllImport("SampleDLL.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void RegisterCallback(CallBackFunction callback);

        [DllImport("SampleDLL.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void UnregisterCallback();

        [DllImport("SampleDLL.dll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void Deinitialize();

        // ── YUV helpers (identical to multi-mode) ─────────────────────────────────────────────

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

        public static string ConvertYUV420ToBase64Jpeg(IntPtr yuvFrame, int imageSize,
            int width, int height, long quality = 50L)
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
                return Convert.ToBase64String(ms.ToArray());
            }
        }

        // ── Callback (called by C++ DLL when a channel detects motion) ────────────────────────

        public static void Callback(int channelId, int width, int height,
            IntPtr imageframe, int image_size, ulong timestamp,
            IntPtr rois_rects, int rois_count, int node_count)
        {
            Console.WriteLine($"[Single] Callback: ch={channelId} {width}x{height} sz={image_size} rois={rois_count}");

            // Look up per-channel jpg quality
            int jpgQuality = m_channelJpgCompress.GetOrAdd(channelId, 50);
            string base64Jpeg = ConvertYUV420ToBase64Jpeg(imageframe, image_size,
                width, height, jpgQuality);

            // Convert flattened ROI array
            List<List<ROI>> rois = new List<List<ROI>>();
            if (rois_count > 0 && node_count > 0)
            {
                for (int i = 0; i < rois_count; i++)
                {
                    List<ROI> nodes = new List<ROI>();
                    for (int j = 0; j < node_count; j++)
                    {
                        int offset = (i * node_count + j) * Marshal.SizeOf<ROI>();
                        ROI roi = Marshal.PtrToStructure<ROI>(IntPtr.Add(rois_rects, offset));
                        nodes.Add(new ROI { x = roi.x, y = roi.y });
                    }
                    rois.Add(nodes);
                }
            }

            var result = new AnalyticsResult
            {
                version    = "1.3",
                port_num   = channelId,
                keyframe   = string.Copy(base64Jpeg),
                timestamp  = timestamp,
                rois_rects = rois,
                channel_id = channelId.ToString(),
                datastring = "",
                items      = new List<AnalyticsItem>()
            };

            lock (queueLock)
                httpRequestQueue.Enqueue(result);
        }

        // ── Queue processor ───────────────────────────────────────────────────────────────────

        private static void ProcessHttpRequestQueue(object state)
        {
            lock (queueLock)
            {
                while (httpRequestQueue.Count > 0)
                {
                    AnalyticsResult result = httpRequestQueue.Dequeue();
                    SendHttpRequest(result);
                }
            }
        }

        private static void SendHttpRequest(AnalyticsResult result)
        {
            // Route to the correct per-channel analytics URL
            if (!m_channelUrls.TryGetValue(result.port_num, out string url))
            {
                Console.WriteLine($"[Single] No URL for ch{result.port_num} — dropping. Known channels: [{string.Join(",", m_channelUrls.Keys)}]");
                return;
            }

            Console.WriteLine($"[Single] Ch{result.port_num}: posting to {url}");
            SimpleHttpClient client = new SimpleHttpClient();
            Task<string> task = client.PostAnalyticsResultAsync(url, result);
            task.ContinueWith(t => {
                try   { string r = t.Result; Console.WriteLine($"[Single] Ch{result.port_num}: sent analytics result"); }
                catch (Exception e) { Console.WriteLine($"[Single] Ch{result.port_num}: post error: {e.Message}"); }
            });
        }

        public static void StopQueueProcessor()
        {
            queueProcessorTimer?.Dispose();
        }

        // ── ROI helpers ───────────────────────────────────────────────────────────────────────

        public static ROI[] InitializeRoisArray(int size)
        {
            ROI[] rois = new ROI[size];
            for (int i = 0; i < size; i++)
                rois[i] = new ROI { x = -1, y = -1 };
            return rois;
        }

        // ── Channel open / close handlers (called from SimpleHttpServer) ──────────────────────

        private static void HandleOpenChannel(int channelPort, string mmfName)
        {
            Console.WriteLine($"[Single] OpenChannel: port={channelPort}, mmf={mmfName}");
            m_server.AddChannelPort(channelPort, HandleSetChannelParameters);
            OpenChannel(channelPort, mmfName);
        }

        private static void HandleCloseChannel(int channelPort)
        {
            Console.WriteLine($"[Single] CloseChannel: port={channelPort}");
            CloseChannel(channelPort);
            m_channelUrls.TryRemove(channelPort, out _);
            m_channelJpgCompress.TryRemove(channelPort, out _);
            m_server.RemoveChannelPort(channelPort);
        }

        private static void HandleSetChannelParameters(int channelPort, SettingParameters settings)
        {
            Console.WriteLine($"[Single] SetChannelParameters: port={channelPort}");

            // Cache per-channel values
            m_channelUrls[channelPort]       = settings.analytics_event_api_url;
            m_channelJpgCompress[channelPort] = settings.jpg_compress > 0 ? settings.jpg_compress : 50;

            // Pass to DLL
            SetChannelParameters(channelPort, ref settings);
        }

        // ── Main ──────────────────────────────────────────────────────────────────────────────

        static async Task Main(string[] args)
        {
            Console.WriteLine("Usage: SampleWrapper port=<basePort> mode=single [channel_count=<N>]");

            int basePort     = 9000;
            int channelCount = 4;

            foreach (string arg in args)
            {
                if (arg.StartsWith("port=") &&
                    int.TryParse(arg.Substring("port=".Length), out int p))
                    basePort = p;
                else if (arg.StartsWith("channel_count=") &&
                    int.TryParse(arg.Substring("channel_count=".Length), out int cc))
                    channelCount = cc;
            }

            Console.WriteLine($"[Single] Base port: {basePort}, max channels: {channelCount}");

            // Initialize DLL
            Initialize(basePort);

            // Register callback
            callbackDelegate = new CallBackFunction(Callback);
            RegisterCallback(callbackDelegate);
            Console.WriteLine("[Single] Callback registered");

            // Start queue processor timer
            queueProcessorTimer = new Timer(ProcessHttpRequestQueue, null,
                TimeSpan.Zero, TimeSpan.FromMilliseconds(500));

            // Create & start HTTP server
            m_server = new SimpleHttpServer(basePort);
            m_server.OnOpenChannel  += HandleOpenChannel;
            m_server.OnCloseChannel += HandleCloseChannel;

            Console.WriteLine($"[Single] Starting HTTP server on base port {basePort}...");
            Task serverTask = m_server.StartAsync();

            // Pre-open the base-port channel (channel 0).
            // ARGO never sends /OpenChannel for channel 0 — it assumes the channel is already
            // open because it started the process at the base port.  We must open it ourselves
            // so that _channelHandlers[basePort] is populated before /SetParameters arrives.
            Console.WriteLine($"[Single] Pre-opening base channel (port={basePort}, mmf=ChannelFrame_{basePort})");
            HandleOpenChannel(basePort, $"ChannelFrame_{basePort}");

            Console.WriteLine("[Single] Ready. Waiting for ARGO connections...");
            await serverTask;   // runs forever

            // Cleanup (reached only on server error)
            StopQueueProcessor();
            UnregisterCallback();
            Deinitialize();
            callbackDelegate = null;
        }
    }
}
