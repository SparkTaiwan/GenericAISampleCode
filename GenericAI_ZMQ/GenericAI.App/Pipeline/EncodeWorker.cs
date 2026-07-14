using System;
using System.Collections.Concurrent;
using System.Threading;
using System.Threading.Tasks;

namespace GenericAI.App
{
    // Process-wide encode worker. Drains all channels' _encodeQ via
    // BlockingCollection<T>.TakeFromAny; the returned idx names the source
    // queue, which is parallel to _channelsByIdx — no per-frame dict hash.
    internal sealed class EncodeWorker
    {
        private readonly BlockingCollection<RawDetection>[] _allEncodeQs;
        private readonly ChannelHandle[] _channelsByIdx;
        private readonly DropCounter _drops;

        public EncodeWorker(BlockingCollection<RawDetection>[] allEncodeQs,
                            ChannelHandle[] channelsByIdx,
                            DropCounter drops)
        {
            _allEncodeQs = allEncodeQs;
            _channelsByIdx = channelsByIdx;
            _drops = drops;
        }

        public Task RunAsync(CancellationToken ct)
        {
            return Task.Run(() =>
            {
                try
                {
                    int cursor = 0;
                    while (!ct.IsCancellationRequested)
                    {
                        RawDetection raw;
                        int idx = RoundRobinTaker.TryTakeRoundRobin(_allEncodeQs, ref cursor, out raw);
                        if (idx == -1) break;
                        if (idx == -2)
                        {
                            // All queues empty but some still open. WaitOne
                            // returns true if ct gets cancelled, false on timeout —
                            // no try/catch needed for cancellation here. 5 ms idle
                            // poll: a few ms of empty-to-busy wake-up latency in
                            // exchange for not spinning at 1 kHz per worker.
                            if (ct.WaitHandle.WaitOne(5)) break;
                            continue;
                        }

                        TimingRecorder.Instance.MarkEncodeQueueOut(raw.Timestamp);

                        ChannelHandle channel = _channelsByIdx[idx];

                        // Trigger-interval throttle: at most one send per trigger_interval seconds per
                        // channel (0 = no limit). Checked BEFORE the JPEG encode so suppressed frames
                        // cost nothing. Both schemas (motion and object detection) define trigger_interval
                        // with default 1, so this throttle applies to both detectors.
                        if (!channel.TryPassTriggerInterval(channel.Parameters.TriggerIntervalSec))
                        {
                            try { FrameDispatcher.FramePool.Return(raw.FrameI420, clearArray: false); } catch { }
                            try { TimingRecorder.Instance.Flush(raw.Timestamp, TimingRecorder.FrameState.DroppedThrottled); } catch { }
                            continue;
                        }

                        byte[] buf = raw.FrameI420;
                        try
                        {
                            try
                            {
                                int quality = channel.Parameters.JpgQuality;
                                if (quality <= 0) quality = 30;

                                // fixed is cheaper than GCHandle.Alloc(Pinned) —
                                // no handle-table round trip per frame; the buffer
                                // only needs to stay pinned for the native call.
                                byte[] jpeg;
                                unsafe
                                {
                                    fixed (byte* p = buf)
                                    {
                                        jpeg = TurboJpegInterop.EncodeI420(
                                            (IntPtr)p, raw.FrameLength,
                                            raw.Width, raw.Height, quality);
                                    }
                                }

                                TimingRecorder.Instance.MarkJpegDone(raw.Timestamp);

                                // Per-class counts -> metadata items. The item name is the schema key
                                // (SupportedClasses value == SettingsSchema `classes` option, e.g. "person"):
                                // the schema's counting list already tells the recorder which items count,
                                // so there is NO "__Count" suffix -- the name is sent exactly as the schema key.
                                System.Collections.Generic.List<HttpEnvelope.EnvelopeItem> items = null;
                                string detectSummary = null;   // "person=3, car=1" for the console, so recognition can be eyeballed
                                if (raw.ClassCounts != null)
                                {
                                    int m = Math.Min(raw.ClassCounts.Length, NativeInterop.SupportedClasses.Length);
                                    for (int i = 0; i < m; i++)
                                    {
                                        if (raw.ClassCounts[i] <= 0) continue;
                                        if (items == null) items = new System.Collections.Generic.List<HttpEnvelope.EnvelopeItem>();
                                        items.Add(new HttpEnvelope.EnvelopeItem
                                        {
                                            name = NativeInterop.SupportedClasses[i],
                                            value = raw.ClassCounts[i].ToString(),
                                        });
                                        detectSummary = (detectSummary == null ? "" : detectSummary + ", ")
                                            + NativeInterop.SupportedClasses[i] + "=" + raw.ClassCounts[i];
                                    }
                                }

                                // Object-detection: show WHAT was recognized on this channel (only when
                                // there are class counts; motion has none and is covered by SendWorker's
                                // "Detected!!" line). Lets you confirm the recognition is correct.
                                if (detectSummary != null)
                                    ConsoleLog.WriteLine($"[ch{raw.Port}] Recognized: {detectSummary}");

                                HttpEnvelope env = new HttpEnvelope
                                {
                                    version = Protocol.Version,
                                    port_num = raw.Port,
                                    keyframe = jpeg,
                                    timestamp = raw.Timestamp,
                                    RoisFlat = raw.RoisFlat,
                                    RoisCount = raw.RoisCount,
                                    NodeCount = raw.NodeCount,
                                    items = items,
                                };

                                // Blocking Add: pressure backs up to this worker,
                                // through EncodeQ -> callback -> native pipeline ->
                                // share memory. Wrapper never drops frames on its own.
                                // CompleteAdding (shutdown) wakes a blocked Add with
                                // InvalidOperationException, caught by the outer catch.
                                channel.SendQ.Add(env);
                                TimingRecorder.Instance.MarkSendQueueIn(raw.Timestamp);
                            }
                            catch (InvalidOperationException)
                            {
                                // SendQ.CompleteAdding called during shutdown —
                                // expected when this worker was blocked on Add.
                                // Exit loop quietly; next TakeFromAny would also
                                // unblock via the cancellation token.
                                return;
                            }
                            catch (Exception ex)
                            {
                                FileLogger.Error("EncodeWorker frame failed", ex);
                                try { TimingRecorder.Instance.Flush(raw.Timestamp, TimingRecorder.FrameState.DroppedEncodeException); } catch { }
                            }
                        }
                        finally
                        {
                            // Envelope holds RoisFlat and the base64 string;
                            // it no longer references the I420 buffer, so the
                            // pool buffer can be returned here unconditionally.
                            try { FrameDispatcher.FramePool.Return(buf, clearArray: false); } catch { }
                        }
                    }
                }
                catch (OperationCanceledException) { /* shutdown */ }
                catch (Exception ex)
                {
                    FileLogger.Error("EncodeWorker fatal", ex);
                }
                finally
                {
                    TurboJpegInterop.ReleaseThreadHandle();
                }
            }, ct);
        }
    }
}
