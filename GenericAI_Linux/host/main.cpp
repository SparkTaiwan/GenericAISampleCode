// GenericAI (Linux) host process — C++ port of GenericAI.App/Program.cs.
//
// Startup order and cleanup order follow the C# host exactly; see the
// comments there for the reasoning behind each step.

#include "channel_handle.h"
#include "command_line_args.h"
#include "console_log.h"
#include "file_logger.h"
#include "gai_native_api.h"
#include "http_control_server.h"
#include "pipeline.h"
#include "settings_schema.h"
#include "state.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

using namespace gai_host;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitGeneric = 1;
constexpr int kExitBadArgs = 2;
constexpr int kExitPortInUse = 3;
constexpr int kExitNativeFailed = 4;

// GenericAI-specific verbose console lines (C# gates them on the compile-time
// TimingRecorder.Enabled, which ships false).
constexpr bool kVerboseConsole = false;

void VerboseConsole(const std::string& line) {
    if (kVerboseConsole) ConsoleLog::WriteLine(line);
}

const char* DetectorName(int kind) { return kind == 0 ? "Motion" : "Person"; }

std::string JoinPorts(const std::vector<int>& ports) {
    std::string s;
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(ports[i]);
    }
    return s;
}

// Native diagnostics sink (GAI_RegisterLogCallback). Runs on native threads;
// FileLogger only enqueues, so it is cheap and never throws.
void OnNativeLog(int level, const char* message) {
    try {
        if (!message || !*message) return;
        const std::string msg = std::string("[native] ") + message;
        switch (level) {
            case 2: FileLogger::Error(msg); break;
            case 1: FileLogger::Warn(msg); break;
            default: FileLogger::Info(msg); break;
        }
    } catch (...) {
    }
}

// Process state torn down by Cleanup().
struct App {
    std::vector<std::unique_ptr<ChannelHandle>> channels;
    std::map<int, ChannelHandle*> by_channel_id;
    std::unique_ptr<DropCounter> drops;
    std::unique_ptr<FrameDispatcher> dispatcher;
    std::vector<std::unique_ptr<EncodeWorker>> encoders;
    std::vector<std::unique_ptr<SendWorker>> senders;
#ifdef USE_ZMQ
    std::unique_ptr<ZmqResultSender> zmq_result_sender;  // null => HTTP results
#endif
    StopToken stop;
    std::thread drop_reporter;
    std::mutex reporter_mtx;
    std::condition_variable reporter_cv;
    bool native_init = false;
    bool cleaned_up = false;
};

void DropReporter(App& app) {
    long long last_cb = 0, last_enc = 0, last_send = 0;
    std::unique_lock<std::mutex> lk(app.reporter_mtx);
    while (!app.reporter_cv.wait_for(lk, std::chrono::minutes(1),
                                     [&] { return app.stop.IsCancelled(); })) {
        long long cb, enc, send;
        app.drops->Snapshot(cb, enc, send);
        if (cb != last_cb || enc != last_enc || send != last_send) {
            FileLogger::Info("drops cumulative cb=" + std::to_string(cb) + " enc=" + std::to_string(enc) +
                             " send=" + std::to_string(send));
            last_cb = cb;
            last_enc = enc;
            last_send = send;
        }
    }
}

void Cleanup(App& app) {
    if (app.cleaned_up) return;
    app.cleaned_up = true;
    try {
        FileLogger::Info("Cleanup begin");

        app.stop.Cancel();
        app.reporter_cv.notify_all();

        for (auto& h : app.channels) {
            try { h->StopListener(); } catch (...) {}
        }

        if (app.dispatcher) app.dispatcher->Unregister();

        // CompleteAdding both queues up front: the callback may be blocked on
        // EncodeQ.Add and an EncodeWorker on SendQ.Add (backpressure design);
        // completing both lets every producer wake at once.
        for (auto& h : app.channels) {
            h->EncodeQ().CompleteAdding();
            h->SendQ().CompleteAdding();
        }
        for (auto& w : app.encoders) w->Join();
        for (auto& w : app.senders) w->Join();
        if (app.drop_reporter.joinable()) app.drop_reporter.join();

#ifdef USE_ZMQ
        // Send workers joined -> no more result sends (LINGER=0, never blocks).
        if (app.zmq_result_sender) app.zmq_result_sender->Close();
#endif

        // Return I420 buffers an EncodeWorker left behind when it stopped.
        for (auto& h : app.channels) {
            RawDetection r;
            while (h->EncodeQ().TryTake(r)) FramePool::Return(std::move(r.frame_i420));
        }

        if (app.native_init) {
            GAI_Deinitialize();
            app.native_init = false;
        }
        // After Deinitialize native threads are joined: no more log callbacks.
        GAI_RegisterLogCallback(nullptr);

        FileLogger::Info("Cleanup end");
        FileLogger::Shutdown();
    } catch (...) {
    }
}

// Blocks SIGINT/SIGTERM in every thread (they inherit the mask) so that only
// the main thread's sigwait() sees them and Cleanup runs on a normal stack.
sigset_t BlockShutdownSignals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    // A peer closing an HTTP keep-alive socket mid-write must not kill us.
    std::signal(SIGPIPE, SIG_IGN);
    return set;
}

