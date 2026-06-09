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
            // cap=5: in simulator runs, stop-stream-to-callback-stop latency (residual buffer drain)
            // matters more than backpressure absorption. 100 -> 5 cuts post-stop residue from 3-6 s to ~200 ms.
            // Side effect: any HTTP jitter >150 ms immediately pushes pressure back to the MMF reader and raises RS drops.
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
