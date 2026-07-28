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
    // Per-ROI object-detection tuning (schema scope=roi). Index aligns with the
    // rois[] groups above. -1 = unset -> the detector inherits the channel-wide
    // ai_settings value (GAI_SetChannelAiSettings), so a caller that fills only the
    // legacy fields keeps today's behaviour. object_size_* are % of frame (0..100).
    // Appended at the END so the leading layout stays byte-compatible.
    float confidence[10];
    int   class_mask[10];
    float object_size_min[10];
    float object_size_max[10];
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
    int node_count,
    // Per supported-class detection count, in class_table.h order; length =
    // class_counts_len. The host emits "<class>___Count" items from these.
    const int* class_counts,
    int class_counts_len);
