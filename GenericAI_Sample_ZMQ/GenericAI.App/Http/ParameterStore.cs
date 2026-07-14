namespace GenericAI.App
{
    // Holds the small handful of parameters that change at runtime via
    // /SetParameters. Readers see volatile semantics — no lock needed on the
    // hot path; the only writer is HttpListenerHost, which serialises itself
    // through _lock. P/Invoke into native is the caller's responsibility
    // (HttpListenerHost owns the channel port and routes via
    // GAI_SetChannelParameters).
    internal sealed class ParameterStore
    {
        private readonly object _lock = new object();
        private volatile string _url = "";
        private volatile int _jpgQuality = 50;
        // Min seconds between result sends (motion schema "trigger_interval"). 0 = no
        // limit (send on every trigger). Read by the send throttle in Program.cs.
        private volatile int _triggerIntervalSec = 1;

        public string Url                => _url;
        public int    JpgQuality         => _jpgQuality;
        public int    TriggerIntervalSec => _triggerIntervalSec;

        // triggerIntervalSec is nullable: null (v1.2 / not in the schema) leaves the
        // current value unchanged; a value updates it (0 disables throttling).
        public void Update(string url, int jpgCompress, int? triggerIntervalSec = null)
        {
            lock (_lock)
            {
                // Clamp to the JPEG encoder's valid 1..100 quality range.
                if (jpgCompress > 0) _jpgQuality = System.Math.Min(jpgCompress, 100);
                if (triggerIntervalSec.HasValue && triggerIntervalSec.Value >= 0)
                    _triggerIntervalSec = triggerIntervalSec.Value;
                _url = url ?? "";
            }
        }
    }
}
