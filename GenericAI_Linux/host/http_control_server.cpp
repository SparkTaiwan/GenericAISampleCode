#include "http_control_server.h"

#include "console_log.h"
#include "file_logger.h"
#include "gai_native_api.h"
#include "settings_schema.h"
#include "state.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace gai_host {

namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxRequestBodyBytes = 1u * 1024 * 1024;

// Handler threads per listener. /SetParameters and /Alive are very low
// frequency; the cap only keeps a misbehaving recorder's retry storm from
// spawning threads.
constexpr std::size_t kHandlerThreads = 4;

// ---- Newtonsoft-like scalar access ---------------------------------------
// ai_settings may be flat ({"confidence":0.8}) or schema-shaped
// ({"fields":[{"key":"confidence","value":0.8}]}); the C# Extract* helpers
// accept both, and so do these.

const json* Field(const json& obj, const char* key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

bool IsScalar(const json& j) { return !j.is_object() && !j.is_array(); }

// Flat key first, then fields[] entry with that key (value ?? default).
const json* LookupScalar(const json& settings, const char* key) {
    if (const json* flat = Field(settings, key)) {
        if (IsScalar(*flat)) return flat;
    }
    const json* fields = Field(settings, "fields");
    if (fields && fields->is_array()) {
        for (const json& f : *fields) {
            const json* k = Field(f, "key");
            if (!k || !k->is_string() || k->get<std::string>() != key) continue;
            const json* v = Field(f, "value");
            if (!v) v = Field(f, "default");
            if (v && IsScalar(*v)) return v;
        }
    }
    return nullptr;
}

bool ParseWholeDouble(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0';
}

std::optional<double> ToDouble(const json& j) {
    if (j.is_number()) return j.get<double>();
    if (j.is_boolean()) return j.get<bool>() ? 1.0 : 0.0;
    double d;
    if (j.is_string() && ParseWholeDouble(j.get<std::string>(), d)) return d;
    return std::nullopt;
}

std::optional<int> ToInt(const json& j) {
    if (j.is_number_integer()) return static_cast<int>(j.get<long long>());
    // Convert.ToInt32(double) rounds half to even, like nearbyint's default mode.
    if (j.is_number_float()) return static_cast<int>(std::nearbyint(j.get<double>()));
    if (j.is_boolean()) return j.get<bool>() ? 1 : 0;
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        char* end = nullptr;
        long v = std::strtol(s.c_str(), &end, 10);
        if (!s.empty() && end != s.c_str() && *end == '\0') return static_cast<int>(v);
    }
    return std::nullopt;
}

std::optional<bool> ToBool(const json& j) {
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number()) return j.get<double>() != 0.0;
    if (j.is_string()) {
        std::string s = j.get<std::string>();
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (s == "true") return true;
        if (s == "false") return false;
    }
    return std::nullopt;
}

std::optional<float> ExtractFloat(const json& s, const char* key) {
    if (const json* v = LookupScalar(s, key)) {
        if (auto d = ToDouble(*v)) return static_cast<float>(*d);
    }
    return std::nullopt;
}

std::optional<int> ExtractInt(const json& s, const char* key) {
    if (const json* v = LookupScalar(s, key)) return ToInt(*v);
    return std::nullopt;
}

std::optional<bool> ExtractBool(const json& s, const char* key) {
    if (const json* v = LookupScalar(s, key)) return ToBool(*v);
    return std::nullopt;
}

// classes string_array -> supported-class bitmask (kSupportedClasses order).
std::optional<int> ExtractClassMask(const json& s) {
    const json* arr = Field(s, "classes");
    if (arr && !arr->is_array()) arr = nullptr;
    if (!arr) {
        const json* fields = Field(s, "fields");
        if (fields && fields->is_array()) {
            for (const json& f : *fields) {
                const json* k = Field(f, "key");
                if (!k || !k->is_string() || k->get<std::string>() != "classes") continue;
                const json* v = Field(f, "value");
                if (!v) v = Field(f, "default");
                arr = (v && v->is_array()) ? v : nullptr;
                break;
            }
        }
    }
    if (!arr) return std::nullopt;

    int mask = 0;
    for (const json& t : *arr) {
        if (!t.is_string()) continue;
        const std::string name = t.get<std::string>();
        for (int i = 0; i < kSupportedClassCount; ++i) {
            if (name == kSupportedClasses[i]) {
                mask |= (1 << i);
                break;
            }
        }
    }
    return mask;
}

std::string FloatOrDash(const std::optional<float>& v) {
    if (!v) return "-";
    std::ostringstream os;
    os << *v;
    return os.str();
}

// ---- /SetParameters body -> GAI_Settings ---------------------------------

