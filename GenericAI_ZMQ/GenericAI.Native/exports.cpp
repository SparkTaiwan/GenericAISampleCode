#include "pch.h"
#include "channel_pipeline.h"
#include "detector_factory.h"
#include "gai_abi.h"
#include "gai_config.h"
#include "host_log.h"
#include "shared_detector_scheduler.h"
#include "timing_recorder.h"
#include "zmq_frame_receiver.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

std::mutex g_lifecycle_mtx;
std::unique_ptr<gai::SharedDetectorScheduler> g_scheduler;

// ZMQ frame plane (optional). When started, frames arrive over ZMQ (NAL -> decode
// -> ChannelPipeline::SubmitDecodedFrame) instead of MMF.
std::unique_ptr<gai::ZmqFrameReceiver> g_zmq_receiver;

// Last initialization error (e.g. detector/model load failure). Exposed to the
// C# host via GAI_GetInitError so it can report it on /Alive instead of the
// process crashing.
std::mutex g_init_error_mtx;
std::string g_init_error;

// Directory of the running exe (ANSI/CP_ACP, to round-trip through detector_person's
// Widen()). Empty on failure.
std::string ExeDir() {
    wchar_t wbuf[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, wbuf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    std::wstring w(wbuf, n);
    size_t slash = w.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::string();
    std::wstring wdir = w.substr(0, slash);
    int len = WideCharToMultiByte(CP_ACP, 0, wdir.c_str(), (int)wdir.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string dir(len, '\0');
    WideCharToMultiByte(CP_ACP, 0, wdir.c_str(), (int)wdir.size(), &dir[0], len, nullptr, nullptr);
    return dir;
}

// Resolve a (possibly relative) resource path against the exe's own directory so
// it works regardless of the process working directory. The recorder spawns this
// exe with an arbitrary CWD; a bare "models/yolo.onnx" would otherwise be looked
// up under the recorder's CWD and fail.
std::string ResolveAgainstExe(const std::string& path) {
    // Already absolute? ("X:\..." or "\\server\..." or "/...")
    if (path.size() >= 2 && (path[1] == ':' || path[0] == '\\' || path[0] == '/'))
        return path;
    std::string dir = ExeDir();
    if (dir.empty()) return path;  // fall back to CWD-relative behavior
    return dir + "\\" + path;
}

}  // namespace

extern "C" {

// detector_kind selects the active detector at runtime:
//   0 = Motion, 1 = Person (object detection); any other value (e.g. -1)
//   falls back to the compile-time gai::kDetectorKind in gai_config.h.
// This lets the recorder spawn one built-in GenericAI.exe and pick the
// backend via `detector=` on the command line instead of a rebuild.
__declspec(dllexport) int __cdecl GAI_InitializeChannels(const int* ports, int count, int detector_kind) {
    std::lock_guard<std::mutex> lk(g_lifecycle_mtx);
    if (g_scheduler) return 0;
    if (!ports || count <= 0) return 1;

    try {
        // spec §3 says C# generates X+2k so ports never repeat; FindByPort is
        // a linear scan that silently returns the first hit, so a duplicate
        // would route every /SetParameters to one channel and starve the rest.
        std::set<int> seen;
        for (int i = 0; i < count; ++i) {
            if (!seen.insert(ports[i]).second) return 1;
        }

        gai::TimingRecorder::Instance().Init(ports[0]);

        gai::DetectorKind kind = gai::kDetectorKind;
        if (detector_kind == static_cast<int>(gai::DetectorKind::Motion))
            kind = gai::DetectorKind::Motion;
        else if (detector_kind == static_cast<int>(gai::DetectorKind::Person))
            kind = gai::DetectorKind::Person;
        // else: keep compile-time default (gai::kDetectorKind).

        // Resolve the model path against the exe's own folder so it loads no matter
        // what the recorder set as the working directory.
        const std::string modelPath = ResolveAgainstExe(gai::kDefaultModelPath);

        std::string detErr;
        auto det = gai::CreateDetector(kind, modelPath, detErr);
        if (!det) {
            // Degraded mode: the detector could not load (e.g. missing model). Do
            // NOT crash — record the reason and return 5 so the host stays alive
            // and reports it on /Alive. SetParameters will no-op (g_scheduler null).
            std::lock_guard<std::mutex> elk(g_init_error_mtx);
            g_init_error = detErr.empty() ? std::string("detector initialization failed") : detErr;
            return 5;
        }

        std::vector<std::unique_ptr<gai::ChannelPipeline>> chans;
        chans.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            auto p = std::unique_ptr<gai::ChannelPipeline>(
                         new gai::ChannelPipeline(ports[i]));
            p->Start();
            chans.push_back(std::move(p));
        }

        auto s = std::unique_ptr<gai::SharedDetectorScheduler>(
                     new gai::SharedDetectorScheduler());
        if (!s->Start(std::move(det), std::move(chans))) return 4;
        g_scheduler = std::move(s);
        return 0;
    } catch (...) {
        // C ABI must not let C++ exceptions escape. Half-built channels /
        // scheduler are cleaned up by unique_ptr destruction: ~ChannelPipeline
        // runs Stop()+Join(), ~SharedDetectorScheduler runs Stop().
        return 4;
    }
}

__declspec(dllexport) int __cdecl GAI_SetChannelParameters(int port, const GAI_Settings* parameters) {
    if (!parameters) return 1;
    std::lock_guard<std::mutex> lk(g_lifecycle_mtx);
    if (!g_scheduler) return 1;
    auto* c = g_scheduler->FindByPort(port);
    if (!c) return 1;
    try { c->ApplyParameters(*parameters); }
    catch (...) { return 1; }
    return 0;
}

__declspec(dllexport) void __cdecl GAI_RegisterCallback(GAI_DetectionCallback cb) {
    std::lock_guard<std::mutex> lk(g_lifecycle_mtx);
    if (!g_scheduler) return;
    try {
        for (std::size_t i = 0; i < g_scheduler->ChannelCount(); ++i) {
            if (auto* c = g_scheduler->ChannelAt(i)) c->SetCallback(cb);
        }
    } catch (...) {}
}

// Starts the ZMQ frame plane. Call AFTER GAI_InitializeChannels and BEFORE the
// first /SetParameters (it flips channels into ZMQ mode so they do not spawn an
// MmfReader). endpoint is the module's bound PUSH address, e.g. "tcp://host:5556";
// this side connects a PULL socket to it. Returns 0 on success.
__declspec(dllexport) int __cdecl GAI_StartZmqReceiver(const char* endpoint) {
    if (!endpoint || !endpoint[0]) return 1;
    std::lock_guard<std::mutex> lk(g_lifecycle_mtx);
    if (!g_scheduler) return 1;          // must init channels first
    if (g_zmq_receiver) return 0;        // already running
    try {
        gai::SetZmqFrameMode(true);
        auto r = std::unique_ptr<gai::ZmqFrameReceiver>(
                     new gai::ZmqFrameReceiver(endpoint, g_scheduler.get()));
        if (!r->Start()) return 1;
        g_zmq_receiver = std::move(r);
        return 0;
    } catch (...) {
        return 1;
    }
}

__declspec(dllexport) void __cdecl GAI_StopZmqReceiver(void) {
    std::unique_ptr<gai::ZmqFrameReceiver> r;
    {
        std::lock_guard<std::mutex> lk(g_lifecycle_mtx);
        r = std::move(g_zmq_receiver);
    }
    if (r) { try { r->Stop(); } catch (...) {} }
}

__declspec(dllexport) int __cdecl GAI_Deinitialize(void) {
    // ORDER: stop the frame receiver FIRST so no SubmitDecodedFrame races the
    // channel teardown below (same ordering contract as the shared InferLoop).
    std::unique_ptr<gai::ZmqFrameReceiver> r;
    {
        std::lock_guard<std::mutex> lk(g_lifecycle_mtx);
        r = std::move(g_zmq_receiver);
    }
    if (r) { try { r->Stop(); } catch (...) {} }

    std::unique_ptr<gai::SharedDetectorScheduler> s;
    {
        std::lock_guard<std::mutex> lk(g_lifecycle_mtx);
        s = std::move(g_scheduler);
    }
    if (s) {
        try { s->Stop(); } catch (...) {}
    }
    try { gai::TimingRecorder::Instance().Shutdown(); } catch (...) {}
    return 0;
}

// Registers the host log sink. Independent of the scheduler lifecycle so the
// C# side can register before GAI_InitializeChannels and catch the first
// channel's pool-sizing / frame-drop diagnostics. Pass nullptr to unregister
// (the host must do this before freeing its delegate).
__declspec(dllexport) void __cdecl GAI_RegisterLogCallback(GAI_LogCallback cb) {
    gai::SetHostLogCallback(cb);
}

// Writes "CPU" / "DirectML(0)" / "" into buf (null-terminated, ANSI). Returns
// number of chars written (excluding NUL). Used by C# Program.cs to record
// the active EP in the log file at startup.
// Writes the last init error (e.g. model-load failure) into buf (null-terminated,
// ANSI). Returns chars written (excluding NUL); 0 means no error (healthy). The
// C# host calls this after a degraded (rc=5) init and serves it on /Alive.
__declspec(dllexport) int __cdecl GAI_GetInitError(char* buf, int buf_len) {
    if (!buf || buf_len <= 0) return 0;
    try {
        std::string msg;
        {
            std::lock_guard<std::mutex> lk(g_init_error_mtx);
            msg = g_init_error;
        }
        std::size_t n = msg.size();
        if (n >= static_cast<std::size_t>(buf_len)) n = static_cast<std::size_t>(buf_len) - 1;
        std::memcpy(buf, msg.data(), n);
        buf[n] = '\0';
        return static_cast<int>(n);
    } catch (...) {
        buf[0] = '\0';
        return 0;
    }
}

__declspec(dllexport) int __cdecl GAI_GetBackend(char* buf, int buf_len) {
    if (!buf || buf_len <= 0) return 0;
    try {
        std::string label;
        {
            std::lock_guard<std::mutex> lk(g_lifecycle_mtx);
            if (g_scheduler) label = g_scheduler->Backend();
        }
        std::size_t n = label.size();
        if (n >= static_cast<std::size_t>(buf_len)) n = static_cast<std::size_t>(buf_len) - 1;
        std::memcpy(buf, label.data(), n);
        buf[n] = '\0';
        return static_cast<int>(n);
    } catch (...) {
        buf[0] = '\0';
        return 0;
    }
}

}
