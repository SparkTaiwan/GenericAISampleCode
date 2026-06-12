using System.Collections.Concurrent;

namespace GenericAI.App
{
    // Fair round-robin replacement for BlockingCollection<T>.TakeFromAny.
    // TakeFromAny always probes from index 0 on each wake, so when every
    // channel has frames queued the worker only ever drains ch0/ch1 and the
    // later channels starve. This helper tries queues in (cursor, cursor+1, ...)
    // order and advances the cursor past whatever was taken, giving every
    // queue equal long-run service.
    internal static class RoundRobinTaker
    {
        // Return values:
        //   idx >= 0 : hit. `item` is the taken element; cursor advanced to (idx + 1) % n.
        //   -1       : all queues empty AND all IsCompleted (CompleteAdding + drained).
        //              Caller should exit its loop.
        //   -2       : all queues empty but at least one is still open.
        //              Caller should wait briefly and try again.
        public static int TryTakeRoundRobin<T>(
            BlockingCollection<T>[] queues, ref int cursor, out T item)
        {
            int n = queues.Length;
            for (int i = 0; i < n; i++)
            {
                int probe = (cursor + i) % n;
                if (queues[probe].TryTake(out item))
                {
                    cursor = (probe + 1) % n;
                    return probe;
                }
            }

            item = default(T);

            // IsCompleted == IsAddingCompleted && Count == 0, which is the
            // "no more items will ever arrive" signal we need.
            for (int i = 0; i < n; i++)
            {
                if (!queues[i].IsCompleted) return -2;
            }
            return -1;
        }
    }
}
