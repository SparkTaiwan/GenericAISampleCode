namespace GenericAI.App
{
    // Per-channel container: the channel's HTTP listener plus the url /
    // jpgQuality cache its /SetParameters fills in.
    internal sealed class ChannelHandle
    {
        public int Port { get; }
        public ParameterStore Parameters { get; }
        public HttpListenerHost Listener { get; }

        public ChannelHandle(int port, string host = "127.0.0.1")
        {
            Port = port;
            Parameters = new ParameterStore();
            Listener = new HttpListenerHost(port, Parameters, host);
        }

        public bool StartListener()
        {
            return Listener.Start();
        }

        public void StopListener()
        {
            Listener.Stop();
        }
    }
}
