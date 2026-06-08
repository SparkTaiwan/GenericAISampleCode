using System;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
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
                    // Fair round-robin TakeFromAny replacement. .NET's
                    // BlockingCollection<T>.TakeFromAny always probes from
                    // index 0 on each wake, so when every channel has frames
                    // queued the worker only ever drains ch0/ch1 and the
                    // later channels starve. We track a per-worker cursor
                    // and try queues in (cursor, cursor+1, ...) order, then
                    // advance the cursor past whatever we took.
                    int n = _allEncodeQs.Length;
                    int cursor = 0;
                    while (!ct.IsCancellationRequested)
                    {
                        RawDetection raw = default(RawDetection);
                        int idx = -1;
                        for (int i = 0; i < n; i++)
                        {
                            int probe = (cursor + i) % n;
                            if (_allEncodeQs[probe].TryTake(out raw))
                            {
                                idx = probe;
                                cursor = (probe + 1) % n;
                                break;
                            }
                        }

                        if (idx < 0)
                        {
                            // All queues empty. Exit if every queue is
                            // closed (CompleteAdding + drained); otherwise
                            // wait briefly. WaitOne(1) returns true if ct
                            // gets cancelled, false on timeout — no try /
                            // catch needed for cancellation here.
                            bool allDone = true;
                            for (int i = 0; i < n; i++)
                            {
                                if (!_allEncodeQs[i].IsCompleted) { allDone = false; break; }
                            }
                            if (allDone) break;
                            if (ct.WaitHandle.WaitOne(1)) break;
                            continue;
                        }

                        TimingRecorder.Instance.MarkEncodeQueueOut(raw.Timestamp);

                        ChannelHandle channel = _channelsByIdx[idx];

                        try
                        {
                            int quality = channel.Parameters.JpgQuality;
                            if (quality <= 0) quality = 50;

                            GCHandle pinned = GCHandle.Alloc(raw.FrameI420, GCHandleType.Pinned);
                            byte[] jpeg;
                            try
                            {
                                jpeg = TurboJpegInterop.EncodeI420(
                                    pinned.AddrOfPinnedObject(), raw.FrameLength,
                                    raw.Width, raw.Height, quality);
                            }
                            finally
                            {
                                pinned.Free();
                            }

                            TimingRecorder.Instance.MarkJpegDone(raw.Timestamp);

                            HttpEnvelope env = new HttpEnvelope
                            {
                                version = "1.2",
                                port_num = raw.Port,
                                keyframe = Convert.ToBase64String(jpeg),
                                timestamp = raw.Timestamp,
                                rois_rects = raw.Rois,
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
