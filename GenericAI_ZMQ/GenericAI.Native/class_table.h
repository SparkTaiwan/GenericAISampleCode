#pragma once

namespace gai {

// AI-settings "classes" supported set (spec §5), in a FIXED order. The per-channel
// class_mask bit i, and the class_counts[] index i, both refer to this order; the
// C# side (NativeInterop.SupportedClasses) mirrors it 1:1. coco_id maps a supported
// index to the YOLOX/COCO output channel the model emits (the bundled yolox_m model
// is COCO-80). Aligns with the dongle's current class set.
struct SupportedClass { const char* name; int coco_id; };

static const SupportedClass kSupportedClasses[] = {
    { "person",      0 },
    { "car",         2 },
    { "bus",         5 },
    { "truck",       7 },
    { "motorcycle",  3 },
    { "bicycle",     1 },
    { "cat",        15 },
    { "dog",        16 },
};

static const int kNumSupportedClasses = 8;

}  // namespace gai
