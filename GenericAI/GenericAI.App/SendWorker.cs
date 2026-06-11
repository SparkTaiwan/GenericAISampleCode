using System;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;

namespace GenericAI.App
{
    // Process-wide send worker. Scans channels in round-robin order; a fresh
    // envelope gets one POST attempt, and on failure its payload is parked on
    // the owning channel (ChannelHandle.ParkSend) instead of being retried in
    // place. A channel with a parked payload is "blocked": no new envelopes
    // are taken from it until the parked one finally goes through, so a dead
    // analytics URL backpressures that channel only (SendQ -> EncodeQ ->
    // native pipeline) and can no longer wedge every worker in the shared
    // pool. Frames are still never dropped. spark.recorder keys results by
    // timestamp, so the per-channel reordering parking can introduce is
    // harmless.
    internal sealed class SendWorker
    {
        private static readonly TimeSpan RetryBackoff = TimeSpan.FromSeconds(1);

        private readonly ChannelHandle[] _channelsByIdx;
        private readonly HttpPostClient _client;
        private readonly DropCounter _drops;

        public SendWorker(ChannelHandle[] channelsByIdx,
                          HttpPostClient client,
                          DropCounter drops)
        {
            _channelsByIdx = channelsByIdx;
            _client = client;
            _drops = drops;
        }

        public Task RunAsync(CancellationToken ct)
        {
            return Task.Run(async () =>
            {
                try
                {
                    int n = _channelsByIdx.Length;
                    int cursor = 0;
                    while (!ct.IsCancellationRequested)
                    {
                        int worked = -1;
                        for (int i = 0; i < n; i++)
                        {
                            int idx = (cursor + i) % n;
                            ChannelHandle channel = _channelsByIdx[idx];

                            // URL not yet configured (or cleared): leave the
                            // envelopes queued so backpressure reaches the
                            // native pipeline; taking one out just to hold it
                            // would tie this worker to the channel.
                            string url = channel.Parameters.Url;
                            if (string.IsNullOrEmpty(url)) continue;

                            // Parked payload first: one attempt per backoff
                            // window, by whichever worker visits first (the
                            // claim flag keeps it to one worker at a time).
                            byte[] parked;
                            if (channel.TryClaimParkedSend(out parked))
                            {
                                bool ok = false;
                                try
                                {
                                    ok = await TrySendOnceAsync(url, parked, channel.Port, ct).ConfigureAwait(false);
                                }
                                finally
                                {
                                    channel.CompleteParkedSend(parked, ok, RetryBackoff);
                                }
                                worked = idx;
                                break;
                            }

                            // Blocked channel (parked payload waiting out its
                            // backoff, or another worker is retrying it): take
                            // nothing new — this is the isolation that keeps
                            // one dead URL from spreading to other channels.
                            if (channel.HasParkedSend) continue;

                            HttpEnvelope env;
                            if (!channel.SendQ.TryTake(out env)) continue;

                            // Serialise once per envelope — the JSON (base64
                            // keyframe included) is the largest allocation on
                            // this path, and the same bytes are reused across
                            // retries via the parking queue.
                            byte[] payload = Encoding.UTF8.GetBytes(JsonConvert.SerializeObject(env));
                            if (!await TrySendOnceAsync(url, payload, channel.Port, ct).ConfigureAwait(false))
                            {
                                channel.ParkSend(payload, RetryBackoff);
                            }
                            worked = idx;
                            break;
                        }

                        if (worked >= 0)
                        {
                            cursor = (worked + 1) % n;
                            continue;
                        }

                        // Nothing serviceable this scan. Exit once every queue
                        // is completed and drained (parked payloads are
                        // abandoned — by then shutdown has already cancelled
                        // ct, retrying them is hopeless).
                        bool allCompleted = true;
                        for (int i = 0; i < n; i++)
                        {
                            if (!_channelsByIdx[i].SendQ.IsCompleted)
                            {
                                allCompleted = false;
                                break;
                            }
                        }
                        if (allCompleted) break;

                        // 5 ms idle poll: each Task.Delay allocates a timer +
                        // promise, so 1 ms here meant ~1,000 allocations per
                        // second per idle worker for nothing.
                        try { await Task.Delay(5, ct).ConfigureAwait(false); }
                        catch (OperationCanceledException) { break; }
                    }
                }
                catch (OperationCanceledException) { /* shutdown */ }
                catch (Exception ex)
                {
                    FileLogger.Error("SendWorker fatal", ex);
                }
            }, ct);
        }

        private async Task<bool> TrySendOnceAsync(string url, byte[] payload, int port, CancellationToken ct)
        {
            try
            {
                await _client.PostAsync(url, payload).ConfigureAwait(false);
                Console.WriteLine("Detected!! send analytics result to server!!");
                FileLogger.Info($"Analytics result posted ok (ch={port})");
                return true;
            }
            catch (Exception ex) when (!ct.IsCancellationRequested)
            {
                // Transient or persistent HTTP failure (5s client timeout,
                // refused connection, 5xx, DNS, ...). Warn (not Error) since
                // parking + retry is the expected behaviour.
                FileLogger.Warn($"SendWorker POST failed (ch={port}), parked for retry: {ex.Message}");
                return false;
            }
        }
    }
}