// Nullable int field of the DTO: absent/null -> nullopt, convertible -> value,
// anything else is a 400 (Newtonsoft would throw JsonException).
std::optional<int> DtoInt(const json& obj, const char* key) {
    const json* v = Field(obj, key);
    if (!v || v->is_null()) return std::nullopt;
    auto i = ToInt(*v);
    if (!i) throw std::runtime_error(std::string("invalid value for ") + key);
    return i;
}

std::string DtoString(const json& obj, const char* key) {
    const json* v = Field(obj, key);
    if (!v || v->is_null()) return std::string();
    if (v->is_string()) return v->get<std::string>();
    if (IsScalar(*v)) return v->dump();
    throw std::runtime_error(std::string("invalid value for ") + key);
}

void CopyFixed(char* dst, std::size_t cap, const std::string& src) {
    std::size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

GAI_Settings ParseSettings(const std::string& body, int& roi_groups) {
    json data = json::parse(body);  // throws on malformed JSON
    if (data.is_null()) throw std::runtime_error("request body is empty or null");
    if (!data.is_object()) throw std::runtime_error("request body is not a JSON object");

    const std::string version = DtoString(data, "version");
    const auto image_width = DtoInt(data, "image_width");
    const auto image_height = DtoInt(data, "image_height");
    const auto jpg_compress = DtoInt(data, "jpg_compress");
    if (!image_width) throw std::runtime_error("image_width is required");
    if (!image_height) throw std::runtime_error("image_height is required");
    // v1.3 carries jpg_compress inside ai_settings (schema); only the legacy
    // v1.2 top-level field is required here.
    if (version != "1.3" && !jpg_compress) throw std::runtime_error("jpg_compress is required");

    GAI_Settings s;
    std::memset(&s, 0, sizeof(s));
    CopyFixed(s.version, sizeof(s.version), Protocol::kVersion);
    CopyFixed(s.analytics_event_api_url, sizeof(s.analytics_event_api_url),
              DtoString(data, "analytics_event_api_url"));
    s.image_width = *image_width;
    s.image_height = *image_height;
    s.jpg_compress = jpg_compress.value_or(0);
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 10; ++j) {
            s.rois[i][j].x = -1;
            s.rois[i][j].y = -1;
        }
        // Per-ROI object-detection tuning: -1 = unset -> native inherits the
        // channel ai_settings.
        s.confidence[i] = -1.f;
        s.class_mask[i] = -1;
        s.object_size_min[i] = -1.f;
        s.object_size_max[i] = -1.f;
    }

    roi_groups = 0;
    const json* rois = Field(data, "rois");
    if (rois && !rois->is_null()) {
        if (!rois->is_array()) throw std::runtime_error("rois must be an array");
        for (std::size_t i = 0; i < rois->size() && i < 10; ++i) {
            const json& g = (*rois)[i];
            if (g.is_null()) continue;
            if (!g.is_object()) throw std::runtime_error("rois[" + std::to_string(i) + "] must be an object");
            const auto sens = DtoInt(g, "sensitivity");
            const auto thr = DtoInt(g, "threshold");
            if (!sens) throw std::runtime_error("rois[" + std::to_string(i) + "].sensitivity is required");
            if (!thr) throw std::runtime_error("rois[" + std::to_string(i) + "].threshold is required");
            s.sensitivity[i] = *sens;
            s.threshold[i] = *thr;

            // Per-ROI object-detection settings (schema scope=roi).
            const json* ai = Field(g, "ai_settings");
            if (ai && !ai->is_null()) {
                if (!ai->is_object())
                    throw std::runtime_error("rois[" + std::to_string(i) + "].ai_settings must be an object");
                if (auto c = ExtractFloat(*ai, "confidence")) s.confidence[i] = *c;
                if (auto cm = ExtractClassMask(*ai)) s.class_mask[i] = *cm;
                if (auto mn = ExtractFloat(*ai, "object_size_min")) s.object_size_min[i] = *mn;
                if (auto mx = ExtractFloat(*ai, "object_size_max")) s.object_size_max[i] = *mx;
            }

            const json* rects = Field(g, "rects");
            if (rects && !rects->is_null()) {
                if (!rects->is_array()) throw std::runtime_error("rois[" + std::to_string(i) + "].rects must be an array");
                for (std::size_t j = 0; j < rects->size() && j < 10; ++j) {
                    const json& r = (*rects)[j];
                    if (r.is_null()) continue;
                    // RoiDto.x/y are non-nullable ints: absent -> 0.
                    s.rois[i][j].x = DtoInt(r, "x").value_or(0);
                    s.rois[i][j].y = DtoInt(r, "y").value_or(0);
                }
            }
            ++roi_groups;
        }
    }
    return s;
}

