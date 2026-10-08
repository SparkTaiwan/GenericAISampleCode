#pragma once

#include <string>

namespace gai_host {

// Built-in payload for GET /GetSettingsSchema (spec §5), ported from
// SettingsSchema.cs. ConfigClient renders the dynamic AI-settings UI from it;
// filled values come back in SetParameters.ai_settings.
class SettingsSchema {
public:
    // detector kind: 0 = Motion, 1 = Person. main calls this with the kind
    // native actually loaded (GAI_GetDetectorKind). Until then the motion
    // schema is served, mirroring gai::kDetectorKind.
    static void Configure(int detector_kind);
    static const std::string& Json();
};

}  // namespace gai_host
