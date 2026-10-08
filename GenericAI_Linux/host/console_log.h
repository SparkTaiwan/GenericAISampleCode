#pragma once

#include <atomic>
#include <string>

namespace gai_host {

// Directory of the running executable (no trailing '/'). Empty on failure.
std::string ExeDir();

// Console output helper. Verbose/debug lines (WriteLine) are gated by a runtime
// switch loaded once at startup from a "GenericAI.Config" file next to the
// executable:
//
//     show_debug = 1          # host verbose console output (this class)
//     show_native_debug = 1   # native informational log lines ([AI]/[PersonDetector]/
//                             # [MotionDetector]/[channel]/[zmq]); mirrored into
//                             # native via GAI_SetVerbose (see main.cpp)
//     log_to_file = 1         # persist INFO/WARN/ERROR to a file (FileLogger)
//     log_dir = /some/dir     # Linux only: override the log directory
//
// All default OFF and are INDEPENDENT. ErrorLine (bad args, HTTP listener
// failed, native init failed, FATAL, ...) and native std::cerr errors are
// ALWAYS printed so a run that dies early still shows WHY.
class ConsoleLog {
public:
    static std::atomic<bool> Enabled;
    static std::atomic<bool> NativeDebug;

    static constexpr const char* kConfigFileName = "GenericAI.Config";

    // Read GenericAI.Config (if present). Each switch accepts
    // 1 / true / yes / on (case-insensitive). Never throws.
    static void LoadFromConfig();

    // Verbose/debug line -- gated by show_debug.
    static void WriteLine(const std::string& line);
    // Error/failure line -- ALWAYS shown (stderr).
    static void ErrorLine(const std::string& line);
};

}  // namespace gai_host
