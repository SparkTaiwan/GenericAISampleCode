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
                    while (true)
                    {
                        HttpEnvelope env;
                        int idx = BlockingCollection<HttpEnvelope>.TakeFromAny(_allSendQs, out env, ct);
                        if (idx < 0) break;

                        TimingRecorder.Instance.MarkSendQueueOut(env.timestamp);

                        ChannelHandle channel = _channelsByIdx[idx];

                        string url = channel.Parameters.Url;
                        if (string.IsNullOrEmpty(url))
                        {
                            _drops.IncSendDropped();
                            TimingRecorder.Instance.Flush(env.timestamp, TimingRecorder.FrameState.DroppedUrlEmpty);
                            continue;
                        }

                        try
                        {
                            await _client.PostAsync(url, env).ConfigureAwait(false);
                            Console.WriteLine("Detected!! send analytics result to server!!");
                            FileLogger.Info($"Analytics result posted ok (ch={env.port_num})");
                            TimingRecorder.Instance.Flush(env.timestamp, TimingRecorder.FrameState.Ok);
                        }
                        catch (Exception ex)
                        {
                            FileLogger.Error("SendWorker POST failed", ex);
                            TimingRecorder.Instance.Flush(env.timestamp, TimingRecorder.FrameState.DroppedHttpException);
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
