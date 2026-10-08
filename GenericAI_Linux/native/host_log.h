#pragma once

#include "gai_abi.h"

#include <string>

namespace gai {

// Push-side of the host log bridge. Pull-style exports (GAI_GetBackend) can't
// carry asynchronous warnings, so the C# host registers a callback via
// GAI_RegisterLogCallback and native diagnostics flow through here into the
// host's FileLogger. No-op until a callback is registered; safe from any
// thread; the message pointer is only valid for the duration of the call.

enum class HostLogLevel { Info = 0, Warn = 1, Error = 2 };

void SetHostLogCallback(GAI_LogCallback cb);
void HostLog(HostLogLevel level, const std::string& message);

// Runtime verbose switch, driven by the host from the "GenericAI.Config"
// show_native_debug flag (GAI_SetVerbose). Off by default. Gates the
// informational native console lines ([AI]/[PersonDetector]/[MotionDetector]/
// [channel]/[zmq]) so they can be toggled WITHOUT a rebuild. std::cerr error
// lines ignore it (always shown); the per-frame [TIMING] instrumentation stays
// behind the compile-time gai::kEnableTimingLog. Safe from any thread.
void SetVerboseLogging(bool enabled);
bool VerboseLogging();

}  // namespace gai
