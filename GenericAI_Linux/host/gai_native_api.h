#pragma once

// The GAI_* C ABI from native/exports.cpp. On Windows the C# host reaches it
// through P/Invoke into GenericAI.Native.dll; on Linux the native code is
// linked straight into the GenericAI executable and called directly.

#include "gai_abi.h"

extern "C" {

int  GAI_InitializeChannels(const int* ports, int count, int detector_kind);
int  GAI_SetChannelParameters(int port, const GAI_Settings* parameters);
int  GAI_SetChannelAiSettings(int port, float confidence, int class_mask, int sensitivity,
                              int threshold, float min_object_size, float max_object_size);
void GAI_RegisterCallback(GAI_DetectionCallback cb);
void GAI_RegisterLogCallback(GAI_LogCallback cb);
void GAI_SetVerbose(int enabled);
int  GAI_Deinitialize(void);
int  GAI_GetBackend(char* buf, int buf_len);
int  GAI_GetDetectorKind(void);
int  GAI_GetInitError(char* buf, int buf_len);
#ifdef USE_ZMQ
int  GAI_StartZmqReceiver(const char* endpoint);
void GAI_StopZmqReceiver(void);
#endif

}

namespace gai_host {

// Supported detection classes, FIXED order — mirrors native/class_table.h 1:1.
// Class bitmask bit i and the callback's class_counts[i] both index this.
constexpr const char* kSupportedClasses[] = {
    "person", "car", "bus", "truck", "motorcycle", "bicycle", "cat", "dog"};
constexpr int kSupportedClassCount = 8;

}  // namespace gai_host
