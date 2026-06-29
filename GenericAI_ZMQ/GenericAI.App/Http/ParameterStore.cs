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
        private volatile int _jpgQuality = 30;

        public string Url        => _url;
        public int    JpgQuality => _jpgQuality;

        public void Update(string url, int jpgCompress)
        {
            lock (_lock)
            {
                // Clamp to turbojpeg's valid 1..100 range — an out-of-range
                // quality would make tjCompressFromYUV fail on every frame.
                if (jpgCompress > 0) _jpgQuality = System.Math.Min(jpgCompress, 100);
                _url = url ?? "";
            }
        }
    }
}
