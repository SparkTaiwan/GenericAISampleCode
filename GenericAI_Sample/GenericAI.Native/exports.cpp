#include "pch.h"
#include "channel.h"
#include "gai_abi.h"

#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace {

std::mutex g_lifecycle_mtx;
std::vector<std::unique_ptr<gai::Channel>> g_channels;

gai::Channel* FindByPort(int port) {
    for (auto& c : g_channels) {
        if (c && c->Port() == port) return c.get();
    }
    return nullptr;
}

}  // namespace

extern "C" {

__declspec(dllexport) int __cdecl GAI_InitializeChannels(const int* ports, int count) {
    std::lock_guard<std::mutex> lk(g_lifecycle_mtx);
    if (!g_channels.empty()) return 0;
    if (!ports || count <= 0) return 1;

    try {
        // The host generates consecutive ports so they never repeat; FindByPort
        // is a linear scan that silently returns the first hit, so a duplicate
        // would route every /SetParameters to one channel and starve the rest.
        std::set<int> seen;
        for (int i = 0; i < count; ++i) {
            if (!seen.insert(ports[i]).second) return 1;
        }

        std::vector<std::unique_ptr<gai::Channel>> chans;
        chans.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            auto c = std::unique_ptr<gai::Channel>(new gai::Channel(ports[i]));
            c->Start();
            chans.push_back(std::move(c));
        }
        g_channels = std::move(chans);
        std::cout << "[GAI] " << count << " channel(s) initialized" << std::endl;
        return 0;
    } catch (...) {
        // C ABI must not let C++ exceptions escape. Half-built channels are
        // cleaned up by unique_ptr destruction (~Channel stops + joins).
        return 1;
    }
}

__declspec(dllexport) int __cdecl GAI_SetChannelParameters(int port, const GAI_Settings* parameters) {
    if (!parameters) return 1;
    std::lock_guard<std::mutex> lk(g_lifecycle_mtx);
    gai::Channel* c = FindByPort(port);
    if (!c) return 1;
    try { c->ApplyParameters(*parameters); }
    catch (...) { return 1; }
    return 0;
}

__declspec(dllexport) void __cdecl GAI_RegisterCallback(GAI_DetectionCallback cb) {
    gai::g_callback.store(cb, std::memory_order_release);
}

__declspec(dllexport) int __cdecl GAI_Deinitialize(void) {
    std::vector<std::unique_ptr<gai::Channel>> chans;
    {
        std::lock_guard<std::mutex> lk(g_lifecycle_mtx);
        chans.swap(g_channels);
    }
    // Signal every worker first, then join: a callback blocked inside the
    // host only stalls its own channel's join, not the stop signal of the
    // others.
    for (auto& c : chans) {
        if (c) c->RequestStop();
    }
    for (auto& c : chans) {
        if (c) {
            try { c->Join(); } catch (...) {}
        }
    }
    chans.clear();
    std::cout << "[GAI] deinitialized" << std::endl;
    return 0;
}

}
