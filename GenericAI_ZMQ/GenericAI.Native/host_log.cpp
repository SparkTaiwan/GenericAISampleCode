#include "pch.h"
#include "host_log.h"

#include <atomic>

namespace gai {

namespace {
std::atomic<GAI_LogCallback> g_log_cb{nullptr};
}  // namespace

void SetHostLogCallback(GAI_LogCallback cb) {
    g_log_cb.store(cb, std::memory_order_release);
}

void HostLog(HostLogLevel level, const std::string& message) {
    GAI_LogCallback cb = g_log_cb.load(std::memory_order_acquire);
    if (!cb) return;
    cb(static_cast<int>(level), message.c_str());
}

}  // namespace gai
