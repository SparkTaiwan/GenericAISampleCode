#pragma once

// Small process-wide state types, ported 1:1 from GenericAI.App:
// Protocol.cs, Diagnostics/HealthState.cs, Http/ParameterStore.cs,
// Pipeline/DropCounter.cs.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace gai_host {

// Single source of truth for the recorder<->wrapper wire protocol version.
// Emitted on GET /Alive and stamped on every result envelope. Bump in
// lockstep with the "version" the recorder sends in /SetParameters.
namespace Protocol {
constexpr const char* kVersion = "1.3";
}

// Process-wide health surfaced on GET /Alive. Empty error => healthy.
class HealthState {
public:
    static void SetError(const std::string& message) {
        std::lock_guard<std::mutex> lk(mtx_);
        error_ = message.empty() ? "unknown error" : message;
    }
    static std::string Error() {
        std::lock_guard<std::mutex> lk(mtx_);
        return error_;
    }
    static bool IsHealthy() {
        std::lock_guard<std::mutex> lk(mtx_);
        return error_.empty();
    }

private:
    static inline std::mutex mtx_;
    static inline std::string error_;
};

// Set once native init has finished (including the default ai_settings seed,
// or the decision to run degraded). The HTTP listeners are up before that so
// the port-in-use check comes first; until then /SetParameters answers 503 so
// the recorder retries (it keeps m_needSendSetting until it gets a 200)
// instead of the settings being silently dropped by an uninitialised native
// side and then overwritten by the default seed.
class NativeReady {
public:
    static void Set() { ready_.store(true); }
    static bool Get() { return ready_.load(); }

private:
    static inline std::atomic<bool> ready_{false};
};

// Parameters that change at runtime via /SetParameters. The only writer is
// the channel's HTTP handler; workers read on the hot path.
class ParameterStore {
public:
    std::string Url() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return url_;
    }
    bool HasUrl() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return !url_.empty();
    }
    int  JpgQuality() const { return jpg_quality_.load(std::memory_order_relaxed); }
    int  TriggerIntervalSec() const { return trigger_interval_sec_.load(std::memory_order_relaxed); }
    bool DrawRoi() const { return draw_roi_.load(std::memory_order_relaxed); }

    // trigger_interval / draw_roi: has_* false keeps the previous value (field
    // absent from this SetParameters). 0 / false are meaningful values, so they
    // can't use the ">0 keeps previous" rule jpg_compress uses.
    void Update(const std::string& url, int jpg_compress,
                bool has_trigger_interval, int trigger_interval_sec,
                bool has_draw_roi, bool draw_roi) {
        std::lock_guard<std::mutex> lk(mtx_);
        // Clamp to turbojpeg's valid 1..100 range.
        if (jpg_compress > 0) jpg_quality_ = std::min(jpg_compress, 100);
        url_ = url;
        if (has_trigger_interval) {
            int v = std::max(0, std::min(trigger_interval_sec, 3600));
            trigger_interval_sec_ = v;
        }
        if (has_draw_roi) draw_roi_ = draw_roi;
    }

private:
    mutable std::mutex mtx_;
    std::string url_;
    // Initial values mirror the schema defaults (jpg_compress 30,
    // trigger_interval 1, draw_roi true): the schema default is only a UI hint
    // and is never pushed here on its own, so a first /SetParameters that
    // omits a field must still behave like the advertised default.
    std::atomic<int> jpg_quality_{30};
    std::atomic<int> trigger_interval_sec_{1};
    std::atomic<bool> draw_roi_{true};
};

// Lock-free counters for drops at each pipeline stage, reported once a minute.
class DropCounter {
public:
    void IncCallbackDropped() { callback_.fetch_add(1, std::memory_order_relaxed); }
    void IncEncodeDropped()   { encode_.fetch_add(1, std::memory_order_relaxed); }
    void IncSendDropped()     { send_.fetch_add(1, std::memory_order_relaxed); }

    void Snapshot(long long& cb, long long& enc, long long& send) const {
        cb = callback_.load();
        enc = encode_.load();
        send = send_.load();
    }

private:
    std::atomic<long long> callback_{0};
    std::atomic<long long> encode_{0};
    std::atomic<long long> send_{0};
};

}  // namespace gai_host
