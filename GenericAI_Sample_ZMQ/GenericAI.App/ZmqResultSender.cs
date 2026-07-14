// ZMQ result transport -- compiled only when USE_ZMQ is defined.
// Release-NoZmq leaves this out, so the output has no libzmq dependency.
#if USE_ZMQ
using System;
using System.Runtime.InteropServices;

namespace GenericAI.App
{
    // Sends analytics results over ZMQ instead of HTTP POST. Connection model
    // mirrors the frame plane in reverse: the module/recorder BINDs a PULL socket
    // and this side (the AI) CONNECTs a PUSH to it (so ZMQ auto-reconnects when the
    // module restarts). One process-wide socket carries every channel's results;
    // the channel is identified by port_num inside the JSON payload.
    //
    // P/Invokes the same libzmq the native wrapper already ships, so no managed ZMQ
    // dependency is added. Send() is serialised because ZMQ sockets are not
    // thread-safe (the send timer + callback path can race).
    internal sealed class ZmqResultSender : IDisposable
    {
        private const string ZMQ = "libzmq-v143-mt-4_2_0.dll";
        private const int ZMQ_PUSH = 8;
        private const int ZMQ_SNDHWM = 23;
        private const int ZMQ_SNDTIMEO = 28;
        private const int ZMQ_LINGER = 17;

        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr zmq_ctx_new();
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern int zmq_ctx_term(IntPtr ctx);
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr zmq_socket(IntPtr ctx, int type);
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern int zmq_close(IntPtr s);
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)] private static extern int zmq_connect(IntPtr s, string addr);
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern int zmq_setsockopt(IntPtr s, int opt, ref int val, UIntPtr len);
        [DllImport(ZMQ, CallingConvention = CallingConvention.Cdecl)] private static extern int zmq_send(IntPtr s, byte[] buf, UIntPtr len, int flags);

        private readonly object _lock = new object();
        private IntPtr _ctx;
        private IntPtr _sock;
        private bool _disposed;

        public ZmqResultSender(string endpoint, int channelCount = 1)
        {
            _ctx = zmq_ctx_new();
            _sock = zmq_socket(_ctx, ZMQ_PUSH);
            // HWM scales with the shard size (all channels share this PUSH).
            int hwm = Math.Max(64, channelCount * 2), sndtimeo = 1000, linger = 0;
            zmq_setsockopt(_sock, ZMQ_SNDHWM, ref hwm, (UIntPtr)(uint)sizeof(int));
            zmq_setsockopt(_sock, ZMQ_SNDTIMEO, ref sndtimeo, (UIntPtr)(uint)sizeof(int));
            zmq_setsockopt(_sock, ZMQ_LINGER, ref linger, (UIntPtr)(uint)sizeof(int));
            if (zmq_connect(_sock, endpoint) != 0)
                throw new InvalidOperationException("zmq_connect failed for result endpoint: " + endpoint);
        }

        // Returns true if the payload was queued for sending. False on timeout
        // (no consumer / HWM).
        public bool Send(byte[] payload)
        {
            lock (_lock)
            {
                if (_disposed || _sock == IntPtr.Zero) return false;
                int r = zmq_send(_sock, payload, (UIntPtr)(uint)payload.Length, 0);
                return r >= 0;
            }
        }

        public void Dispose()
        {
            lock (_lock)
            {
                if (_disposed) return;
                _disposed = true;
                if (_sock != IntPtr.Zero) { try { zmq_close(_sock); } catch { } _sock = IntPtr.Zero; }
                if (_ctx != IntPtr.Zero) { try { zmq_ctx_term(_ctx); } catch { } _ctx = IntPtr.Zero; }
            }
        }
    }
}
#endif // USE_ZMQ
