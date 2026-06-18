#pragma once

#include <cstdint>

#pragma pack(push, 1)
struct GAI_Roi {
    int x;
    int y;
};

struct GAI_Settings {
    char version[32];
    char analytics_event_api_url[256];
    int  image_width;
    int  image_height;
    int  jpg_compress;
    int  sensitivity[10];
    int  threshold[10];
    GAI_Roi rois[10][10];
};
#pragma pack(pop)

// Diagnostic line pushed to the host's file logger. level: 0 = info,
// 1 = warning, 2 = error. message is ANSI, null-terminated, and only valid
// for the duration of the call.
typedef void(__stdcall* GAI_LogCallback)(int level, const char* message);

typedef void(__stdcall* GAI_DetectionCallback)(
    int channel_id,
    int width,
    int height,
    const unsigned char* frame_i420,
    int frame_size,
    unsigned long long timestamp,
    const GAI_Roi* rois_flat,
    int rois_count,
    int node_count);
