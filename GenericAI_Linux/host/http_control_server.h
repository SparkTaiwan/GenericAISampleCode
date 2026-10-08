#pragma once

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <string>
#include <thread>

namespace httplib { class Server; }

namespace gai_host {

class ParameterStore;

// HTTP control plane for one channel port (HttpListenerHost.cs). Endpoints
// match spark.recorder/modules/AIService/Generic/AStreamPerimeter_GenericAI.cpp:
//   GET  /Alive              {"status":"ok"|"error","version":"1.3"[,"message":...]}
//   GET  /GetLicense         empty body
//   GET  /GetSettingsSchema  SettingsSchema::Json()
//   POST /SetParameters      per-channel config (url, resolution, ROIs, ai_settings)
class HttpControlServer {
public:
    HttpControlServer(int port, ParameterStore& params, const std::string& host);
    ~HttpControlServer();

    HttpControlServer(const HttpControlServer&) = delete;
    HttpControlServer& operator=(const HttpControlServer&) = delete;

    // Binds the port and starts serving on a background thread. False when the
    // bind fails (port in use / permission) -> the host exits with code 3.
    bool Start();
    void Stop();

    // Applies schema-shaped or flat ai_settings to native for one channel.
    // Shared by /SetParameters and the startup default-seeding in main.
    static void ApplyAiSettingsToNative(int port, const nlohmann::json& ai_settings);

private:
    void Register();

    const int port_;
    const std::string host_;
    ParameterStore& params_;
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
};

}  // namespace gai_host
