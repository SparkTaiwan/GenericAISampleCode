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
        // Pinned like FrameDispatcher's detection delegate: native threads
        // hold the function pointer until GAI_RegisterLogCallback(null).
        private static NativeInterop.LogCallbackFunction _nativeLogDelegate;
        private static GCHandle _nativeLogHandle;
        private static List<ChannelHandle> _channels;
        private static Dictionary<int, ChannelHandle> _byChannelId;
        private static List<Task> _encodeTasks;
        private static List<Task> _sendTasks;
#if USE_ZMQ
        private static ZmqResultSender _zmqResultSender;  // null => HTTP results
#endif
        private static CancellationTokenSource _shutdownCts;

        public static async Task<int> Main(string[] args)
        {
            // Load the debug switch first so it gates every ConsoleLog.WriteLine below.
            // Drop a "GenericAI.Config" next to the exe with "show_debug = 1" to see the
            // verbose output; no rebuild needed.
            ConsoleLog.LoadFromConfig();

            ConsoleLog.WriteLine(CommandLineArgs.Usage());

            if (!CommandLineArgs.TryParse(args, out CommandLineArgs parsed, out string err))
            {
                ConsoleLog.ErrorLine($"Bad args: {err}");
                ConsoleLog.ErrorLine(CommandLineArgs.Usage());
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

            FileLogger.Init(parsed.Port);
            TimingRecorder.Instance.Init(parsed.Port);
            string detectorLabel = parsed.DetectorKind < 0
                ? "<compile-time default>"
                : ((DetectorType)parsed.DetectorKind).ToString();
            // Provisional schema selection. When detector= is passed explicitly the
            // kind is already known, so /GetSettingsSchema is correct immediately.
            // When it is unset (-1) the compile-time default lives natively
            // (gai_config.h); we re-Configure with the authoritative kind from
            // GAI_GetDetectorKind right after a successful init below, so the schema
            // always matches the detector that actually loaded.
            if (parsed.DetectorKind >= 0) SettingsSchema.Configure(parsed.DetectorKind);
            FileLogger.Info($"GenericAI starting (basePort={parsed.Port}, channels={n}, mode={parsed.Mode}, detector={detectorLabel}, encode={parsed.EncodeWorkers}, send={parsed.SendWorkers})");
            if (parsed.PortFromArgs)
            {
                ConsoleLog.WriteLine($"Port number: {parsed.Port}");
            }
            else
            {
                ConsoleLog.WriteLine($"Invalid Input. Use default {CommandLineArgs.DefaultPort}");
                FileLogger.Warn($"port not provided in args; using default {CommandLineArgs.DefaultPort} (Debug Run mode)");
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
                    ChannelHandle h = new ChannelHandle(p, parsed.HttpHost);
                    _channels.Add(h);
                    _byChannelId[p] = h;
                    ConsoleLog.WriteLine($"httpServerUrl: http://{parsed.HttpHost}:{p}/");
                }

                foreach (ChannelHandle h in _channels)
                {
                    if (!h.StartListener())
                    {
                        ConsoleLog.ErrorLine($"Exiting: HTTP control listener for port {h.Port} did not start (see error above).");
                        FileLogger.Error($"Port {h.Port} is in use; exiting");
                        return ExitPortInUse;
                    }
                }

                // Register the log sink BEFORE native init so the first
                // channel's pool-sizing / frame-drop diagnostics land in the
                // file log too.
                _nativeLogDelegate = OnNativeLog;
                _nativeLogHandle = GCHandle.Alloc(_nativeLogDelegate);
                // First native P/Invoke: if GenericAI.Native.dll (or any implicit
                // dep -- onnxruntime / DirectML / ffmpeg / turbojpeg / libzmq) is
                // missing, this throws DllNotFoundException, caught below.
                ConsoleLog.WriteLine("Loading native library (GenericAI.Native.dll + deps) ...");
                NativeInterop.GAI_RegisterLogCallback(_nativeLogDelegate);

                // Turn native informational logging on when GenericAI.Config has
                // show_native_debug = 1. Independent of show_debug (C# console); gates
                // the native [AI]/[PersonDetector]/[MotionDetector]/[channel]/[zmq]
                // lines. std::cerr errors on the native side always print regardless.
                NativeInterop.GAI_SetVerbose(ConsoleLog.NativeDebug ? 1 : 0);

                // Set BEFORE the call so a SIGINT in any post-call window still
                // routes through GAI_Deinitialize. C++ side guards null scheduler
                // (exports.cpp lock_guard + null check), so an extra Deinit on
                // failed init is a no-op.
                s_nativeInit = true;
                ConsoleLog.WriteLine($"Initializing native channels (detector={detectorLabel}) ...");
                int rc = NativeInterop.GAI_InitializeChannels(ports, n, parsed.DetectorKind);
                ConsoleLog.WriteLine($"GAI_InitializeChannels rc={rc}");

                // rc == 5: degraded — the detector could not initialise (e.g. the
                // ONNX model is missing). Do NOT exit: keep the HTTP listeners up so
                // the recorder learns the reason from /Alive, instead of the process
                // crashing and the recorder pointlessly restarting it.
                bool degraded = (rc == 5);
                if (rc != 0 && !degraded)
                {
                    System.Text.StringBuilder initErr = new System.Text.StringBuilder(1024);
                    try { NativeInterop.GAI_GetInitError(initErr, initErr.Capacity); } catch { }
                    string emsg = initErr.ToString();
                    ConsoleLog.ErrorLine($"Native init FAILED: GAI_InitializeChannels returned {rc}"
                        + (string.IsNullOrEmpty(emsg) ? "" : $" -- {emsg}"));
                    FileLogger.Error($"GAI_InitializeChannels returned {rc} {emsg}");
                    return ExitNativeFailed;
                }

                _shutdownCts = new CancellationTokenSource();

                if (degraded)
                {
                    System.Text.StringBuilder errBuf = new System.Text.StringBuilder(1024);
                    NativeInterop.GAI_GetInitError(errBuf, errBuf.Capacity);
                    string em = errBuf.ToString();
                    if (string.IsNullOrEmpty(em)) em = "detector initialization failed";
                    HealthState.SetError(em);
                    ConsoleLog.ErrorLine($"DEGRADED: detector init failed; /Alive will report error: {em}");
                    FileLogger.Error($"DEGRADED: detector init failed; serving /Alive error, no detection: {em}");
                }
                else
                {
                    // Authoritative detector kind: whatever native actually loaded,
                    // resolving the detector_kind<0 fallback to gai::kDetectorKind
                    // (gai_config.h) — the single source of truth. This makes
                    // /GetSettingsSchema and the channel-seed below serve the matching
                    // schema (motion vs object detection) even when detector= was unset.
                    int activeKind = NativeInterop.GAI_GetDetectorKind();
                    if (activeKind >= 0)
                    {
                        SettingsSchema.Configure(activeKind);
                        detectorLabel = ((DetectorType)activeKind).ToString();
                        FileLogger.Info($"active detector = {detectorLabel} (kind={activeKind})");
                    }

                    System.Text.StringBuilder backendBuf = new System.Text.StringBuilder(64);
                    NativeInterop.GAI_GetBackend(backendBuf, backendBuf.Capacity);
                    string backend = backendBuf.ToString();
                    if (string.IsNullOrEmpty(backend)) backend = "<unknown>";
                    VerboseConsole($"[INFO] detector backend = {backend}");
                    FileLogger.Info($"detector backend = {backend}");

                    // Seed every channel with the built-in schema defaults so a channel
                    // that never receives /SetParameters still follows the default schema
                    // (e.g. object detection: only person/car at confidence 0.70) instead
                    // of the native all-classes default. A later /SetParameters overrides.
                    try
                    {
                        var schemaRoot = Newtonsoft.Json.Linq.JObject.Parse(SettingsSchema.Json);
                        foreach (int p in ports)
                            HttpListenerHost.ApplyAiSettingsToNative(p, schemaRoot);
                        FileLogger.Info("Applied built-in default ai_settings to all channels");
                    }
                    catch (Exception ex)
                    {
                        FileLogger.Warn($"Applying default ai_settings failed: {ex.Message}");
                    }

                    DropCounter    drops      = new DropCounter();
                    HttpPostClient postClient = new HttpPostClient();

                    _dispatcher = new FrameDispatcher(_byChannelId, drops);
                    _dispatcher.Register();
                    ConsoleLog.WriteLine("register Callback");

                    ChannelHandle[] channelsByIdx = _channels.ToArray();
                    BlockingCollection<RawDetection>[] allEncodeQs = channelsByIdx.Select(c => c.EncodeQ).ToArray();

                    _encodeTasks = new List<Task>();
                    for (int i = 0; i < parsed.EncodeWorkers; i++)
                    {
                        EncodeWorker w = new EncodeWorker(allEncodeQs, channelsByIdx, drops);
                        _encodeTasks.Add(w.RunAsync(_shutdownCts.Token));
                    }

#if USE_ZMQ
                    // Result plane: ZMQ PUSH (connect to the module's bound PULL) when
                    // result_endpoint is given, else HTTP POST to analytics_event_api_url.
                    if (parsed.UseZmqResults)
                    {
                        try
                        {
                            _zmqResultSender = new ZmqResultSender(parsed.ResultEndpoint, parsed.ChannelCount);
                            ConsoleLog.WriteLine($"ZMQ result sink: connect PUSH -> {parsed.ResultEndpoint}");
                            FileLogger.Info($"ZMQ result sender connected, endpoint={parsed.ResultEndpoint}");
                        }
                        catch (Exception ex)
                        {
                            FileLogger.Error($"ZMQ result sender init failed ({parsed.ResultEndpoint})", ex);
                            ConsoleLog.ErrorLine($"ZMQ result sender init failed: {ex.Message}");
                            return ExitNativeFailed;
                        }
                    }
#endif

                    _sendTasks = new List<Task>();
                    for (int i = 0; i < parsed.SendWorkers; i++)
                    {
#if USE_ZMQ
                        SendWorker w = new SendWorker(channelsByIdx, postClient, drops, _zmqResultSender);
#else
                        SendWorker w = new SendWorker(channelsByIdx, postClient, drops);
#endif
                        _sendTasks.Add(w.RunAsync(_shutdownCts.Token));
                    }

                    _ = Task.Run(() => DropReporterAsync(drops, _shutdownCts.Token));

#if USE_ZMQ
                    // Frame source: ZMQ (NAL) when frame_endpoint is given, else MMF.
                    // Start BEFORE the listeners serve /SetParameters so channels are
                    // already in ZMQ mode (they skip the MmfReader). GAI_Deinitialize
                    // stops the receiver first on shutdown.
                    if (parsed.UseZmqFrames)
                    {
                        int zrc = NativeInterop.GAI_StartZmqReceiver(parsed.FrameEndpoint);
                        if (zrc != 0)
                        {
                            FileLogger.Error($"GAI_StartZmqReceiver({parsed.FrameEndpoint}) returned {zrc}");
                            ConsoleLog.ErrorLine($"Failed to start ZMQ frame receiver at {parsed.FrameEndpoint}");
                            return ExitNativeFailed;
                        }
                        ConsoleLog.WriteLine($"ZMQ frame source: connect PULL -> {parsed.FrameEndpoint}");
                        FileLogger.Info($"ZMQ frame receiver started, endpoint={parsed.FrameEndpoint}");
                    }
#endif
                }

                VerboseConsole("");
                foreach (ChannelHandle h in _channels)
                {
                    VerboseConsole($"  MMF:  ChannelFrame_{h.Port}");
                }
                VerboseConsole($"  Workers: encode={parsed.EncodeWorkers}, send={parsed.SendWorkers}");
                VerboseConsole("");
                VerboseConsole("  [Ready] Waiting for /SetParameters and frames...");
                VerboseConsole("  Press Ctrl+C to stop.");
                VerboseConsole("");
                FileLogger.Info($"Ready on ports {string.Join(",", ports)}");

                await Task.WhenAll(_channels.Select(c => c.Listener.RunAsync(_shutdownCts.Token))).ConfigureAwait(false);

                return ExitOk;
            }
            catch (Exception ex)
            {
                // Surface to the console too: on a fresh machine this is almost
                // always a missing native dependency (DllNotFoundException for
                // GenericAI.Native.dll / onnxruntime / DirectML / ffmpeg / libzmq),
                // and the file log under C:\ProgramData\Spark\GenericAI\Logs is
                // easy to miss.
                ConsoleLog.ErrorLine($"FATAL: {ex.GetType().Name}: {ex.Message}");
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

#if USE_ZMQ
                // Send workers have joined -> no more ZMQ result sends; close the
                // result socket (LINGER=0, so this never blocks).
                try { _zmqResultSender?.Dispose(); } catch { }
#endif

                // After workers have joined, drain any I420 buffers that the
                // EncodeWorker left in EncodeQ when its CTS got cancelled.
                // Without this, the pool would degrade across a hot restart —
                // GC still reclaims the byte[], but the pool's internal slot
                // count does not.
                if (_channels != null)
                {
                    foreach (ChannelHandle h in _channels)
                    {
                        RawDetection r;
                        while (h.EncodeQ.TryTake(out r))
                        {
                            try { FrameDispatcher.FramePool.Return(r.FrameI420, clearArray: false); } catch { }
                        }
                    }
                }

                try { TimingRecorder.Instance.Shutdown(); } catch { }

                if (s_nativeInit)
                {
                    try { NativeInterop.GAI_Deinitialize(); } catch { }
                    s_nativeInit = false;
                }

                // After Deinitialize: native threads are joined, so no more
                // log callbacks can arrive once the pointer is cleared.
                try { NativeInterop.GAI_RegisterLogCallback(null); } catch { }
                try { if (_nativeLogHandle.IsAllocated) _nativeLogHandle.Free(); } catch { }

                if (s_timeBeginPeriodSet)
                {
                    try { TimeEndPeriod(1); } catch { }
                    s_timeBeginPeriodSet = false;
                }

                FileLogger.Info("Cleanup end");
                FileLogger.Shutdown();
            }
            catch { }
        }

        // Native diagnostics sink (GAI_RegisterLogCallback target). Runs on
        // native threads (MmfReader / SetParameters apply); must never throw
        // across the P/Invoke boundary. FileLogger only enqueues, so this is
        // cheap enough for the native callers.
        private static void OnNativeLog(int level, IntPtr message)
        {
            try
            {
                string msg = Marshal.PtrToStringAnsi(message);
                if (string.IsNullOrEmpty(msg)) return;
                switch (level)
                {
                    case 2: FileLogger.Error("[native] " + msg); break;
                    case 1: FileLogger.Warn("[native] " + msg); break;
                    default: FileLogger.Info("[native] " + msg); break;
                }
            }
            catch
            {
                // Logging must never crash the native caller.
            }
        }

        // GenericAI-specific console lines beyond the CSharp sample's set.
        // Gated by the timing/verbose switch so the default console output
        // matches the sample exactly.
        private static void VerboseConsole(string line)
        {
#pragma warning disable 162  // CS0162: TimingRecorder.Enabled is a compile-time const
            if (TimingRecorder.Enabled) Console.WriteLine(line);
#pragma warning restore 162
        }
    }
}
