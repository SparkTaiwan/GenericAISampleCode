#include "pch.h"
#include "channel_pipeline.h"
#include "detector_factory.h"
#include "gai_abi.h"
#include "gai_config.h"
#include "shared_detector_scheduler.h"
#include "timing_recorder.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

std::mutex g_lifecycle_mtx;
std::unique_ptr<gai::SharedDetectorScheduler> g_scheduler;

}  // namespace

extern "C" {

__declspec(dllexport) int __cdecl GAI_InitializeChannels(const int* ports, int count) {
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

        auto det = gai::CreateDetector(gai::kDetectorKind, gai::kDefaultModelPath);
        // Person path needs a usable ONNX session; Motion never returns null.
        if (!det) return 4;

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

__declspec(dllexport) int __cdecl GAI_Deinitialize(void) {
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

// Writes "CPU" / "DirectML(0)" / "" into buf (null-terminated, ANSI). Returns
// number of chars written (excluding NUL). Used by C# Program.cs to record
// the active EP in the log file at startup.
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
