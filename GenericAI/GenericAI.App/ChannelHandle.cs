using System.Collections.Concurrent;

namespace GenericAI.App
{
    // Per-channel container. Does NOT hold worker tasks — Program.cs owns the
    // process-wide EncodeWorker / SendWorker pool that drains all channels via
    // BlockingCollection<T>.TakeFromAny over the per-channel queues.
    internal sealed class ChannelHandle
    {
        public int Port { get; }
        public ParameterStore Parameters { get; }
        public BlockingCollection<RawDetection> EncodeQ { get; }
        public BlockingCollection<HttpEnvelope> SendQ { get; }
        public HttpListenerHost Listener { get; }

        public ChannelHandle(int port)
        {
            Port = port;
            Parameters = new ParameterStore();
            Listener = new HttpListenerHost(port, Parameters);
            // cap=5：simulator 場景下「停串流→callback 馬上停」的延遲（殘餘 buffer 排空時間）
            // 比 backpressure 吸收量重要。100 → 5 把停後殘餘從 3–6 秒壓到 ~200 ms。
            // 副作用：任何 >150 ms 的 HTTP 抖動會立刻把壓力打回 MMF reader、推升 RS drop。
            EncodeQ = new BlockingCollection<RawDetection>(5);
            SendQ = new BlockingCollection<HttpEnvelope>(5);
        }

        public bool StartListener()
        {
            return Listener.Start();
        }

        public void StopListener()
        {
            Listener.Stop();
        }

        public void CompleteAddingEncode()
        {
            EncodeQ.CompleteAdding();
        }

        public void CompleteAddingSend()
        {
            SendQ.CompleteAdding();
        }
    }
}
