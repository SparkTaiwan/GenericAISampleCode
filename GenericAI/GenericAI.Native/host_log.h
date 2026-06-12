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

}  // namespace gai
