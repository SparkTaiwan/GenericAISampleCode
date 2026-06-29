#pragma once

#include "gai_abi.h"
#include "roi_geometry.h"
#include "class_table.h"

#include <memory>
#include <string>
#include <vector>

namespace gai {

// Per-detection output for a single Detect() call. Populated by the IDetector
// adapter so the caller (InferThread / DispatchThread) needs no detector-kind
// knowledge to feed the C ABI callback.
struct DetectionResult {
    std::vector<GAI_Roi> flattened_points;
    int rois_count = 0;
    int node_count = 0;
    // Per supported-class detection count (class_table.h order); fed to the C ABI
    // callback so the host can emit "<class>___Count" items.
    int class_counts[kNumSupportedClasses] = { 0 };
};

enum class DetectorKind : int {
    Motion = 0,
    Person = 1,
};

// Per-channel detector state. Concrete subclass is created by the detector
// (CreateContext); ChannelPipeline owns one per channel and feeds it back into
// every Detect call. This is how the shared detector stays stateless w.r.t.
// individual channels.
class DetectorContext {
public:
    virtual ~DetectorContext() = default;
};

// Per-call detector tunables. Pulled from ParamSnapshot::View on the inference
// thread and passed in alongside the frame. image_width/height carry the
// /SetParameters reference resolution the ROI coordinates were authored in;
// 0 means unknown (legacy caller) and disables ROI rescaling.
struct DetectorParams {
    int threshold = 0;
    int sensitivity = 0;
    int image_width = 0;
    int image_height = 0;
    // Per-channel ai_settings value (spec §5): confidence threshold 0..1, supplied
    // via GAI_SetChannelAiSettings. -1 = unset -> detector falls back to the
    // threshold-derived value, so legacy channels are unaffected.
    float confidence = -1.0f;
    // Bitmask over class_table.h supported-class index (bit i => kSupportedClasses[i]
    // enabled). -1 = unset -> detector reports all supported classes.
    int class_mask = -1;
};

// Abstract detector. Implementations must be stateless w.r.t. channels —
// any cross-frame state lives in DetectorContext. Detect() is invoked from a
// single thread (the shared InferLoop); ctx is the calling channel's context.
class IDetector {
public:
    virtual ~IDetector() = default;

    virtual std::unique_ptr<DetectorContext> CreateContext() = 0;

    virtual int Detect(DetectorContext* ctx,
                       const unsigned char* yuv420,
                       int width, int height,
                       const ROIRect* roi_rects, int roi_count,
                       const std::vector<std::vector<GAI_Roi>>& original_roi_points,
                       const DetectorParams& params,
                       DetectionResult& out_result) = 0;

    virtual const char* Name() const = 0;

    // "CPU" / "DirectML(0)" etc. Motion is always CPU; Person returns based on the actual loaded EP.
    virtual std::string Backend() const { return "CPU"; }

    // Pipelined inference (route B). When HasPipelined() returns true the
    // scheduler splits InferLoop into PreLoop (CPU preprocess) and GpuLoop
    // (session.Run + CPU postprocess), overlapping CPU work across frames
    // with GPU inference. Default = false (Motion path); PersonAdapter overrides.
    virtual bool HasPipelined() const { return false; }

    // Phase 1 (PreLoop): CPU preprocess into the detector-owned input_buffer
    // pool. Returns a pool slot index (>=0) the caller must carry to Phase2 /
    // Phase3, or -1 for stride-skipped frames (scheduler still pushes the
    // queue item so per-channel FIFO is preserved; GpuLoop CommitEmpty's it),
    // or -2 when ClosePipelinedPool() unblocked an Acquire (shutdown).
    virtual int Phase1Prepare(DetectorContext* /*ctx*/,
                              const unsigned char* /*yuv420*/,
                              int /*width*/, int /*height*/,
                              const ROIRect* /*roi_rects*/, int /*roi_count*/,
                              const std::vector<std::vector<GAI_Roi>>& /*original_roi_points*/,
                              const DetectorParams& /*params*/) { return -1; }

    // Phase 2 (GpuLoop): session.Run() against the slot's input_buffer.
    virtual void Phase2Gpu(DetectorContext* /*ctx*/, int /*detector_slot*/) {}

    // Phase 3 (GpuLoop): decode + NMS + ROI filter; fills out_result; the
    // detector implementation releases the pool slot before returning.
    virtual int Phase3Post(DetectorContext* /*ctx*/, int /*detector_slot*/,
                           const ROIRect* /*roi_rects*/, int /*roi_count*/,
                           const std::vector<std::vector<GAI_Roi>>& /*original_roi_points*/,
                           const DetectorParams& /*params*/,
                           DetectionResult& /*out_result*/) { return 0; }

    // Shutdown hook: wakes any thread blocked inside Phase1's slot acquire so
    // PreLoop can exit. Scheduler::Stop() calls this before joining PreLoop.
    virtual void ClosePipelinedPool() {}
};

}  // namespace gai
