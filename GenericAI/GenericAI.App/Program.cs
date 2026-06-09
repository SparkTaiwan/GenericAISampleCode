using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

namespace GenericAI.App
{
    public static class Program
    {
        // Exit codes (see plan): 0 OK, 1 generic, 2 bad args, 3 port in use,
        // 4 native init failed (detector / ONNX session / MMF).
        public const int ExitOk            = 0;
        public const int ExitGeneric       = 1;
        public const int ExitBadArgs       = 2;
        public const int ExitPortInUse     = 3;
        public const int ExitNativeFailed  = 4;

        [DllImport("winmm.dll", EntryPoint = "timeBeginPeriod")]
        private static extern uint TimeBeginPeriod(uint uMilliseconds);
        [DllImport("winmm.dll", EntryPoint = "timeEndPeriod")]
        private static extern uint TimeEndPeriod(uint uMilliseconds);

        private static int s_cleanupRun;
        private static bool s_timeBeginPeriodSet;
        private static bool s_nativeInit;

        private static FrameDispatcher _dispatcher;
        private static List<ChannelHandle> _channels;
        private static Dictionary<int, ChannelHandle> _byChannelId;
        private static List<Task> _encodeTasks;
        private static List<Task> _sendTasks;
        private static CancellationTokenSource _shutdownCts;

        public static async Task<int> Main(string[] args)
        {
            if (!CommandLineArgs.TryParse(args, out CommandLineArgs parsed, out string err))
            {
                Console.Error.WriteLine($"Bad args: {err}");
                Console.Error.WriteLine(CommandLineArgs.Usage());
                return ExitBadArgs;
            }

            // .NET Framework's default per-host outbound connection cap is 2,
            // so without this every SendWorker beyond the second one would sit
            // queued inside ServicePoint instead of actually POSTing in parallel.
            System.Net.ServicePointManager.DefaultConnectionLimit =
                Math.Max(parsed.SendWorkers * parsed.ChannelCount, 16);

            TimeBeginPeriod(1);
            s_timeBeginPeriodSet = true;

            int n = parsed.ChannelCount;
            int[] ports = new int[n];
            // Consecutive sample ports (aligns with spark.recorder's getExeServerPort() + index scheme).
            for (int k = 0; k < n; k++) ports[k] = parsed.Port + k;

            FileLogger.Init(parsed.Port, parsed.LogDir);
            FileLogger.Enabled = true;
            FileLogger.Info($"GenericAI starting (basePort={parsed.Port}, channels={n}, encode={parsed.EncodeWorkers}, send={parsed.SendWorkers})");
            if (!parsed.PortFromArgs)
            {
                string msg = $"port not provided in args; using default {CommandLineArgs.DefaultPort} (Debug Run mode)";
                Console.WriteLine($"[WARN] {msg}");
                FileLogger.Warn(msg);
            }

            Console.Title = $"GenericAI ports:{string.Join(",", ports)}";

            AppDomain.CurrentDomain.ProcessExit += (s, e) => Cleanup();
            Console.CancelKeyPress += (s, e) =>
            {
                // Suppress runtime auto-terminate so Cleanup's encode/send joins finish
                // before native Deinit; main loop unwinds on its own once CTS cancels
                // all listener.RunAsync tasks.
                e.Cancel = true;
                Cleanup();
            };

            try
            {
                _channels = new List<ChannelHandle>(n);
                _byChannelId = new Dictionary<int, ChannelHandle>(n);
                foreach (int p in ports)
                {
                    ChannelHandle h = new ChannelHandle(p);
                    _channels.Add(h);
                    _byChannelId[p] = h;
                }

                foreach (ChannelHandle h in _channels)
                {
                    if (!h.StartListener())
                    {
                        FileLogger.Error($"Port {h.Port} is in use; exiting");
                        return ExitPortInUse;
                    }
                }

                // Set BEFORE the call so a SIGINT in any post-call window still
                // routes through GAI_Deinitialize. C++ side guards null scheduler
                // (exports.cpp lock_guard + null check), so an extra Deinit on
                // failed init is a no-op.
                s_nativeInit = true;
                int rc = NativeInterop.GAI_InitializeChannels(ports, n);
                if (rc != 0)
                {
                    FileLogger.Error($"GAI_InitializeChannels returned {rc}");
                    return ExitNativeFailed;
                }

                System.Text.StringBuilder backendBuf = new System.Text.StringBuilder(64);
                NativeInterop.GAI_GetBackend(backendBuf, backendBuf.Capacity);
                string backend = backendBuf.ToString();
                if (string.IsNullOrEmpty(backend)) backend = "<unknown>";
                Console.WriteLine($"[INFO] detector backend = {backend}");
                FileLogger.Info($"detector backend = {backend}");

                DropCounter    drops      = new DropCounter();
                HttpPostClient postClient = new HttpPostClient();

                _dispatcher = new FrameDispatcher(_byChannelId, drops);
                _dispatcher.Register();

                ChannelHandle[] channelsByIdx = _channels.ToArray();
                BlockingCollection<RawDetection>[] allEncodeQs = channelsByIdx.Select(c => c.EncodeQ).ToArray();
                BlockingCollection<HttpEnvelope>[] allSendQs   = channelsByIdx.Select(c => c.SendQ).ToArray();

                _shutdownCts = new CancellationTokenSource();

                _encodeTasks = new List<Task>();
                for (int i = 0; i < parsed.EncodeWorkers; i++)
                {
                    EncodeWorker w = new EncodeWorker(allEncodeQs, channelsByIdx, drops);
                    _encodeTasks.Add(w.RunAsync(_shutdownCts.Token));
                }

                _sendTasks = new List<Task>();
                for (int i = 0; i < parsed.SendWorkers; i++)
                {
                    SendWorker w = new SendWorker(allSendQs, channelsByIdx, postClient, drops);
                    _sendTasks.Add(w.RunAsync(_shutdownCts.Token));
                }

                _ = Task.Run(() => DropReporterAsync(drops, _shutdownCts.Token));

                Console.WriteLine();
                foreach (ChannelHandle h in _channels)
                {
                    Console.WriteLine($"  HTTP: http://127.0.0.1:{h.Port}/");
                    Console.WriteLine($"  MMF:  ChannelFrame_{h.Port}");
                }
                Console.WriteLine($"  Workers: encode={parsed.EncodeWorkers}, send={parsed.SendWorkers}");
                Console.WriteLine();
                Console.WriteLine("  [Ready] Waiting for /SetParameters and frames...");
                Console.WriteLine("  Press Ctrl+C to stop.");
                Console.WriteLine();
                FileLogger.Info($"Ready on ports {string.Join(",", ports)}");

                await Task.WhenAll(_channels.Select(c => c.Listener.RunAsync(_shutdownCts.Token))).ConfigureAwait(false);

                return ExitOk;
            }
            catch (Exception ex)
            {
                FileLogger.Error("Main fatal", ex);
                return ExitGeneric;
            }
            finally
            {
                Cleanup();
            }
        }