std::string JsonString(const std::string& s) {
    return json(s).dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace

HttpControlServer::HttpControlServer(int port, ParameterStore& params, const std::string& host)
    : port_(port),
      host_(host.empty() ? "127.0.0.1" : host),
      params_(params),
      server_(new httplib::Server()) {
    server_->new_task_queue = [] { return new httplib::ThreadPool(kHandlerThreads); };
    server_->set_payload_max_length(kMaxRequestBodyBytes);
    Register();
}

HttpControlServer::~HttpControlServer() { Stop(); }

bool HttpControlServer::Start() {
    // "+" / "*" are the HttpListener spellings for "all interfaces".
    const std::string bind_host = (host_ == "+" || host_ == "*") ? "0.0.0.0" : host_;
    if (!server_->bind_to_port(bind_host.c_str(), port_)) {
        ConsoleLog::ErrorLine("HTTP listener FAILED on http://" + host_ + ":" + std::to_string(port_) +
                              "/ : bind failed (port in use, or address not on this machine)");
        FileLogger::Error("HTTP listener bind failed on port " + std::to_string(port_));
        return false;
    }
    thread_ = std::thread([this] { server_->listen_after_bind(); });
    ConsoleLog::WriteLine("HTTP Server started: http://" + host_ + ":" + std::to_string(port_) +
                          "/  (reachable for /Alive,/SetParameters)");
    FileLogger::Info("HTTP listener started on http://" + host_ + ":" + std::to_string(port_) + "/");
    return true;
}

void HttpControlServer::Stop() {
    if (!thread_.joinable()) return;
    try { server_->stop(); } catch (...) {}
    thread_.join();
    ConsoleLog::WriteLine("HTTP Server stopped.");
}

void HttpControlServer::Register() {
    auto remote_of = [](const httplib::Request& req) {
        return req.remote_addr + ":" + std::to_string(req.remote_port);
    };

    server_->Get("/Alive", [remote_of](const httplib::Request& req, httplib::Response& res) {
        try {
            // HTTP 200 = the process is alive; the body reports health. The
            // recorder reads `status` to surface a degraded init and `version`
            // to pick the /SetParameters protocol level.
            const bool healthy = HealthState::IsHealthy();
            std::string body = std::string("{\"status\":\"") + (healthy ? "ok" : "error") +
                               "\",\"version\":" + JsonString(Protocol::kVersion);
            if (!healthy) body += ",\"message\":" + JsonString(HealthState::Error());
            body += "}";
            res.set_content(body, "application/json");
            const std::string remote = remote_of(req);
            ConsoleLog::WriteLine("/Alive from " + remote + " -> 200 (healthy=" +
                                  (healthy ? "True" : "False") + ")");
            FileLogger::Info("Alive received from " + remote + " (healthy=" +
                             (healthy ? "True" : "False") + ")");
        } catch (const std::exception& ex) {
            FileLogger::Error(std::string("HTTP handler error (/Alive): ") + ex.what());
            res.status = 500;
            res.set_content("Server Error", "text/plain");
        }
    });

    server_->Get("/GetLicense", [remote_of](const httplib::Request& req, httplib::Response& res) {
        res.set_content("", "text/plain");
        FileLogger::Info("GetLicense received from " + remote_of(req));
    });

    server_->Get("/GetSettingsSchema", [remote_of](const httplib::Request& req, httplib::Response& res) {
        const std::string& schema = SettingsSchema::Json();
        res.set_content(schema, "application/json");
        const std::string remote = remote_of(req);
        ConsoleLog::WriteLine("/GetSettingsSchema from " + remote + " -> 200 (" +
                              std::to_string(schema.size()) + " bytes)");
        FileLogger::Info("GetSettingsSchema served to " + remote);
    });

    server_->Post("/SetParameters", [this, remote_of](const httplib::Request& req, httplib::Response& res) {
        const std::string remote = remote_of(req);
        const std::string& body = req.body;
        ConsoleLog::WriteLine("Received SetParameters request: " + body);

        if (!NativeReady::Get()) {
            FileLogger::Warn("SetParameters from " + remote + " deferred: detector still initializing (503)");
            res.status = 503;
            res.set_content("Service Unavailable: detector initializing, retry", "text/plain");
            return;
        }

        // `version` drives where jpg_compress comes from: v1.3 reads it from
        // ai_settings (schema); v1.2 keeps the legacy top-level field.
        std::string version;
        std::optional<int> jpg_from_schema;
        std::optional<int> trigger_interval;
        std::optional<bool> draw_roi;
        try {
            json root = json::parse(body);
            if (const json* v = Field(root, "version")) {
                if (v->is_string()) version = v->get<std::string>();
            }
            // draw_roi: top-level built-in flag (the perimeter's
            // draw_rect_on_jpg), NOT a schema ai_settings field.
            draw_roi = ExtractBool(root, "draw_roi");
            if (const json* ai = Field(root, "ai_settings")) {
                if (!ai->is_null()) {
                    const std::string s = ai->dump(-1, ' ', false, json::error_handler_t::replace);
                    ConsoleLog::WriteLine("ai_settings received from " + remote + ": " + s);
                    FileLogger::Info("ai_settings received from " + remote + ": " + s);
                    jpg_from_schema = ExtractInt(*ai, "jpg_compress");
                    // Wrapper-side send throttle (not a native setting).
                    trigger_interval = ExtractInt(*ai, "trigger_interval");
                    ApplyAiSettingsToNative(port_, *ai);
                }
            }
        } catch (...) {
            // Body already logged above; ParseSettings reports the error.
        }

        GAI_Settings settings;
        int roi_groups = 0;
        try {
            settings = ParseSettings(body, roi_groups);
        } catch (const std::exception& ex) {
            FileLogger::Error("SetParameters from " + remote + " parse failed: " + ex.what());
            res.status = 400;
            res.set_content(std::string("Bad Request: ") + ex.what(), "text/plain");
            return;
        }

        if (version == "1.3" && jpg_from_schema) settings.jpg_compress = *jpg_from_schema;

        try {
            params_.Update(settings.analytics_event_api_url, settings.jpg_compress,
                           trigger_interval.has_value(), trigger_interval.value_or(0),
                           draw_roi.has_value(), draw_roi.value_or(false));
            const int rc = GAI_SetChannelParameters(port_, &settings);
            // Degraded init has no scheduler, so rc != 0 there by design; the
            // recorder already learns about that from /Alive (C# answers 200).
            if (rc != 0 && HealthState::IsHealthy())
                throw std::runtime_error("GAI_SetChannelParameters returned " + std::to_string(rc));
        } catch (const std::exception& ex) {
            FileLogger::Error("SetParameters from " + remote + " failed: " + ex.what());
            res.status = 500;
            res.set_content(std::string("SetParameters failed: ") + ex.what(), "text/plain");
            return;
        }

        res.set_content("{\"message\":\"Parameters set successfully\"}", "application/json");
        FileLogger::Info("SetParameters received from " + remote + " (url=" +
                         settings.analytics_event_api_url + ", w=" + std::to_string(settings.image_width) +
                         ", h=" + std::to_string(settings.image_height) +
                         ", jpg=" + std::to_string(settings.jpg_compress) +
                         ", roi_groups=" + std::to_string(roi_groups) + ")");
    });

    // Fills the body for responses httplib produces itself (404 for unknown
    // routes, 413 when the body exceeds kMaxRequestBodyBytes).
    server_->set_error_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (res.status == 404) {
            res.set_content("Not Found", "text/plain");
        } else if (res.status == 413) {
            FileLogger::Warn("Request to " + req.path + " on port " + std::to_string(port_) +
                             " rejected: body exceeds " + std::to_string(kMaxRequestBodyBytes));
            res.set_content("Payload Too Large", "text/plain");
        }
    });
}

