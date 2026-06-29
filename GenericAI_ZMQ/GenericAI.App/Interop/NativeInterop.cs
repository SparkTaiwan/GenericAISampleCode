using System;
using System.Runtime.InteropServices;

namespace GenericAI.App
{
    internal static class NativeInterop
    {
        private const string DllName = "GenericAI.Native.dll";

        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        public struct ROI
        {
            public int x;
            public int y;
        }

        [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
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
        }

        // Supported detection classes, FIXED order — mirrors
        // GenericAI.Native/class_table.h 1:1. The per-channel class bitmask bit i
        // and the callback's classCounts[i] both index this array.
        public static readonly string[] SupportedClasses =
            { "person", "car", "bus", "truck", "motorcycle", "bicycle", "cat", "dog" };

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        public delegate void CallBackFunction(
            int channelId, int width, int height,
            IntPtr frameI420, int frameSize,
            ulong timestamp,
            IntPtr roisFlat, int roisCount, int nodeCount,
            // Per supported-class detection count (SupportedClasses order); length
            // = classCountsLen. Used to emit "<class>___Count" items.
            IntPtr classCounts, int classCountsLen);

        // Native diagnostic line for the file log. level: 0 info, 1 warn,
        // 2 error; message is ANSI and only valid during the call.
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        public delegate void LogCallbackFunction(int level, IntPtr message);

        // detectorKind: 0 = Motion, 1 = Person (object detection); -1 (or any
        // out-of-range value) falls back to the native compile-time default
        // in gai_config.h. Lets the recorder pick the backend via `detector=`.
        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int GAI_InitializeChannels(int[] ports, int count, int detectorKind);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int GAI_SetChannelParameters(int port, ref SettingParameters parameters);

        // Per-channel ai_settings (spec §5). Scalars so it needs no ABI struct change.
        // Object detection: confidence (0..1; <0 clears) + classMask (bitmask over
        // SupportedClasses index; <0 => all). Motion: sensitivity + threshold
        // (0..100; <0 => keep per-ROI value). Pass <0 for keys a detector doesn't use.
        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int GAI_SetChannelAiSettings(
            int port, float confidence, int classMask, int sensitivity, int threshold);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void GAI_RegisterCallback(CallBackFunction cb);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void GAI_RegisterLogCallback(LogCallbackFunction cb);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int GAI_Deinitialize();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int GAI_GetBackend(System.Text.StringBuilder buf, int bufLen);

        // Last init error (e.g. model load failure). Returns 0 when healthy. Used
        // after a degraded init (GAI_InitializeChannels == 5) to report on /Alive.
        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int GAI_GetInitError(System.Text.StringBuilder buf, int bufLen);

        // ZMQ frame plane. Call after GAI_InitializeChannels and before the first
        // /SetParameters. endpoint = the module's bound PUSH address; this side
        // connects a PULL socket to it. Returns 0 on success. When used, frames
        // arrive over ZMQ (NAL -> decode) instead of MMF.
        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int GAI_StartZmqReceiver(string endpoint);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void GAI_StopZmqReceiver();
    }
}
