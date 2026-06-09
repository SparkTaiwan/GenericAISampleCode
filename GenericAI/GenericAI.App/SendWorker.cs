using System;
using System.Collections.Concurrent;
using System.Threading;
using System.Threading.Tasks;

namespace GenericAI.App
{
    // Process-wide send worker. Drains all channels' _sendQ via TakeFromAny;
    // the returned idx names the source queue, parallel to _channelsByIdx —
    // no per-envelope dict hash. spark.recorder keys results by timestamp so
    // out-of-order delivery is harmless.
    internal sealed class SendWorker
    {
        private readonly BlockingCollection<HttpEnvelope>[] _allSendQs;
        private readonly ChannelHandle[] _channelsByIdx;
        private readonly HttpPostClient _client;
        private readonly DropCounter _drops;

        public SendWorker(BlockingCollection<HttpEnvelope>[] allSendQs,
                          ChannelHandle[] channelsByIdx,
                          HttpPostClient client,
                          DropCounter drops)
        {
            _allSendQs = allSendQs;
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
                    // Fair round-robin TakeFromAny replacement — see
                    // EncodeWorker for rationale. TakeFromAny biases toward
                    // index 0; with N channels all producing concurrently
                    // the later channels never get serviced.
                    int n = _allSendQs.Length;
                    int cursor = 0;
                    while (!ct.IsCancellationRequested)
                    {
                        HttpEnvelope env = default(HttpEnvelope);
                        int idx = -1;
                        for (int i = 0; i < n; i++)
                        {
                            int probe = (cursor + i) % n;
                            if (_allSendQs[probe].TryTake(out env))
                            {
                                idx = probe;
                                cursor = (probe + 1) % n;
                                break;
                            }
                        }

                        if (idx < 0)
                        {
                            bool allDone = true;
                            for (int i = 0; i < n; i++)
                            {
                                if (!_allSendQs[i].IsCompleted) { allDone = false; break; }
                            }
                            if (allDone) break;
                            try { await Task.Delay(1, ct).ConfigureAwait(false); }
                            catch (OperationCanceledException) { break; }
                            continue;
                        }

                        ChannelHandle channel = _channelsByIdx[idx];

                        // Retry until success or shutdown. Wrapper never drops
                        // frames on its own: empty URL, HTTP timeout, network
                        // outage all backpressure through this worker. The
                        // worker holds onto `env`, SendQ accumulates, EncodeQ
                        // backs up, callback blocks, dispatch_q fills, InferLoop
                        // stalls, infer_q fills, MmfReader stops acking — share
                        // memory writer is stalled until the downstream
                        // recovers.
                        while (!ct.IsCancellationRequested)
                        {
                            string url = channel.Parameters.Url;
                            if (string.IsNullOrEmpty(url))
                            {
                                // URL not yet configured (or cleared) — hold
                                // this envelope and wait for /SetParameters.
                                await Task.Delay(TimeSpan.FromSeconds(1), ct).ConfigureAwait(false);
                                continue;
                            }

                            try
                            {
                                await _client.PostAsync(url, env).ConfigureAwait(false);
                                Console.WriteLine("Detected!! send analytics result to server!!");
                                FileLogger.Info($"Analytics result posted ok (ch={env.port_num})");
                                break;
                            }
                            catch (Exception ex) when (!ct.IsCancellationRequested)
                            {
                                // Transient or persistent HTTP failure (5s
                                // client timeout, refused connection, 5xx,
                                // DNS, ...). Log at Warn (not Error) since
                                // retry is the expected behaviour, then back
                                // off 1s before trying the same envelope again.
                                FileLogger.Warn($"SendWorker POST failed (ch={env.port_num}), will retry: {ex.Message}");
                                await Task.Delay(TimeSpan.FromSeconds(1), ct).ConfigureAwait(false);
                            }
                        }
                    }
                }
                catch (OperationCanceledException) { /* shutdown */ }
                catch (Exception ex)
                {
                    FileLogger.Error("SendWorker fatal", ex);
                }
            }, ct);
        }
    }
}
