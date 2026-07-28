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

                                // draw_roi (from the perimeter's draw_rect_on_jpg flag): overlay the
                                // detected ROI/box outlines onto the keyframe's Y plane BEFORE JPEG
                                // encode. RoisFlat is in frame coordinates (RoisCount groups x
                                // NodeCount points). Drawing on buf is safe — detection already
                                // consumed this frame; the buffer is returned to the pool after encode.
                                if (channel.Parameters.DrawRoi && raw.RoisFlat != null
                                    && raw.RoisCount > 0 && raw.NodeCount > 1)
                                {
                                    DrawRoisOnI420(buf, raw.Width, raw.Height,
                                                   raw.RoisFlat, raw.RoisCount, raw.NodeCount);
                                }

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

        // ---- draw_roi keyframe overlay -------------------------------------------
        // Draws each ROI group's outline (closed polygon) onto the I420 frame as a 2px
        // RED line. Red in BT.601 is Y=76, U=85, V=255, so the Y plane and both chroma
        // planes are written. Points are in frame coordinates; groups are laid out as
        // RoisCount blocks of NodeCount points (person = 4-corner quads, motion =
        // polygon vertices).
        private const byte kRoiY = 76;    // red luma
        private const byte kRoiU = 85;    // red Cb
        private const byte kRoiV = 255;   // red Cr

        private static void DrawRoisOnI420(byte[] frame, int width, int height,
                                           NativeInterop.ROI[] rois, int roisCount, int nodeCount)
        {
            if (frame == null || rois == null || width <= 0 || height <= 0) return;
            long need = (long)roisCount * nodeCount;
            if (need <= 0 || rois.Length < need) return;
            // I420 layout: Y (w*h), then U (cw*ch), then V (cw*ch).
            int ySize = width * height;
            int cw = width / 2, ch = height / 2;
            int uOff = ySize, vOff = ySize + cw * ch;
            if (frame.Length < (long)ySize + 2L * cw * ch) return;   // not a full I420 buffer

            for (int r = 0; r < roisCount; r++)
            {
                int baseIdx = r * nodeCount;
                for (int n = 0; n < nodeCount; n++)
                {
                    NativeInterop.ROI p0 = rois[baseIdx + n];
                    NativeInterop.ROI p1 = rois[baseIdx + (n + 1) % nodeCount];   // close the polygon
                    DrawLine(frame, width, height, cw, ch, uOff, vOff, p0.x, p0.y, p1.x, p1.y);
                }
            }
        }

        // Bresenham line, 2px thick, red.
        private static void DrawLine(byte[] frame, int width, int height, int cw, int ch, int uOff, int vOff,
                                     int x0, int y0, int x1, int y1)
        {
            int dx = Math.Abs(x1 - x0), dy = Math.Abs(y1 - y0);
            int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
            int err = dx - dy;
            // Bound the iteration count so a stray (large/negative) coordinate can't spin.
            int guard = dx + dy + 4;
            while (guard-- > 0)
            {
                PlotRed(frame, width, height, cw, ch, uOff, vOff, x0, y0);
                if (x0 == x1 && y0 == y1) break;
                int e2 = 2 * err;
                if (e2 > -dy) { err -= dy; x0 += sx; }
                if (e2 < dx) { err += dx; y0 += sy; }
            }
        }

        private static void PlotRed(byte[] frame, int width, int height, int cw, int ch, int uOff, int vOff,
                                    int x, int y)
        {
            for (int oy = 0; oy <= 1; oy++)
            {
                int yy = y + oy;
                if (yy < 0 || yy >= height) continue;
                int rowBase = yy * width;
                for (int ox = 0; ox <= 1; ox++)
                {
                    int xx = x + ox;
                    if (xx < 0 || xx >= width) continue;
                    frame[rowBase + xx] = kRoiY;                       // Y
                    int cx = xx >> 1, cy = yy >> 1;                    // 4:2:0 subsampling
                    if (cx < cw && cy < ch)
                    {
                        int cIdx = cy * cw + cx;
                        frame[uOff + cIdx] = kRoiU;                    // U
                        frame[vOff + cIdx] = kRoiV;                    // V
                    }
                }
            }
        }
    }
}
