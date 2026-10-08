#include "pipeline.h"

#include "console_log.h"
#include "file_logger.h"
#include "gai_native_api.h"

#include <httplib.h>
#include <turbojpeg.h>
#ifdef USE_ZMQ
#include <zmq.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace gai_host {

namespace {

constexpr auto kIdlePoll = std::chrono::milliseconds(5);
constexpr auto kRetryBackoff = std::chrono::milliseconds(1000);

// ---- FramePool -----------------------------------------------------------
// Buffers above this are not pooled (4K I420 = 12 MB fits).
constexpr std::size_t kFramePoolMaxBytes = 16u * 1024 * 1024;
constexpr std::size_t kFramePoolMaxBuffers = 16;

std::mutex g_pool_mtx;
std::vector<std::vector<unsigned char>> g_pool;

// ---- draw_roi keyframe overlay -------------------------------------------
// Red in BT.601: Y=76, U=85, V=255. 2px lines, closed polygons; points are in
// frame coordinates, groups are rois_count blocks of node_count points.
constexpr unsigned char kRoiY = 76;
constexpr unsigned char kRoiU = 85;
constexpr unsigned char kRoiV = 255;

void PlotRed(unsigned char* frame, int width, int height, int cw, int ch, int u_off, int v_off,
             int x, int y) {
    for (int oy = 0; oy <= 1; ++oy) {
        const int yy = y + oy;
        if (yy < 0 || yy >= height) continue;
        const int row = yy * width;
        for (int ox = 0; ox <= 1; ++ox) {
            const int xx = x + ox;
            if (xx < 0 || xx >= width) continue;
            frame[row + xx] = kRoiY;
            const int cx = xx >> 1, cy = yy >> 1;  // 4:2:0 subsampling
            if (cx < cw && cy < ch) {
                const int c = cy * cw + cx;
                frame[u_off + c] = kRoiU;
                frame[v_off + c] = kRoiV;
            }
        }
    }
}

// Bresenham line, 2px thick, red.
void DrawLine(unsigned char* frame, int width, int height, int cw, int ch, int u_off, int v_off,
              int x0, int y0, int x1, int y1) {
    const int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    // Bound the iteration count so a stray (large/negative) coordinate can't spin.
    int guard = dx + dy + 4;
    while (guard-- > 0) {
        PlotRed(frame, width, height, cw, ch, u_off, v_off, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void DrawRoisOnI420(std::vector<unsigned char>& frame, int width, int height,
                    const std::vector<GAI_Roi>& rois, int rois_count, int node_count) {
    if (width <= 0 || height <= 0) return;
    const long long need = static_cast<long long>(rois_count) * node_count;
    if (need <= 0 || static_cast<long long>(rois.size()) < need) return;
    const int y_size = width * height;
    const int cw = width / 2, ch = height / 2;
    const int u_off = y_size, v_off = y_size + cw * ch;
    if (static_cast<long long>(frame.size()) < static_cast<long long>(y_size) + 2LL * cw * ch) return;

    for (int r = 0; r < rois_count; ++r) {
        const int base = r * node_count;
        for (int n = 0; n < node_count; ++n) {
            const GAI_Roi& p0 = rois[base + n];
            const GAI_Roi& p1 = rois[base + (n + 1) % node_count];  // close the polygon
            DrawLine(frame.data(), width, height, cw, ch, u_off, v_off, p0.x, p0.y, p1.x, p1.y);
        }
    }
}

// libjpeg-turbo I420 -> JPEG. One handle per EncodeWorker thread.
class JpegEncoder {
public:
    ~JpegEncoder() {
        if (handle_) tjDestroy(handle_);
    }

    std::vector<unsigned char> EncodeI420(const unsigned char* yuv, int length,
                                          int width, int height, int quality) {
        const int expected = width * height * 3 / 2;
        if (!yuv || length < expected) throw std::invalid_argument("Invalid I420 buffer");
        if (!handle_) {
            handle_ = tjInitCompress();
            if (!handle_) throw std::runtime_error("tjInitCompress failed");
        }
        unsigned char* jpeg = nullptr;
        unsigned long jpeg_size = 0;
        const int rc = tjCompressFromYUV(handle_, yuv, width, 1, height, TJSAMP_420,
                                         &jpeg, &jpeg_size, quality, 0);
        if (rc != 0) {
            const char* err = tjGetErrorStr2(handle_);
            std::string msg = err ? err : "tjCompressFromYUV failed";
            if (jpeg) tjFree(jpeg);
            throw std::runtime_error(msg);
        }
        std::vector<unsigned char> out(jpeg, jpeg + jpeg_size);
        tjFree(jpeg);
        return out;
    }

private:
    tjhandle handle_ = nullptr;
};

// Round-robin take over every channel's EncodeQ (RoundRobinTaker.cs):
//   idx >= 0 hit; -1 all queues completed and drained; -2 all empty, some open.
int TryTakeRoundRobin(const std::vector<ChannelHandle*>& channels, int& cursor, RawDetection& item) {
    const int n = static_cast<int>(channels.size());
    for (int i = 0; i < n; ++i) {
        const int probe = (cursor + i) % n;
        if (channels[probe]->EncodeQ().TryTake(item)) {
            cursor = (probe + 1) % n;
            return probe;
        }
    }
    for (int i = 0; i < n; ++i) {
        if (!channels[i]->EncodeQ().IsCompleted()) return -2;
    }
    return -1;
}

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void AppendBase64(std::string& out, const std::vector<unsigned char>& in) {
    const std::size_t n = in.size();
    out.reserve(out.size() + ((n + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i < n) {
        unsigned v = in[i] << 16;
        if (i + 1 < n) v |= in[i + 1] << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += (i + 1 < n) ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
}

// Names/values here are ASCII class names and digits; quote defensively anyway.
void AppendJsonString(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
            out += buf;
        } else {
            out += c;
        }
    }
    out += '"';
}

}  // namespace

// ---- FramePool -----------------------------------------------------------

std::vector<unsigned char> FramePool::Rent(std::size_t size) {
    {
        std::lock_guard<std::mutex> lk(g_pool_mtx);
        for (std::size_t i = 0; i < g_pool.size(); ++i) {
            if (g_pool[i].capacity() >= size) {
                std::vector<unsigned char> buf = std::move(g_pool[i]);
                g_pool.erase(g_pool.begin() + static_cast<std::ptrdiff_t>(i));
                buf.resize(size);
                return buf;
            }
        }
    }
    return std::vector<unsigned char>(size);
}

void FramePool::Return(std::vector<unsigned char>&& buf) {
    if (buf.capacity() == 0 || buf.capacity() > kFramePoolMaxBytes) return;
    std::lock_guard<std::mutex> lk(g_pool_mtx);
    if (g_pool.size() < kFramePoolMaxBuffers) g_pool.push_back(std::move(buf));
}

// ---- FrameDispatcher -----------------------------------------------------

FrameDispatcher* FrameDispatcher::instance_ = nullptr;

FrameDispatcher::FrameDispatcher(const std::map<int, ChannelHandle*>& by_channel_id, DropCounter& drops)
    : by_channel_id_(by_channel_id), drops_(drops) {}

void FrameDispatcher::Register() {
    instance_ = this;
    GAI_RegisterCallback(&FrameDispatcher::OnNativeCallback);
    registered_ = true;
}

void FrameDispatcher::Unregister() {
    if (!registered_) return;
    GAI_RegisterCallback(nullptr);
    registered_ = false;
}

// Runs on the native DispatchThread: copy everything off the native buffers
// before returning, and never let an exception unwind into native code.
void FrameDispatcher::OnNativeCallback(int channel_id, int width, int height,
                                       const unsigned char* frame_i420, int frame_size,
                                       unsigned long long timestamp,
                                       const GAI_Roi* rois_flat, int rois_count, int node_count,
                                       const int* class_counts, int class_counts_len) {
    FrameDispatcher* self = instance_;
    if (!self) return;
    try {
        if (frame_size <= 0 || !frame_i420 || rois_count <= 0 || node_count <= 0) return;

        auto it = self->by_channel_id_.find(channel_id);
        if (it == self->by_channel_id_.end()) {
            self->drops_.IncCallbackDropped();
            FileLogger::Warn("Detection callback: unknown channel_id=" + std::to_string(channel_id));
            return;
        }
        ChannelHandle* channel = it->second;

        RawDetection raw;
        raw.width = width;
        raw.height = height;
        raw.timestamp = timestamp;
        raw.frame_i420 = FramePool::Rent(static_cast<std::size_t>(frame_size));
        std::memcpy(raw.frame_i420.data(), frame_i420, static_cast<std::size_t>(frame_size));
        raw.frame_length = frame_size;
        const int total = rois_count * node_count;
        if (rois_flat) raw.rois_flat.assign(rois_flat, rois_flat + total);
        else raw.rois_flat.assign(static_cast<std::size_t>(total), GAI_Roi{-1, -1});
        raw.rois_count = rois_count;
        raw.node_count = node_count;
        if (class_counts && class_counts_len > 0)
            raw.class_counts.assign(class_counts, class_counts + class_counts_len);
        raw.port = channel_id;

        // Blocking Add: pressure backs up to the native DispatchThread. False
        // means CompleteAdding (shutdown) — expected, not a failure.
        if (!channel->EncodeQ().Add(std::move(raw))) {
            FramePool::Return(std::move(raw.frame_i420));
            return;
        }

        FileLogger::Info("Detection callback: ch=" + std::to_string(channel_id) +
                         " size=" + std::to_string(frame_size) + " w=" + std::to_string(width) +
                         " h=" + std::to_string(height) + " rois=" + std::to_string(rois_count) +
                         " nodes=" + std::to_string(node_count) + " ts=" + std::to_string(timestamp));
    } catch (const std::exception& ex) {
        try { FileLogger::Error(std::string("FrameDispatcher.OnNativeCallback: ") + ex.what()); } catch (...) {}
    } catch (...) {
    }
}

// ---- EncodeWorker --------------------------------------------------------

EncodeWorker::EncodeWorker(std::vector<ChannelHandle*> channels, DropCounter& drops, StopToken& stop)
    : channels_(std::move(channels)), drops_(drops), stop_(stop) {}

void EncodeWorker::Start() { thread_ = std::thread(&EncodeWorker::Run, this); }

void EncodeWorker::Join() {
    if (thread_.joinable()) thread_.join();
}

void EncodeWorker::Run() {
    JpegEncoder encoder;
    try {
        int cursor = 0;
        while (!stop_.IsCancelled()) {
            RawDetection raw;
            const int idx = TryTakeRoundRobin(channels_, cursor, raw);
            if (idx == -1) break;
            if (idx == -2) {
                std::this_thread::sleep_for(kIdlePoll);
                continue;
            }

            ChannelHandle* channel = channels_[static_cast<std::size_t>(idx)];

            // Trigger-interval throttle, checked BEFORE the JPEG encode so
            // suppressed frames cost nothing.
            if (!channel->TryPassTriggerInterval(channel->Parameters().TriggerIntervalSec())) {
                FramePool::Return(std::move(raw.frame_i420));
                continue;
            }

            try {
                int quality = channel->Parameters().JpgQuality();
                if (quality <= 0) quality = 30;

                // draw_roi: overlay the ROI/box outlines onto the keyframe
                // before encode (detection already consumed this frame).
                if (channel->Parameters().DrawRoi() && raw.rois_count > 0 && raw.node_count > 1) {
                    DrawRoisOnI420(raw.frame_i420, raw.width, raw.height,
                                   raw.rois_flat, raw.rois_count, raw.node_count);
                }

                Envelope env;
                env.keyframe = encoder.EncodeI420(raw.frame_i420.data(), raw.frame_length,
                                                  raw.width, raw.height, quality);

                // Per-class counts -> metadata items named by the schema key
                // (no "__Count" suffix; the schema's counting list says which count).
                std::string summary;
                const int m = std::min(static_cast<int>(raw.class_counts.size()), kSupportedClassCount);
                for (int i = 0; i < m; ++i) {
                    if (raw.class_counts[i] <= 0) continue;
                    env.items.emplace_back(kSupportedClasses[i], std::to_string(raw.class_counts[i]));
                    if (!summary.empty()) summary += ", ";
                    summary += std::string(kSupportedClasses[i]) + "=" + std::to_string(raw.class_counts[i]);
                }
                if (!summary.empty())
                    ConsoleLog::WriteLine("[ch" + std::to_string(raw.port) + "] Recognized: " + summary);

                env.port_num = raw.port;
                env.timestamp = raw.timestamp;
                env.rois_flat = std::move(raw.rois_flat);
                env.rois_count = raw.rois_count;
                env.node_count = raw.node_count;

                // The envelope no longer references the I420 buffer.
                FramePool::Return(std::move(raw.frame_i420));

                // Blocking Add: pressure backs up through EncodeQ to native.
                if (!channel->SendQ().Add(std::move(env))) return;  // shutdown
            } catch (const std::exception& ex) {
                FramePool::Return(std::move(raw.frame_i420));
                FileLogger::Error(std::string("EncodeWorker frame failed: ") + ex.what());
            }
        }
    } catch (const std::exception& ex) {
        FileLogger::Error(std::string("EncodeWorker fatal: ") + ex.what());
    }
}

// ---- ZmqResultSender -----------------------------------------------------

#ifdef USE_ZMQ
ZmqResultSender::ZmqResultSender(const std::string& endpoint, int channel_count) : endpoint_(endpoint) {
    ctx_ = zmq_ctx_new();
    if (!ctx_) throw std::runtime_error("zmq_ctx_new failed");
    sock_ = zmq_socket(ctx_, ZMQ_PUSH);
    if (!sock_) {
        zmq_ctx_term(ctx_);
        ctx_ = nullptr;
        throw std::runtime_error("zmq_socket(PUSH) failed");
    }
    // HWM scales with the channel count (all channels share this PUSH); the
    // SendWorker parks + retries on a full queue.
    int hwm = std::max(64, channel_count * 2), sndtimeo = 1000, linger = 0;
    zmq_setsockopt(sock_, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    zmq_setsockopt(sock_, ZMQ_SNDTIMEO, &sndtimeo, sizeof(sndtimeo));
    zmq_setsockopt(sock_, ZMQ_LINGER, &linger, sizeof(linger));
    if (zmq_connect(sock_, endpoint.c_str()) != 0) {
        const std::string err = zmq_strerror(zmq_errno());
        Close();
        throw std::runtime_error("zmq_connect failed for result endpoint: " + endpoint + " (" + err + ")");
    }
}

ZmqResultSender::~ZmqResultSender() { Close(); }

// True if queued for sending; false on timeout (no consumer / HWM) so the
// caller parks + retries like the HTTP path.
bool ZmqResultSender::Send(const std::string& payload) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (closed_ || !sock_) {
        ConsoleLog::WriteLine(std::string("[ZMQ result] skipped: sender not ready (disposed=") +
                              (closed_ ? "True" : "False") + ") -> " + endpoint_);
        return false;
    }
    const int r = zmq_send(sock_, payload.data(), payload.size(), 0);
    if (r >= 0) {
        const std::string msg = "[ZMQ result] zmq_send OK: rc=" + std::to_string(r) +
                                " bytes=" + std::to_string(payload.size()) + " -> " + endpoint_;
        ConsoleLog::WriteLine(msg);
        FileLogger::Info(msg);
    } else {
        const std::string msg = "[ZMQ result] zmq_send FAILED: rc=" + std::to_string(r) +
                                " errno=" + std::to_string(zmq_errno()) +
                                " bytes=" + std::to_string(payload.size()) + " -> " + endpoint_ +
                                " (no consumer within SNDTIMEO?)";
        ConsoleLog::WriteLine(msg);
        FileLogger::Warn(msg);
    }
    return r >= 0;
}

void ZmqResultSender::Close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (closed_) return;
    closed_ = true;
    if (sock_) { zmq_close(sock_); sock_ = nullptr; }
    if (ctx_) { zmq_ctx_term(ctx_); ctx_ = nullptr; }
}
#endif  // USE_ZMQ

// ---- Envelope JSON -------------------------------------------------------

std::string SerializeEnvelope(const Envelope& env) {
    // Field order and shapes match HttpEnvelope.cs under Newtonsoft:
    // version, port_num, items (omitted when empty), keyframe (base64),
    // timestamp, rois_rects ([[{x,y},...],...]).
    std::string out;
    out.reserve(256 + env.keyframe.size() * 4 / 3 + env.rois_flat.size() * 24);
    out += "{\"version\":";
    AppendJsonString(out, Protocol::kVersion);
    out += ",\"port_num\":";
    out += std::to_string(env.port_num);
    if (!env.items.empty()) {
        out += ",\"items\":[";
        for (std::size_t i = 0; i < env.items.size(); ++i) {
            if (i) out += ',';
            out += "{\"name\":";
            AppendJsonString(out, env.items[i].first);
            out += ",\"value\":";
            AppendJsonString(out, env.items[i].second);
            out += '}';
        }
        out += ']';
    }
    out += ",\"keyframe\":\"";
    AppendBase64(out, env.keyframe);
    out += "\",\"timestamp\":";
    out += std::to_string(env.timestamp);
    out += ",\"rois_rects\":[";
    for (int r = 0; r < env.rois_count; ++r) {
        if (r) out += ',';
        out += '[';
        for (int n = 0; n < env.node_count; ++n) {
            const std::size_t k = static_cast<std::size_t>(r) * env.node_count + n;
            if (k >= env.rois_flat.size()) break;
            if (n) out += ',';
            out += "{\"x\":";
            out += std::to_string(env.rois_flat[k].x);
            out += ",\"y\":";
            out += std::to_string(env.rois_flat[k].y);
            out += '}';
        }
        out += ']';
    }
    out += "]}";
    return out;
}

// ---- SendWorker ----------------------------------------------------------

SendWorker::SendWorker(std::vector<ChannelHandle*> channels, DropCounter& drops, StopToken& stop
#ifdef USE_ZMQ
                       , ZmqResultSender* zmq
#endif
                       )
    : channels_(std::move(channels)), drops_(drops), stop_(stop)
#ifdef USE_ZMQ
    , zmq_(zmq)
#endif
{}

void SendWorker::Start() { thread_ = std::thread(&SendWorker::Run, this); }

void SendWorker::Join() {
    if (thread_.joinable()) thread_.join();
}

// Scans channels round-robin. A fresh envelope gets one send attempt; on
// failure the payload is parked on its channel, which then takes no new
// envelopes until the parked one goes through — a dead consumer backpressures
// that channel only. Frames are never dropped.
void SendWorker::Run() {
    try {
        const int n = static_cast<int>(channels_.size());
        int cursor = 0;
        while (!stop_.IsCancelled()) {
            int worked = -1;
            for (int i = 0; i < n; ++i) {
                const int idx = (cursor + i) % n;
                ChannelHandle* channel = channels_[static_cast<std::size_t>(idx)];

                // HTTP mode needs analytics_event_api_url; until it is set the
                // envelopes stay queued so backpressure reaches native.
                const std::string url = channel->Parameters().Url();
#ifdef USE_ZMQ
                if (!zmq_ && url.empty()) continue;
#else
                if (url.empty()) continue;
#endif

                std::string parked;
                if (channel->TryClaimParkedSend(parked)) {
                    bool ok = false;
                    try {
                        ok = TrySendOnce(url, parked, channel->Port());
                    } catch (...) {
                    }
                    channel->CompleteParkedSend(std::move(parked), ok, kRetryBackoff);
                    worked = idx;
                    break;
                }

                if (channel->HasParkedSend()) continue;

                Envelope env;
                if (!channel->SendQ().TryTake(env)) continue;

                // Serialise once; the same bytes are reused across retries.
                std::string payload = SerializeEnvelope(env);
                if (!TrySendOnce(url, payload, channel->Port()))
                    channel->ParkSend(std::move(payload), kRetryBackoff);
                worked = idx;
                break;
            }

            if (worked >= 0) {
                cursor = (worked + 1) % n;
                continue;
            }

            // Nothing serviceable. Exit once every queue is completed and
            // drained (parked payloads are abandoned at shutdown).
            bool all_completed = true;
            for (ChannelHandle* c : channels_) {
                if (!c->SendQ().IsCompleted()) {
                    all_completed = false;
                    break;
                }
            }
            if (all_completed) break;

            std::this_thread::sleep_for(kIdlePoll);
        }
    } catch (const std::exception& ex) {
        FileLogger::Error(std::string("SendWorker fatal: ") + ex.what());
    }
}

bool SendWorker::TrySendOnce(const std::string& url, const std::string& payload, int port) {
#ifdef USE_ZMQ
    if (zmq_) {
        const bool ok = zmq_->Send(payload);
        if (ok) {
            ConsoleLog::WriteLine("Detected!! send analytics result to server!!");
            FileLogger::Info("Analytics result sent over ZMQ (ch=" + std::to_string(port) + ")");
        } else {
            ConsoleLog::WriteLine("ZMQ result send timed out (no consumer?), parked for retry");
            FileLogger::Warn("ZMQ result send failed (ch=" + std::to_string(port) + "), parked for retry");
        }
        return ok;
    }
#endif

    std::string error;
    if (HttpPost(url, payload, error)) {
        ConsoleLog::WriteLine("Detected!! send analytics result to server!!");
        FileLogger::Info("Analytics result posted ok (ch=" + std::to_string(port) + ")");
        return true;
    }
    if (stop_.IsCancelled()) return false;
    // Transient or persistent failure (timeout, refused, 5xx, DNS, ...). Warn,
    // not Error: parking + retry is the expected behaviour.
    ConsoleLog::WriteLine("Response: " + error);
    FileLogger::Warn("SendWorker POST failed (ch=" + std::to_string(port) + "), parked for retry: " + error);
    return false;
}

bool SendWorker::HttpPost(const std::string& url, const std::string& payload, std::string& error) {
    // Plain HTTP only: the recorder's analytics_event_api_url is http://.
    // https would need cpp-httplib built with CPPHTTPLIB_OPENSSL_SUPPORT.
    const std::string scheme = "http://";
    if (url.compare(0, scheme.size(), scheme) != 0) {
        error = "unsupported URL (only http:// is supported): " + url;
        return false;
    }
    const std::size_t path_pos = url.find('/', scheme.size());
    const std::string base = path_pos == std::string::npos ? url : url.substr(0, path_pos);
    const std::string path = path_pos == std::string::npos ? "/" : url.substr(path_pos);

    // One keep-alive client per worker thread and target, like the shared
    // HttpClient in HttpPostClient.cs (no per-request socket churn).
    thread_local std::map<std::string, std::unique_ptr<httplib::Client>> clients;
    auto& cli = clients[base];
    if (!cli) {
        cli.reset(new httplib::Client(base));
        cli->set_connection_timeout(5, 0);
        cli->set_read_timeout(5, 0);
        cli->set_write_timeout(5, 0);
        cli->set_keep_alive(true);
    }

    auto res = cli->Post(path.c_str(), payload, "application/json; charset=utf-8");
    if (!res) {
        error = "POST " + url + " failed (httplib error " + std::to_string(static_cast<int>(res.error())) + ")";
        clients.erase(base);  // drop a possibly broken keep-alive connection
        return false;
    }
    if (res->status < 200 || res->status > 299) {
        error = "POST " + url + " returned " + std::to_string(res->status) + ": " + res->body;
        return false;
    }
    return true;
}

}  // namespace gai_host
