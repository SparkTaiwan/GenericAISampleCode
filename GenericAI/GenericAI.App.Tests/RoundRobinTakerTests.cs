using System.Collections.Concurrent;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace GenericAI.App.Tests
{
    [TestClass]
    public class RoundRobinTakerTests
    {
        private static BlockingCollection<int>[] MakeQueues(int count)
        {
            var queues = new BlockingCollection<int>[count];
            for (int i = 0; i < count; i++) queues[i] = new BlockingCollection<int>();
            return queues;
        }

        [TestMethod]
        public void Take_FromCursorPosition_AdvancesCursorPastTaken()
        {
            var queues = MakeQueues(3);
            queues[0].Add(100);
            queues[1].Add(101);
            queues[2].Add(102);
            int cursor = 0;

            int item;
            Assert.AreEqual(0, RoundRobinTaker.TryTakeRoundRobin(queues, ref cursor, out item));
            Assert.AreEqual(100, item);
            Assert.AreEqual(1, cursor);

            Assert.AreEqual(1, RoundRobinTaker.TryTakeRoundRobin(queues, ref cursor, out item));
            Assert.AreEqual(101, item);
            Assert.AreEqual(2, cursor);

            Assert.AreEqual(2, RoundRobinTaker.TryTakeRoundRobin(queues, ref cursor, out item));
            Assert.AreEqual(102, item);
            Assert.AreEqual(0, cursor);
        }

        [TestMethod]
        public void Take_SkipsEmptyQueues_CursorLandsAfterTakenQueue()
        {
            var queues = MakeQueues(3);
            queues[1].Add(42);
            int cursor = 0;

            int item;
            Assert.AreEqual(1, RoundRobinTaker.TryTakeRoundRobin(queues, ref cursor, out item));
            Assert.AreEqual(42, item);
            Assert.AreEqual(2, cursor);  // (probe + 1) % n, not cursor + 1
        }

        [TestMethod]
        public void Take_WrapsAroundFromLastCursor()
        {
            var queues = MakeQueues(3);
            queues[0].Add(7);
            int cursor = 2;

            int item;
            Assert.AreEqual(0, RoundRobinTaker.TryTakeRoundRobin(queues, ref cursor, out item));
            Assert.AreEqual(7, item);
            Assert.AreEqual(1, cursor);
        }

        [TestMethod]
        public void AllEmpty_AllCompleted_ReturnsMinusOne()
        {
            var queues = MakeQueues(3);
            foreach (var q in queues) q.CompleteAdding();
            int cursor = 0;

            int item;
            Assert.AreEqual(-1, RoundRobinTaker.TryTakeRoundRobin(queues, ref cursor, out item));
            Assert.AreEqual(default(int), item);
            Assert.AreEqual(0, cursor);
        }

        [TestMethod]
        public void AllEmpty_AnyStillOpen_ReturnsMinusTwo()
        {
            var queues = MakeQueues(3);
            queues[0].CompleteAdding();
            queues[2].CompleteAdding();
            int cursor = 0;

            int item;
            Assert.AreEqual(-2, RoundRobinTaker.TryTakeRoundRobin(queues, ref cursor, out item));
            Assert.AreEqual(0, cursor);
        }

        [TestMethod]
        public void CompletedQueueWithResidualItem_StillTaken()
        {
            var queues = MakeQueues(2);
            queues[1].Add(9);
            queues[1].CompleteAdding();  // completed but not yet drained
            queues[0].CompleteAdding();
            int cursor = 0;

            int item;
            Assert.AreEqual(1, RoundRobinTaker.TryTakeRoundRobin(queues, ref cursor, out item));
            Assert.AreEqual(9, item);

            // Residual drained; now everything is empty + completed.
            Assert.AreEqual(-1, RoundRobinTaker.TryTakeRoundRobin(queues, ref cursor, out item));
        }
    }
}
