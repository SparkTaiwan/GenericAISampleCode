using System;
using System.Collections.Concurrent;
using System.Threading;

namespace GenericAI.App
{
    // Per-channel container. Does NOT hold worker tasks — Program.cs owns the
    // process-wide EncodeWorker / SendWorker pool that drains all channels via
    // BlockingCollection<T>.TakeFromAny over the per-channel queues.
    internal sealed class ChannelHandle
    {
        // cap=5: in simulator runs, stop-stream-to-callback-stop latency (residual buffer drain)
        // matters more than backpressure absorption. 100 -> 5 cuts post-stop residue from 3-6 s to ~200 ms.
        // Side effect: any HTTP jitter >150 ms immediately pushes pressure back to the MMF reader and raises RS drops.
        private const int EncodeQueueCapacity = 5;
        private const int SendQueueCapacity = 5;

        public int Port { get; }
        public ParameterStore Parameters { get; }
        public BlockingCollection<RawDetection> EncodeQ { get; }
        public BlockingCollection<HttpEnvelope> SendQ { get; }
        public HttpListenerHost Listener { get; }

        public ChannelHandle(int port, string host = "127.0.0.1")
        {
            Port = port;
            Parameters = new ParameterStore();
            Listener = new HttpListenerHost(port, Parameters, host);
            EncodeQ = new BlockingCollection<RawDetection>(EncodeQueueCapacity);
            SendQ = new BlockingCollection<HttpEnvelope>(SendQueueCapacity);
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

        // ---- motion trigger throttle (min interval between sends) -----------
        // Records the last time a frame was allowed through for this channel.
        // Stopwatch ticks (monotonic); 0 = never sent.
        private long _lastTriggerTicks;

        // Returns true (and stamps "now") when at least intervalSec has elapsed since the last pass;
        // false to suppress this trigger. intervalSec <= 0 always passes (throttle disabled).
        // Thread-safe for the multi-worker EncodeWorker pool: a CAS lets exactly one frame per window win.
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
                // Lost the race to another worker; re-read and re-check.
            }
        }

        // ---- send-retry parking (per-channel failure isolation) ------------
        // A payload whose POST failed is parked here instead of being retried
        // in place by the worker that happened to hold it. While anything is
        // parked the channel counts as "blocked": SendWorkers take no new
        // envelopes from its SendQ, so the failure's backpressure stays on
        // this channel (SendQ -> EncodeQ -> native pipeline) instead of
        // wedging the shared worker pool. The queue holds at most one payload
        // per worker (each can fail one fresh send concurrently); order
        // within a channel is irrelevant downstream (timestamp-keyed).

        private readonly ConcurrentQueue<byte[]> _parkedSends = new ConcurrentQueue<byte[]>();
        private long _sendRetryAtTicks;
        private int _sendRetryClaimed;

        public bool HasParkedSend => !_parkedSends.IsEmpty;

        public void ParkSend(byte[] payload, TimeSpan backoff)
        {
            _parkedSends.Enqueue(payload);
            Volatile.Write(ref _sendRetryAtTicks, DateTime.UtcNow.Ticks + backoff.Ticks);
        }

        // Claims the channel's next parked payload for one retry attempt.
        // False when nothing is parked, the backoff window has not elapsed,
        // or another worker already holds the claim — the claim flag is what
        // keeps concurrent workers from double-sending the same payload.
        public bool TryClaimParkedSend(out byte[] payload)
        {
            payload = null;
            if (_parkedSends.IsEmpty) return false;
            if (DateTime.UtcNow.Ticks < Volatile.Read(ref _sendRetryAtTicks)) return false;
            if (Interlocked.CompareExchange(ref _sendRetryClaimed, 1, 0) != 0) return false;
            if (!_parkedSends.TryDequeue(out payload))
            {
                Volatile.Write(ref _sendRetryClaimed, 0);
                return false;
            }
            return true;
        }

        // Releases the claim taken by TryClaimParkedSend; on failure the
        // payload goes back to the parking queue with a fresh backoff window.
        public void CompleteParkedSend(byte[] payload, bool success, TimeSpan backoff)
        {
            if (!success) ParkSend(payload, backoff);
            Volatile.Write(ref _sendRetryClaimed, 0);
        }
    }
}
