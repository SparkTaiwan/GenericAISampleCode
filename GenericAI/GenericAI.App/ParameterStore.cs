namespace GenericAI.App
{
    // Holds the small handful of parameters that change at runtime via
    // /SetParameters. Workers read with volatile semantics — no lock needed
    // on the hot path; the only writer is HttpListenerHost, which serialises
    // itself through _lock. P/Invoke into native is the caller's responsibility
    // (HttpListenerHost owns the channel port and routes via
    // GAI_SetChannelParameters).
    internal sealed class ParameterStore
    {
        private readonly object _lock = new object();
        private volatile string _url = "";
        private volatile int _jpgQuality = 50;

        public string Url        => _url;
        public int    JpgQuality => _jpgQuality;

        public void Update(string url, int jpgCompress)
        {
            lock (_lock)
            {
                if (jpgCompress > 0) _jpgQuality = jpgCompress;
                _url = url ?? "";
            }
        }
    }
}
