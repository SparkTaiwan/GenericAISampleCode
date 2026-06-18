using System;
using System.Threading;

namespace GenericAI.App
{
    // Lock-free counter for drops at each pipeline stage. Reported periodically
    // by Program.cs so the operator sees backpressure without having to grep.
    internal sealed class DropCounter
    {
        private long _callbackDropped;
        private long _encodeDropped;
        private long _sendDropped;

        public void IncCallbackDropped() { Interlocked.Increment(ref _callbackDropped); }
        public void IncEncodeDropped()   { Interlocked.Increment(ref _encodeDropped); }
        public void IncSendDropped()     { Interlocked.Increment(ref _sendDropped); }

        public (long callback, long encode, long send) Snapshot()
        {
            return (Interlocked.Read(ref _callbackDropped),
                    Interlocked.Read(ref _encodeDropped),
                    Interlocked.Read(ref _sendDropped));
        }
    }
}