void HttpControlServer::ApplyAiSettingsToNative(int port, const json& ai_settings) {
    const auto conf = ExtractFloat(ai_settings, "confidence");
    const auto class_mask = ExtractClassMask(ai_settings);
    // sensitivity/threshold are ROI-scoped: they flow per ROI through
    // GAI_SetChannelParameters, never through this channel-wide call (pushing
    // them here would flatten every region to one value).
    const auto obj_min = ExtractFloat(ai_settings, "object_size_min");
    const auto obj_max = ExtractFloat(ai_settings, "object_size_max");
    if (!conf && !class_mask && !obj_min && !obj_max) return;

    GAI_SetChannelAiSettings(port, conf.value_or(-1.f), class_mask.value_or(-1), -1, -1,
                             obj_min.value_or(-1.f), obj_max.value_or(-1.f));

    char mask_hex[16];
    std::snprintf(mask_hex, sizeof(mask_hex), "%X", static_cast<unsigned>(class_mask.value_or(-1)));
    ConsoleLog::WriteLine("ai_settings applied: ch" + std::to_string(port) +
                          " confidence=" + FloatOrDash(conf) +
                          " classMask=0x" + mask_hex + " (sensitivity/threshold are per-ROI)" +
                          " object_size_min=" + FloatOrDash(obj_min) +
                          " object_size_max=" + FloatOrDash(obj_max));
}

}  // namespace gai_host
