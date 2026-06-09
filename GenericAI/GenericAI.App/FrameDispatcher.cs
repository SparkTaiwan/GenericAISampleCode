using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace GenericAI.App
{
    internal struct RawDetection
    {
        public int Width;
        public int Height;
        public ulong Timestamp;
        public byte[] FrameI420;      // managed copy; safe to retain after callback returns
        public int FrameLength;
        public List<List<NativeInterop.ROI>> Rois;
        public int Port;              // routing key for shared EncodeWorker/SendWorker pool (spec §7: channel_id == port)
    }

    // Native callback target. Runs on the C++ DispatchThread; must copy the
    // YUV bytes off the native pool slot before returning so the slot can be
    // recycled, and must never throw — exceptions across the P/Invoke boundary
    // are undefined behaviour.
    internal sealed class FrameDispatcher
    {
        private readonly IReadOnlyDictionary<int, ChannelHandle> _byChannelId;
        private readonly DropCounter _drops;
        private readonly NativeInterop.CallBackFunction _delegate;
        private readonly int _roiSize = Marshal.SizeOf(typeof(NativeInterop.ROI));
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
            try
            {
                if (frameSize <= 0 || frameI420 == IntPtr.Zero || roisCount <= 0 || nodeCount <= 0)
                    return;

                ChannelHandle channel;
                if (!_byChannelId.TryGetValue(channelId, out channel))
                {
                    _drops.IncCallbackDropped();
                    FileLogger.Warn($"Detection callback: unknown channel_id={channelId}");
                    return;
                }

                byte[] managed = new byte[frameSize];
                Marshal.Copy(frameI420, managed, 0, frameSize);

                List<List<NativeInterop.ROI>> grouped = new List<List<NativeInterop.ROI>>(roisCount);
                for (int i = 0; i < roisCount; i++)
                {
                    List<NativeInterop.ROI> group = new List<NativeInterop.ROI>(nodeCount);
                    for (int j = 0; j < nodeCount; j++)
                    {
                        IntPtr p = IntPtr.Add(roisFlat, (i * nodeCount + j) * _roiSize);
                        var roi = (NativeInterop.ROI)Marshal.PtrToStructure(p, typeof(NativeInterop.ROI));
                        group.Add(roi);
                    }
                    grouped.Add(group);
                }

                RawDetection raw = new RawDetection
                {
                    Width = width,
                    Height = height,
                    Timestamp = timestamp,
                    FrameI420 = managed,
                    FrameLength = frameSize,
                    Rois = grouped,
                    Port = channelId,
                };

                // Blocking Add: pressure backs up to the native DispatchThread,
                // through dispatch_q -> InferLoop -> infer_q -> MmfReader ->
                // share memory. Wrapper never drops frames on its own.
                // CompleteAdding (shutdown) wakes a blocked Add with
                // InvalidOperationException, caught by the outer try/catch.
                channel.EncodeQ.Add(raw);

                FileLogger.Info($"Detection callback: ch={channelId} size={frameSize} w={width} h={height} rois={roisCount} nodes={nodeCount}");
            }
            catch (InvalidOperationException)
            {
                // EncodeQ.CompleteAdding called during shutdown — expected
                // when an in-flight callback was blocked on Add. Return
                // silently; no log, no drop counter (this is not a failure).
            }
            catch (Exception ex)
            {
                // Never let an exception unwind into the native caller.
                try { FileLogger.Error("FrameDispatcher.OnNativeCallback", ex); } catch { }
            }
        }
    }
}
