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
            // cap=100 per spec §8.1
            EncodeQ = new BlockingCollection<RawDetection>(100);
            SendQ = new BlockingCollection<HttpEnvelope>(100);
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