        private static async Task DropReporterAsync(DropCounter drops, CancellationToken ct)
        {
            try
            {
                long lastCb = 0, lastEnc = 0, lastSend = 0;
                while (!ct.IsCancellationRequested)
                {
                    await Task.Delay(TimeSpan.FromMinutes(1), ct).ConfigureAwait(false);
                    var (cb, enc, send) = drops.Snapshot();
                    if (cb != lastCb || enc != lastEnc || send != lastSend)
                    {
                        FileLogger.Info($"drops cumulative cb={cb} enc={enc} send={send}");
                        lastCb = cb; lastEnc = enc; lastSend = send;
                    }
                }
            }
            catch (OperationCanceledException) { /* shutdown */ }
        }

        private static void Cleanup()
        {
            if (Interlocked.Exchange(ref s_cleanupRun, 1) != 0) return;
            try
            {
                FileLogger.Info("Cleanup begin");

                try { _shutdownCts?.Cancel(); } catch { }

                if (_channels != null)
                {
                    foreach (ChannelHandle h in _channels)
                    {
                        try { h.StopListener(); } catch { }
                    }
                }

                try { _dispatcher?.Unregister(); } catch { }

                // CompleteAdding both queues up-front: callback may be blocked
                // on EncodeQ.Add and EncodeWorker may be blocked on SendQ.Add
                // (backpressure design). If we wait for encode tasks before
                // CompleteAddingSend, a worker stuck on SendQ.Add would burn
                // the 5s timeout for nothing. Doing both first lets every
                // producer wake at once and tasks drain in parallel.
                if (_channels != null)
                {
                    foreach (ChannelHandle h in _channels)
                    {
                        try { h.CompleteAddingEncode(); } catch { }
                        try { h.CompleteAddingSend(); } catch { }
                    }
                }
                try
                {
                    if (_encodeTasks != null)
                        Task.WhenAll(_encodeTasks).Wait(TimeSpan.FromSeconds(5));
                }
                catch { }
                try
                {
                    if (_sendTasks != null)
                        Task.WhenAll(_sendTasks).Wait(TimeSpan.FromSeconds(5));
                }
                catch { }

                try { TimingRecorder.Instance.Shutdown(); } catch { }

                if (s_nativeInit)
                {
                    try { NativeInterop.GAI_Deinitialize(); } catch { }
                    s_nativeInit = false;
                }

                if (s_timeBeginPeriodSet)
                {
                    try { TimeEndPeriod(1); } catch { }
                    s_timeBeginPeriodSet = false;
                }

                FileLogger.Info("Cleanup end");
            }
            catch { }
        }
    }
}
