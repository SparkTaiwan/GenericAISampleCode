#include "command_line_args.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>

namespace gai_host {

namespace {

std::string Trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// int.TryParse equivalent: whole string, optional sign, base 10, int range.
bool TryParseInt(const std::string& s, int& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    if (v < INT_MIN || v > INT_MAX) return false;
    out = static_cast<int>(v);
    return true;
}

}  // namespace

bool CommandLineArgs::TryParse(const std::vector<std::string>& args, CommandLineArgs& parsed,
                               std::string& error) {
    parsed = CommandLineArgs();
    error.clear();

    for (const std::string& raw : args) {
        std::size_t eq = raw.find('=');
        if (eq == std::string::npos || eq == 0) {
            error = "expected key=value, got '" + raw + "'";
            return false;
        }
        const std::string key = Lower(Trim(raw.substr(0, eq)));
        const std::string val = Trim(raw.substr(eq + 1));
        int n = 0;

        if (key == "port") {
            if (!TryParseInt(val, n) || n <= 0 || n > 65535) {
                error = "invalid port: " + val;
                return false;
            }
            parsed.port = n;
            parsed.port_from_args = true;
        } else if (key == "channel_count") {
            if (!TryParseInt(val, n) || n < 1) {
                error = "invalid channel_count: " + val;
                return false;
            }
            parsed.channel_count = n;
        } else if (key == "encode_workers") {
            if (!TryParseInt(val, n) || n < 1 || n > 16) {
                error = "invalid encode_workers: " + val;
                return false;
            }
            parsed.encode_workers = n;
        } else if (key == "send_workers") {
            if (!TryParseInt(val, n) || n < 1 || n > 16) {
                error = "invalid send_workers: " + val;
                return false;
            }
            parsed.send_workers = n;
        } else if (key == "mode") {
            const std::string m = Lower(val);
            if (m != "single" && m != "multi") {
                error = "invalid mode: " + val + " (expected single|multi)";
                return false;
            }
            parsed.mode = m;
        } else if (key == "detector") {
            const std::string d = Lower(val);
            if (d == "motion") {
                parsed.detector_kind = 0;
            } else if (d == "person" || d == "objectdetection" || d == "objdetection") {
                parsed.detector_kind = 1;
            } else {
                error = "invalid detector: " + val + " (expected motion|objectdetection)";
                return false;
            }
        } else if (key == "frame_endpoint") {
            // Minimal validation; libzmq parses the full endpoint.
            if (val.find("://") == std::string::npos) {
                error = "invalid frame_endpoint: " + val + " (expected e.g. tcp://host:port)";
                return false;
            }
            parsed.frame_endpoint = val;
        } else if (key == "result_endpoint") {
            if (val.find("://") == std::string::npos) {
                error = "invalid result_endpoint: " + val + " (expected e.g. tcp://host:port)";
                return false;
            }
            parsed.result_endpoint = val;
        } else if (key == "http_host") {
            if (val.empty()) {
                error = "invalid http_host: empty (expected e.g. 127.0.0.1, an interface IP, or + )";
                return false;
            }
            parsed.http_host = val;
        } else if (key == "server_ip") {
            if (val.empty()) {
                error = "invalid server_ip: empty (expected the recorder's IP, e.g. 172.20.1.36)";
                return false;
            }
            parsed.server_ip = val;
        } else if (key == "result_port") {
            if (!TryParseInt(val, n) || n <= 0 || n > 65535) {
                error = "invalid result_port: " + val + " (1..65535; the recorder's http_server_port)";
                return false;
            }
            parsed.result_port = n;
        } else if (key == "stream_port") {
            if (!TryParseInt(val, n) || n <= 0 || n > 65535) {
                error = "invalid stream_port: " + val + " (1..65535; the recorder's ai_stream_port)";
                return false;
            }
            parsed.stream_port = n;
        } else {
            error = "unknown key: " + key;
            return false;
        }
    }

    // Expand the server_ip + result_port + stream_port shorthand. They go
    // together; explicit frame_endpoint/result_endpoint take precedence.
    if (!parsed.server_ip.empty() || parsed.result_port > 0 || parsed.stream_port > 0) {
        if (parsed.server_ip.empty() || parsed.result_port <= 0 || parsed.stream_port <= 0) {
            error = "server_ip, result_port and stream_port must be given together";
            return false;
        }
        if (parsed.result_endpoint.empty())
            parsed.result_endpoint = "tcp://" + parsed.server_ip + ":" + std::to_string(parsed.result_port);
        if (parsed.frame_endpoint.empty())
            parsed.frame_endpoint = "tcp://" + parsed.server_ip + ":" + std::to_string(parsed.stream_port);
    }
    return true;
}

std::string CommandLineArgs::Usage() {
    return "Usage: GenericAI port=<int> channel_count=<N> mode=single|multi"
           " detector=motion|objectdetection server_ip=<recorderIP>"
           " result_port=<port> stream_port=<port>"
           " [http_host=<ip|+>] [encode_workers=<N>] [send_workers=<M>]"
           "\n  server_ip + result_port + stream_port => result=tcp://server_ip:result_port,"
           " frame=tcp://server_ip:stream_port. Use the recorder's http_server_port for"
           " result_port and its ai_stream_port for stream_port (the two are independent)."
           "\n  http_host = the /Alive,/SetParameters bind address (default 127.0.0.1;"
           " use the machine's IP or + for a remote recorder)."
           "\n  Advanced: frame_endpoint=tcp://host:port result_endpoint=tcp://host:port"
           " override the pair above; detector defaults to gai_config.h when omitted.";
}

}  // namespace gai_host
