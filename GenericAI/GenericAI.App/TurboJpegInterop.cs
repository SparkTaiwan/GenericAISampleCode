using System;
using System.Runtime.InteropServices;

namespace GenericAI.App
{
    // Wraps libjpeg-turbo's tjCompressFromYUV for I420 -> JPEG. The handle is
    // [ThreadStatic] so each EncodeWorker thread owns its own — callers must
    // invoke ReleaseThreadHandle() before the thread exits (Ctrl+C path).
    internal static class TurboJpegInterop
    {
        private const string Dll = "turbojpeg";
        private const int TJSAMP_420 = 2;

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr tjInitCompress();

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        private static extern int tjDestroy(IntPtr handle);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        private static extern int tjCompressFromYUV(
            IntPtr handle, IntPtr srcBuf, int width, int pad, int height,
            int subsamp, ref IntPtr jpegBuf, ref uint jpegSize,
            int jpegQual, int flags);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        private static extern void tjFree(IntPtr buffer);

        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr tjGetErrorStr2(IntPtr handle);

        [ThreadStatic] private static IntPtr s_handle;

        public static byte[] EncodeI420(IntPtr yuvI420, int length, int width, int height, int quality)
        {
            int expected = width * height * 3 / 2;
            if (yuvI420 == IntPtr.Zero || length < expected)
                throw new ArgumentException("Invalid I420 buffer");

            if (s_handle == IntPtr.Zero)
            {
                s_handle = tjInitCompress();
                if (s_handle == IntPtr.Zero)
                    throw new InvalidOperationException("tjInitCompress failed");
            }

            IntPtr jpegBuf = IntPtr.Zero;
            uint jpegSize = 0;
            int rc = tjCompressFromYUV(s_handle, yuvI420, width, 1, height,
                                       TJSAMP_420, ref jpegBuf, ref jpegSize, quality, 0);
            if (rc != 0)
            {
                string err = Marshal.PtrToStringAnsi(tjGetErrorStr2(s_handle)) ?? "tjCompressFromYUV failed";
                if (jpegBuf != IntPtr.Zero) tjFree(jpegBuf);
                throw new InvalidOperationException(err);
            }

            try
            {
                byte[] managed = new byte[jpegSize];
                Marshal.Copy(jpegBuf, managed, 0, (int)jpegSize);
                return managed;
            }
            finally { tjFree(jpegBuf); }
        }

        public static void ReleaseThreadHandle()
        {
            if (s_handle != IntPtr.Zero)
            {
                tjDestroy(s_handle);
                s_handle = IntPtr.Zero;
            }
        }
    }
}
