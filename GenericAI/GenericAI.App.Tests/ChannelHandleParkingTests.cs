using System;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace GenericAI.App.Tests
{
    [TestClass]
    public class ChannelHandleParkingTests
    {
        private static readonly TimeSpan Elapsed = TimeSpan.FromSeconds(-1);
        private static readonly TimeSpan NotElapsed = TimeSpan.FromHours(1);

        [TestMethod]
        public void Claim_NothingParked_Fails()
        {
            var ch = new ChannelHandle(46000);

            byte[] payload;
            Assert.IsFalse(ch.HasParkedSend);
            Assert.IsFalse(ch.TryClaimParkedSend(out payload));
        }

        [TestMethod]
        public void Claim_BeforeBackoffElapses_Fails()
        {
            var ch = new ChannelHandle(46000);
            ch.ParkSend(new byte[] { 1 }, NotElapsed);

            byte[] payload;
            Assert.IsTrue(ch.HasParkedSend);
            Assert.IsFalse(ch.TryClaimParkedSend(out payload));
            Assert.IsTrue(ch.HasParkedSend);  // still parked, nothing consumed
        }

        [TestMethod]
        public void Claim_AfterBackoffElapses_ReturnsParkedPayload()
        {
            var ch = new ChannelHandle(46000);
            byte[] parked = { 1, 2, 3 };
            ch.ParkSend(parked, Elapsed);

            byte[] payload;
            Assert.IsTrue(ch.TryClaimParkedSend(out payload));
            Assert.AreSame(parked, payload);
            Assert.IsFalse(ch.HasParkedSend);
        }

        [TestMethod]
        public void Claim_WhileAnotherClaimHeld_Fails()
        {
            var ch = new ChannelHandle(46000);
            ch.ParkSend(new byte[] { 1 }, Elapsed);
            ch.ParkSend(new byte[] { 2 }, Elapsed);

            byte[] first;
            Assert.IsTrue(ch.TryClaimParkedSend(out first));

            // Second payload is parked and due, but the claim flag is held —
            // a concurrent worker must not get it.
            byte[] second;
            Assert.IsFalse(ch.TryClaimParkedSend(out second));

            ch.CompleteParkedSend(first, success: true, backoff: NotElapsed);
            Assert.IsTrue(ch.TryClaimParkedSend(out second));
            Assert.AreEqual(2, second[0]);
        }

        [TestMethod]
        public void CompleteFailure_ReparksWithFreshBackoff()
        {
            var ch = new ChannelHandle(46000);
            byte[] parked = { 7 };
            ch.ParkSend(parked, Elapsed);

            byte[] payload;
            Assert.IsTrue(ch.TryClaimParkedSend(out payload));
            ch.CompleteParkedSend(payload, success: false, backoff: NotElapsed);

            Assert.IsTrue(ch.HasParkedSend);
            byte[] again;
            Assert.IsFalse(ch.TryClaimParkedSend(out again));  // new backoff window
        }

        [TestMethod]
        public void CompleteSuccess_LeavesChannelUnblocked()
        {
            var ch = new ChannelHandle(46000);
            ch.ParkSend(new byte[] { 7 }, Elapsed);

            byte[] payload;
            Assert.IsTrue(ch.TryClaimParkedSend(out payload));
            ch.CompleteParkedSend(payload, success: true, backoff: NotElapsed);

            Assert.IsFalse(ch.HasParkedSend);
            byte[] again;
            Assert.IsFalse(ch.TryClaimParkedSend(out again));
        }

        [TestMethod]
        public void ParkedPayloads_RetryInFifoOrder()
        {
            var ch = new ChannelHandle(46000);
            ch.ParkSend(new byte[] { 1 }, Elapsed);
            ch.ParkSend(new byte[] { 2 }, Elapsed);

            byte[] payload;
            Assert.IsTrue(ch.TryClaimParkedSend(out payload));
            Assert.AreEqual(1, payload[0]);
            ch.CompleteParkedSend(payload, success: true, backoff: Elapsed);

            Assert.IsTrue(ch.TryClaimParkedSend(out payload));
            Assert.AreEqual(2, payload[0]);
        }
    }
}
