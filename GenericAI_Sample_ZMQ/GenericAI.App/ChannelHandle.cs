using System.Threading;

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

        // ── motion trigger throttle (min interval between result sends) ──────────
        // Stopwatch ticks (monotonic); 0 = never sent.
        private long _lastTriggerTicks;

        // Returns true (and stamps "now") when at least intervalSec has elapsed since
        // the last pass; false to suppress this trigger. intervalSec <= 0 always passes
        // (throttle disabled). Thread-safe: the native callback may fire from a worker
        // thread, so a CAS lets exactly one trigger per window win.
        public bool TryPassTriggerInterval(int intervalSec)
        {
            if (intervalSec <= 0) return true;
            long now = System.Diagnostics.Stopwatch.GetTimestamp();
            long window = (long)intervalSec * System.Diagnostics.Stopwatch.Frequency;
            while (true)
            {
                long last = Volatile.Read(ref _lastTriggerTicks);
                if (last != 0 && (now - last) < window) return false;   // still inside the quiet window
                if (Interlocked.CompareExchange(ref _lastTriggerTicks, now, last) == last) return true;
                // Lost the race to another thread; re-read and re-check.
            }
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
