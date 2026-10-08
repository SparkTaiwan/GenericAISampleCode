#pragma once

#include <string>
#include <vector>

namespace gai_host {

// Mirrors GenericAI.App/CommandLineArgs.cs: key=value pairs in any order; an
// unknown key, a missing '=' or an out-of-range value is a parse error
// (exit code 2).
struct CommandLineArgs {
    // Kept below the usual dynamic port range so the consecutive
    // [port, port + channel_count) block doesn't collide with outbound ports.
    static constexpr int kDefaultPort = 46000;

    int  port = kDefaultPort;
    bool port_from_args = false;
    int  channel_count = 1;
    int  encode_workers = 2;
    int  send_workers = 2;

    // "single" | "multi". Informational (channel_count drives the topology);
    // accepted so the recorder's mode= is not rejected.
    std::string mode = "multi";

    // 0 = Motion, 1 = Person (object detection), -1 = native compile-time default.
    int detector_kind = -1;

    // ZMQ frame plane (wrapper PULL connects here). Empty => MMF frames.
    std::string frame_endpoint;
    // ZMQ result plane (wrapper PUSH connects here). Empty => HTTP POST results.
    std::string result_endpoint;
    bool UseZmqFrames() const { return !frame_endpoint.empty(); }
    bool UseZmqResults() const { return !result_endpoint.empty(); }

    // HTTP control-plane bind host. "+" / "*" bind all interfaces.
    std::string http_host = "127.0.0.1";

    // server_ip + result_port + stream_port shorthand -> the two endpoints.
    std::string server_ip;
    int result_port = 0;
    int stream_port = 0;

    static bool TryParse(const std::vector<std::string>& args, CommandLineArgs& parsed,
                         std::string& error);
    static std::string Usage();
};

}  // namespace gai_host
