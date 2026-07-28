#include "pch.h"
#include "host_log.h"

#include <atomic>

namespace gai {

namespace {
std::atomic<GAI_LogCallback> g_log_cb{nullptr};
std::atomic<bool> g_verbose{false};
}  // namespace

void SetVerboseLogging(bool enabled) {
    g_verbose.store(enabled, std::memory_order_relaxed);
}

bool VerboseLogging() {
    return g_verbose.load(std::memory_order_relaxed);
}

void SetHostLogCallback(GAI_LogCallback cb) {
    g_log_cb.store(cb, std::memory_order_release);
}

void HostLog(HostLogLevel level, const std::string& message) {
    GAI_LogCallback cb = g_log_cb.load(std::memory_order_acquire);
    if (!cb) return;
    cb(static_cast<int>(level), message.c_str());
}

}  // namespace gai
