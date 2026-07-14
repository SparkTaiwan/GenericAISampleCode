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
        // Trigger throttle: minimum seconds between sends. 0 = no throttle. The initial value MUST
        // mirror the schema's advertised default (SettingsSchema trigger_interval @default = 1) — the
        // schema default is only a UI hint served by /GetSettingsSchema; it is never pushed into this
        // runtime store on its own. If the recorder's first /SetParameters omits ai_settings (config
        // values not yet propagated), _triggerIntervalSec stays at this value, so starting at 0 would
        // make the throttle behave like "off" (0 s) until a later SetParameters echoes trigger_interval
        // back — the exact "first send isn't throttled, resend fixes it" bug. Both schemas (motion and
        // object detection) default to 1, so 1 is the correct startup value. (jpg above does the same:
        // _jpgQuality = 30 mirrors JpgCompressField @default = 30.)
        private volatile int _triggerIntervalSec = 1;

        public string Url               => _url;
        public int    JpgQuality        => _jpgQuality;
        public int    TriggerIntervalSec => _triggerIntervalSec;

        // triggerIntervalSec: null keeps the previous value (field absent from this SetParameters);
        // a value (0..3600) replaces it. 0 is meaningful (disable throttle), so it can't use the
        // ">0 keeps previous" trick jpg_compress uses.
        public void Update(string url, int jpgCompress, int? triggerIntervalSec = null)
        {
            lock (_lock)
            {
                // Clamp to turbojpeg's valid 1..100 range — an out-of-range
                // quality would make tjCompressFromYUV fail on every frame.
                if (jpgCompress > 0) _jpgQuality = System.Math.Min(jpgCompress, 100);
                _url = url ?? "";
                if (triggerIntervalSec.HasValue)
                {
                    int v = triggerIntervalSec.Value;
                    if (v < 0) v = 0;
                    if (v > 3600) v = 3600;
                    _triggerIntervalSec = v;
                }
            }
        }
    }
}
