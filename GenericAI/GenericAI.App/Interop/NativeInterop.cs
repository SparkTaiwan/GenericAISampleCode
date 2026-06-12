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

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        public delegate void CallBackFunction(
            int channelId, int width, int height,
            IntPtr frameI420, int frameSize,
            ulong timestamp,
            IntPtr roisFlat, int roisCount, int nodeCount);

        // Native diagnostic line for the file log. level: 0 info, 1 warn,
        // 2 error; message is ANSI and only valid during the call.
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        public delegate void LogCallbackFunction(int level, IntPtr message);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int GAI_InitializeChannels(int[] ports, int count);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int GAI_SetChannelParameters(int port, ref SettingParameters parameters);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void GAI_RegisterCallback(CallBackFunction cb);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void GAI_RegisterLogCallback(LogCallbackFunction cb);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int GAI_Deinitialize();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int GAI_GetBackend(System.Text.StringBuilder buf, int bufLen);
    }
}
