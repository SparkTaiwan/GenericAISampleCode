using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace GenericAI.App
{
    // Per-frame timing recorder (singleton). Records timestamps at 7 points in
    // the C# pipeline, writes one line per frame at the terminal point (HTTP
    // post complete, or any drop path) to both the console and the
    // timing-<port>.log file (FileLogger.Timing). The matching C++ side has
    // its own recorder in timing_recorder.{h,cpp} printing to console only;
    // the lines are correlated by the frame's MMF timestamp.
    //
    // Manual switch: edit Enabled -> rebuild. It also gates the GenericAI-
    // specific verbose console lines in Program (Program.VerboseConsole), so
    // with it off the console output matches the CSharp sample exactly.
    // Matching native switch: kEnableTimingLog in gai_config.h.
    //
    // Enabled is a compile-time const: when false, the guard on every method
    // makes the rest of the body unreachable (CS0162) and the JIT strips the
    // calls entirely, so the hot-path Mark sites cost nothing.
#pragma warning disable 162
    internal sealed class TimingRecorder
    {
        public const bool Enabled = false;

        private static readonly TimingRecorder _instance = new TimingRecorder();
        public static TimingRecorder Instance => _instance;

        public enum FrameState
        {
            InProgress,
            Ok,
            DroppedCallbackUnknownChannel,
            DroppedCallbackException,
            DroppedEncodeQFull,
            DroppedEncodeException,
            DroppedSendQFull,
            DroppedUrlEmpty,
            DroppedHttpException,
            DroppedThrottled,
            IncompleteSwept,
        }

        private sealed class FrameRecord
        {
            public int Port;
            public long Created;
            public long Cb;
            public long EncIn;
            public long EncOut;
            public long Jpeg;
            public long SendIn;
            public long SendOut;
            public long Http;
            public bool HasCb;
            public bool HasEncIn;
            public bool HasEncOut;
            public bool HasJpeg;
            public bool HasSendIn;
            public bool HasSendOut;
            public bool HasHttp;
        }

        private const string LinePrefix = "[TIMING-CS]";
        private static readonly TimeSpan SweepInterval = TimeSpan.FromSeconds(1);
        private static readonly TimeSpan StaleAge = TimeSpan.FromSeconds(10);

        private readonly ConcurrentDictionary<ulong, FrameRecord> _records =
            new ConcurrentDictionary<ulong, FrameRecord>();

        private CancellationTokenSource _sweeperCts;
        private Task _sweeperTask;
        private bool _initialized;
        private bool _enabledRuntime;

        private TimingRecorder() { }

        public void Init(int basePort)
        {
            if (_initialized) return;
            _initialized = true;

            if (!Enabled) return;

            _enabledRuntime = true;
            _sweeperCts = new CancellationTokenSource();
            _sweeperTask = Task.Run(() => SweeperLoop(_sweeperCts.Token));
        }

        public void Shutdown()
        {
            if (!Enabled || !_enabledRuntime) return;
            _enabledRuntime = false;

            try { _sweeperCts?.Cancel(); } catch { }
            try { _sweeperTask?.Wait(TimeSpan.FromSeconds(2)); } catch { }

            List<KeyValuePair<ulong, FrameRecord>> leftover =
                new List<KeyValuePair<ulong, FrameRecord>>(_records.Count);
            foreach (var kv in _records) leftover.Add(kv);
            _records.Clear();
            foreach (var kv in leftover)
            {
                WriteLine(kv.Key, kv.Value, FrameState.IncompleteSwept);
            }
        }

        public void MarkCallbackEnter(ulong timestamp, int port)
        {
            if (!Enabled || !_enabledRuntime) return;
            long now = Stopwatch.GetTimestamp();
            FrameRecord r = _records.GetOrAdd(timestamp, _ => new FrameRecord());
            r.Created = now;
            r.Port = port;
            r.Cb = now;
            r.HasCb = true;
        }

        public void MarkEncodeQueueIn(ulong timestamp)
        {
            if (!Enabled || !_enabledRuntime) return;
            long now = Stopwatch.GetTimestamp();
            if (!_records.TryGetValue(timestamp, out FrameRecord r)) return;
            r.EncIn = now;
            r.HasEncIn = true;
        }

        public void MarkEncodeQueueOut(ulong timestamp)
        {
            if (!Enabled || !_enabledRuntime) return;
            long now = Stopwatch.GetTimestamp();
            if (!_records.TryGetValue(timestamp, out FrameRecord r)) return;
            r.EncOut = now;
            r.HasEncOut = true;
        }

        public void MarkJpegDone(ulong timestamp)
        {
            if (!Enabled || !_enabledRuntime) return;
            long now = Stopwatch.GetTimestamp();
            if (!_records.TryGetValue(timestamp, out FrameRecord r)) return;
            r.Jpeg = now;
            r.HasJpeg = true;
        }

        public void MarkSendQueueIn(ulong timestamp)
        {
            if (!Enabled || !_enabledRuntime) return;
            long now = Stopwatch.GetTimestamp();
            if (!_records.TryGetValue(timestamp, out FrameRecord r)) return;
            r.SendIn = now;
            r.HasSendIn = true;
        }

        public void MarkSendQueueOut(ulong timestamp)
        {
            if (!Enabled || !_enabledRuntime) return;
            long now = Stopwatch.GetTimestamp();
            if (!_records.TryGetValue(timestamp, out FrameRecord r)) return;
            r.SendOut = now;
            r.HasSendOut = true;
        }

        public void Flush(ulong timestamp, FrameState state)
        {
            if (!Enabled || !_enabledRuntime) return;
            long now = Stopwatch.GetTimestamp();
            if (!_records.TryRemove(timestamp, out FrameRecord r)) return;
            if (state == FrameState.Ok)
            {
                r.Http = now;
                r.HasHttp = true;
            }
            WriteLine(timestamp, r, state);
        }

        private async Task SweeperLoop(CancellationToken ct)
        {
            try
            {
                while (!ct.IsCancellationRequested)
                {
                    try { await Task.Delay(SweepInterval, ct).ConfigureAwait(false); }
                    catch (OperationCanceledException) { break; }

                    long cutoff = Stopwatch.GetTimestamp() - (long)(StaleAge.TotalSeconds * Stopwatch.Frequency);
                    List<ulong> stale = new List<ulong>();
                    foreach (var kv in _records)
                    {
                        if (kv.Value.Created < cutoff) stale.Add(kv.Key);
                    }
                    foreach (ulong ts in stale)
                    {
                        if (_records.TryRemove(ts, out FrameRecord r))
                        {
                            WriteLine(ts, r, FrameState.IncompleteSwept);
                        }
                    }
                }
            }
            catch { /* sweeper must never crash the process */ }
        }

        private void WriteLine(ulong timestamp, FrameRecord r, FrameState state)
        {
            // Suppress lines that would print zero segment values — e.g.
            // unknown-channel / encodeq-full drops have only t_cb and nothing
            // else, so every s_xxx column would be blank.
            bool anySegment = r.HasEncIn || r.HasEncOut || r.HasJpeg ||
                              r.HasSendIn || r.HasSendOut || r.HasHttp;
            if (!anySegment) return;

            StringBuilder sb = new StringBuilder(256);
            sb.Append(LinePrefix);
            sb.Append(" [stamp=").Append(timestamp);
            sb.Append(" port=").Append(r.Port);
            sb.Append(" state=").Append(StateName(state)).Append("]");
            AppendField(sb, "s_marshal",      r.HasCb,      r.HasEncIn,   r.Cb,      r.EncIn);
            AppendField(sb, "s_wait_encode",  r.HasEncIn,   r.HasEncOut,  r.EncIn,   r.EncOut);
            AppendField(sb, "s_jpeg",         r.HasEncOut,  r.HasJpeg,    r.EncOut,  r.Jpeg);
            AppendField(sb, "s_post_jpeg",    r.HasJpeg,    r.HasSendIn,  r.Jpeg,    r.SendIn);
            AppendField(sb, "s_wait_send",    r.HasSendIn,  r.HasSendOut, r.SendIn,  r.SendOut);
            AppendField(sb, "s_http",         r.HasSendOut, r.HasHttp,    r.SendOut, r.Http);
            AppendField(sb, "cs_total",       r.HasCb,      r.HasHttp,    r.Cb,      r.Http);

            // Console.WriteLine is internally thread-safe (Console.Out is a
            // SyncTextWriter wrapper), so concurrent writers from EncodeWorker
            // / SendWorker / sweeper don't interleave at the line level.
            string line = sb.ToString();
            try { Console.WriteLine(line); } catch { }
            FileLogger.Timing(line);
        }

        private static void AppendField(StringBuilder sb, string name, bool hasStart, bool hasEnd, long start, long end)
        {
            sb.Append(' ').Append(name).Append('=');
            if (hasStart && hasEnd)
            {
                double ms = (end - start) * 1000.0 / Stopwatch.Frequency;
                sb.Append(ms.ToString("F3", System.Globalization.CultureInfo.InvariantCulture));
            }
        }

        private static string StateName(FrameState s)
        {
            switch (s)
            {
                case FrameState.Ok:                            return "OK";
                case FrameState.DroppedCallbackUnknownChannel: return "dropped_callback_unknown_channel";
                case FrameState.DroppedCallbackException:      return "dropped_callback_exception";
                case FrameState.DroppedEncodeQFull:            return "dropped_encodeq_full";
                case FrameState.DroppedEncodeException:        return "dropped_encode_exception";
                case FrameState.DroppedSendQFull:              return "dropped_sendq_full";
                case FrameState.DroppedUrlEmpty:               return "dropped_url_empty";
                case FrameState.DroppedHttpException:          return "dropped_http_exception";
                case FrameState.DroppedThrottled:              return "dropped_throttled";
                case FrameState.IncompleteSwept:               return "incomplete_swept";
                case FrameState.InProgress:                    return "in_progress";
                default:                                       return "unknown";
            }
        }
    }
#pragma warning restore 162
}
