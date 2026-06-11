using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace GenericAI.App
{
    internal struct RawDetection
    {
        public int Width;
        public int Height;
        public ulong Timestamp;
        public byte[] FrameI420;      // rented from FrameDispatcher.FramePool; EncodeWorker returns it in finally
        public int FrameLength;
        public NativeInterop.ROI[] RoisFlat;  // length = RoisCount * NodeCount
        public int RoisCount;
        public int NodeCount;
        public int Port;              // routing key for shared EncodeWorker/SendWorker pool (spec §7: channel_id == port)
    }

    // Native callback target. Runs on the C++ DispatchThread; must copy the
    // YUV bytes off the native pool slot before returning so the slot can be
    // recycled, and must never throw — exceptions across the P/Invoke boundary
    // are undefined behaviour.
    internal sealed class FrameDispatcher
    {
        // 16 MB 桶上限：4K I420 = 12 MB；下一個 2 的次方桶是 16 MB。
        // 超過上限（5K/8K 未來情境）會 fall back 為 new byte[]，不出錯、無池化效益。
        internal const int FramePoolMaxBytes = 16 * 1024 * 1024;
        internal static readonly ArrayPool<byte> FramePool =
            ArrayPool<byte>.Create(maxArrayLength: FramePoolMaxBytes, maxArraysPerBucket: 16);

        private readonly IReadOnlyDictionary<int, ChannelHandle> _byChannelId;
        private readonly DropCounter _drops;
        private readonly NativeInterop.CallBackFunction _delegate;
        private GCHandle _delegateHandle;
        private bool _registered;

        public FrameDispatcher(IReadOnlyDictionary<int, ChannelHandle> byChannelId, DropCounter drops)
        {
            _byChannelId = byChannelId;
            _drops = drops;
            _delegate = OnNativeCallback;
        }

        public void Register()
        {
            // Pin the delegate so the GC cannot move/collect it while the
            // native side holds a function pointer to it.
            _delegateHandle = GCHandle.Alloc(_delegate);
            NativeInterop.GAI_RegisterCallback(_delegate);
            _registered = true;
        }

        public void Unregister()
        {
            if (!_registered) return;
            try { NativeInterop.GAI_RegisterCallback(null); } catch { }
            try { _delegateHandle.Free(); } catch { }
            _registered = false;
        }

        private void OnNativeCallback(
            int channelId, int width, int height,
            IntPtr frameI420, int frameSize,
            ulong timestamp,
            IntPtr roisFlat, int roisCount, int nodeCount)
        {
            // Tracks pool ownership: non-null means we hold a rented buffer
            // that has not yet been handed to EncodeQ. Set back to null after
            // EncodeQ.Add succeeds so the finally Return is skipped.
            byte[] managed = null;
            try
            {
                if (frameSize <= 0 || frameI420 == IntPtr.Zero || roisCount <= 0 || nodeCount <= 0)
                    return;

                TimingRecorder.Instance.MarkCallbackEnter(timestamp, channelId);

                ChannelHandle channel;
                if (!_byChannelId.TryGetValue(channelId, out channel))
                {
                    _drops.IncCallbackDropped();
                    FileLogger.Warn($"Detection callback: unknown channel_id={channelId}");
                    TimingRecorder.Instance.Flush(timestamp, TimingRecorder.FrameState.DroppedCallbackUnknownChannel);
                    return;
                }

                // Rent only after channel lookup so the early-exit paths above
                // never have to remember to Return.
                managed = FramePool.Rent(frameSize);
                Marshal.Copy(frameI420, managed, 0, frameSize);

                int total = roisCount * nodeCount;
                NativeInterop.ROI[] roiArr = new NativeInterop.ROI[total];
                unsafe
                {
                    NativeInterop.ROI* src = (NativeInterop.ROI*)roisFlat.ToPointer();
                    for (int k = 0; k < total; k++) roiArr[k] = src[k];
                }

                RawDetection raw = new RawDetection
                {
                    Width = width,
                    Height = height,
                    Timestamp = timestamp,
                    FrameI420 = managed,
                    FrameLength = frameSize,
                    RoisFlat = roiArr,
                    RoisCount = roisCount,
                    NodeCount = nodeCount,
                    Port = channelId,
                };

                // Blocking Add: pressure backs up to the native DispatchThread,
                // through dispatch_q -> InferLoop -> infer_q -> MmfReader ->
                // share memory. Wrapper never drops frames on its own.
                // CompleteAdding (shutdown) wakes a blocked Add with
                // InvalidOperationException, caught by the outer try/catch.
                channel.EncodeQ.Add(raw);
                managed = null;  // ownership transferred to EncodeWorker
                TimingRecorder.Instance.MarkEncodeQueueIn(timestamp);

                FileLogger.Info($"Detection callback: ch={channelId} size={frameSize} w={width} h={height} rois={roisCount} nodes={nodeCount}");
            }
            catch (InvalidOperationException)
            {
                // EncodeQ.CompleteAdding called during shutdown — expected
                // when an in-flight callback was blocked on Add. Return
                // silently; no log, no drop counter (this is not a failure).
                // managed (if rented) returned via finally.
            }
            catch (Exception ex)
            {
                // Never let an exception unwind into the native caller.
                try { TimingRecorder.Instance.Flush(timestamp, TimingRecorder.FrameState.DroppedCallbackException); } catch { }
                try { FileLogger.Error("FrameDispatcher.OnNativeCallback", ex); } catch { }
            }
            finally
            {
                if (managed != null)
                {
                    // Buffer never made it onto EncodeQ; return it here so the
                    // pool does not degrade.
                    try { FramePool.Return(managed, clearArray: false); } catch { }
                }
            }
        }
    }
}