int Run(App& app, const CommandLineArgs& parsed, const sigset_t& shutdown_signals) {
    const int n = parsed.channel_count;
    std::vector<int> ports(static_cast<std::size_t>(n));
    // Consecutive ports (matches spark.recorder's getExeServerPort() + index).
    for (int k = 0; k < n; ++k) ports[static_cast<std::size_t>(k)] = parsed.port + k;

    FileLogger::Init(parsed.port);
    std::string detector_label = parsed.detector_kind < 0 ? "<compile-time default>"
                                                          : DetectorName(parsed.detector_kind);
    // Provisional schema; re-configured from GAI_GetDetectorKind after init.
    if (parsed.detector_kind >= 0) SettingsSchema::Configure(parsed.detector_kind);
    FileLogger::Info("GenericAI starting (basePort=" + std::to_string(parsed.port) +
                     ", channels=" + std::to_string(n) + ", mode=" + parsed.mode +
                     ", detector=" + detector_label + ", encode=" + std::to_string(parsed.encode_workers) +
                     ", send=" + std::to_string(parsed.send_workers) + ")");
    if (parsed.port_from_args) {
        ConsoleLog::WriteLine("Port number: " + std::to_string(parsed.port));
    } else {
        ConsoleLog::WriteLine("Invalid Input. Use default " + std::to_string(CommandLineArgs::kDefaultPort));
        FileLogger::Warn("port not provided in args; using default " +
                         std::to_string(CommandLineArgs::kDefaultPort) + " (Debug Run mode)");
    }

    for (int p : ports) {
        app.channels.emplace_back(new ChannelHandle(p, parsed.http_host));
        app.by_channel_id[p] = app.channels.back().get();
        ConsoleLog::WriteLine("httpServerUrl: http://" + parsed.http_host + ":" + std::to_string(p) + "/");
    }
    for (auto& h : app.channels) {
        if (!h->StartListener()) {
            ConsoleLog::ErrorLine("Exiting: HTTP control listener for port " + std::to_string(h->Port()) +
                                  " did not start (see error above).");
            FileLogger::Error("Port " + std::to_string(h->Port()) + " is in use; exiting");
            return kExitPortInUse;
        }
    }

    // Log sink BEFORE native init so the first pool-sizing diagnostics land too.
    GAI_RegisterLogCallback(&OnNativeLog);
    GAI_SetVerbose(ConsoleLog::NativeDebug ? 1 : 0);

    app.native_init = true;
    ConsoleLog::WriteLine("Initializing native channels (detector=" + detector_label + ") ...");
    const int rc = GAI_InitializeChannels(ports.data(), n, parsed.detector_kind);
    ConsoleLog::WriteLine("GAI_InitializeChannels rc=" + std::to_string(rc));

    // rc == 5: degraded (e.g. model missing). Keep the HTTP listeners up so
    // the recorder learns the reason from /Alive instead of restart-looping.
    const bool degraded = (rc == 5);
    if (rc != 0 && !degraded) {
        char err[1024] = {0};
        GAI_GetInitError(err, sizeof(err));
        ConsoleLog::ErrorLine("Native init FAILED: GAI_InitializeChannels returned " + std::to_string(rc) +
                              (err[0] ? std::string(" -- ") + err : std::string()));
        FileLogger::Error("GAI_InitializeChannels returned " + std::to_string(rc) + " " + err);
        return kExitNativeFailed;
    }

    if (degraded) {
        char err[1024] = {0};
        GAI_GetInitError(err, sizeof(err));
        std::string em = err[0] ? err : "detector initialization failed";
        HealthState::SetError(em);
        ConsoleLog::ErrorLine("DEGRADED: detector init failed; /Alive will report error: " + em);
        FileLogger::Error("DEGRADED: detector init failed; serving /Alive error, no detection: " + em);
    } else {
        // Authoritative detector kind (resolves the detector_kind<0 fallback
        // to gai::kDetectorKind natively).
        const int active_kind = GAI_GetDetectorKind();
        if (active_kind >= 0) {
            SettingsSchema::Configure(active_kind);
            detector_label = DetectorName(active_kind);
            FileLogger::Info("active detector = " + detector_label + " (kind=" + std::to_string(active_kind) + ")");
        }

        char backend_buf[64] = {0};
        GAI_GetBackend(backend_buf, sizeof(backend_buf));
        const std::string backend = backend_buf[0] ? backend_buf : "<unknown>";
        VerboseConsole("[INFO] detector backend = " + backend);
        FileLogger::Info("detector backend = " + backend);

        // Seed every channel with the built-in schema defaults so a channel
        // that never receives /SetParameters still follows the default schema.
        try {
            const nlohmann::json schema_root = nlohmann::json::parse(SettingsSchema::Json());
            for (int p : ports) HttpControlServer::ApplyAiSettingsToNative(p, schema_root);
            FileLogger::Info("Applied built-in default ai_settings to all channels");
        } catch (const std::exception& ex) {
            FileLogger::Warn(std::string("Applying default ai_settings failed: ") + ex.what());
        }

        app.drops.reset(new DropCounter());
        app.dispatcher.reset(new FrameDispatcher(app.by_channel_id, *app.drops));
        app.dispatcher->Register();
        ConsoleLog::WriteLine("register Callback");

        std::vector<ChannelHandle*> channels_by_idx;
        for (auto& h : app.channels) channels_by_idx.push_back(h.get());

        for (int i = 0; i < parsed.encode_workers; ++i) {
            app.encoders.emplace_back(new EncodeWorker(channels_by_idx, *app.drops, app.stop));
            app.encoders.back()->Start();
        }

#ifdef USE_ZMQ
        // Result plane: ZMQ PUSH when result_endpoint is given, else HTTP POST.
        if (parsed.UseZmqResults()) {
            try {
                app.zmq_result_sender.reset(new ZmqResultSender(parsed.result_endpoint, n));
                ConsoleLog::WriteLine("ZMQ result sink: connect PUSH -> " + parsed.result_endpoint);
                FileLogger::Info("ZMQ result sender connected, endpoint=" + parsed.result_endpoint);
            } catch (const std::exception& ex) {
                FileLogger::Error("ZMQ result sender init failed (" + parsed.result_endpoint + "): " + ex.what());
                ConsoleLog::ErrorLine(std::string("ZMQ result sender init failed: ") + ex.what());
                return kExitNativeFailed;
            }
        }
#else
        if (parsed.UseZmqResults() || parsed.UseZmqFrames()) {
            ConsoleLog::ErrorLine("This build has no ZMQ support (GAI_ENABLE_ZMQ=OFF); drop the ZMQ endpoints.");
            return kExitBadArgs;
        }
#endif

        for (int i = 0; i < parsed.send_workers; ++i) {
            app.senders.emplace_back(new SendWorker(channels_by_idx, *app.drops, app.stop
#ifdef USE_ZMQ
                                                    , app.zmq_result_sender.get()
#endif
                                                    ));
            app.senders.back()->Start();
        }

        app.drop_reporter = std::thread(DropReporter, std::ref(app));

#ifdef USE_ZMQ
        // Frame source: ZMQ (NAL) when frame_endpoint is given, else MMF.
        // Start before the first /SetParameters so channels never start an MmfReader.
        if (parsed.UseZmqFrames()) {
            const int zrc = GAI_StartZmqReceiver(parsed.frame_endpoint.c_str());
            if (zrc != 0) {
                FileLogger::Error("GAI_StartZmqReceiver(" + parsed.frame_endpoint + ") returned " +
                                  std::to_string(zrc));
                ConsoleLog::ErrorLine("Failed to start ZMQ frame receiver at " + parsed.frame_endpoint);
                return kExitNativeFailed;
            }
            ConsoleLog::WriteLine("ZMQ frame source: connect PULL -> " + parsed.frame_endpoint);
            FileLogger::Info("ZMQ frame receiver started, endpoint=" + parsed.frame_endpoint);
        }
#endif
    }

    // From here /SetParameters reaches native: after the default seed (so it
    // can't overwrite the recorder's values) and after the ZMQ receiver started
    // (so the first SetParameters can't start an MmfReader in ZMQ mode).
    NativeReady::Set();

    VerboseConsole("");
    for (int p : ports) VerboseConsole("  MMF:  /dev/shm/ChannelFrame_" + std::to_string(p));
    VerboseConsole("  Workers: encode=" + std::to_string(parsed.encode_workers) +
                   ", send=" + std::to_string(parsed.send_workers));
    VerboseConsole("");
    VerboseConsole("  [Ready] Waiting for /SetParameters and frames...");
    VerboseConsole("  Press Ctrl+C to stop.");
    VerboseConsole("");
    FileLogger::Info("Ready on ports " + JoinPorts(ports));

    // Serve until SIGINT / SIGTERM / SIGHUP.
    int sig = 0;
    sigwait(&shutdown_signals, &sig);
    FileLogger::Info("Signal " + std::to_string(sig) + " received; shutting down");
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    const sigset_t shutdown_signals = BlockShutdownSignals();

    // Load the debug switches first so they gate every WriteLine below.
    ConsoleLog::LoadFromConfig();
    ConsoleLog::WriteLine(CommandLineArgs::Usage());

    std::vector<std::string> args(argv + 1, argv + argc);
    CommandLineArgs parsed;
    std::string err;
    if (!CommandLineArgs::TryParse(args, parsed, err)) {
        ConsoleLog::ErrorLine("Bad args: " + err);
        ConsoleLog::ErrorLine(CommandLineArgs::Usage());
        return kExitBadArgs;
    }

    App app;
    int code = kExitGeneric;
    try {
        code = Run(app, parsed, shutdown_signals);
    } catch (const std::exception& ex) {
        ConsoleLog::ErrorLine(std::string("FATAL: ") + ex.what());
        FileLogger::Error(std::string("Main fatal: ") + ex.what());
        code = kExitGeneric;
    }
    Cleanup(app);
    return code;
}
