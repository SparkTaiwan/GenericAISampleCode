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
                    while (true)
                    {
                        RawDetection raw;
                        int idx = BlockingCollection<RawDetection>.TakeFromAny(_allEncodeQs, out raw, ct);
                        if (idx < 0) break;

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

                            if (!channel.SendQ.TryAdd(env))
                            {
                                _drops.IncEncodeDropped();
                                TimingRecorder.Instance.Flush(raw.Timestamp, TimingRecorder.FrameState.DroppedSendQFull);
                            }
                            else
                            {
                                TimingRecorder.Instance.MarkSendQueueIn(raw.Timestamp);
                            }
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
