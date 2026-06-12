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

                        byte[] buf = raw.FrameI420;
                        try
                        {
                            try
                            {
                                int quality = channel.Parameters.JpgQuality;
                                if (quality <= 0) quality = 50;

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

                                HttpEnvelope env = new HttpEnvelope
                                {
                                    version = "1.2",
                                    port_num = raw.Port,
                                    keyframe = jpeg,
                                    timestamp = raw.Timestamp,
                                    RoisFlat = raw.RoisFlat,
                                    RoisCount = raw.RoisCount,
                                    NodeCount = raw.NodeCount,
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
