#pragma once

#include "idetector.h"
#include "roi_geometry.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Per-channel state for PersonDetector. Owned by ChannelPipeline; passed back
// into every Detect() / Phase*() call. Defined in the header so the factory/
// adapter can `new` it directly.
//
// input_buffer used to live here but moved to PersonDetector::Impl's
// process-wide InFlight pool (route B) so PreLoop can pipeline against
// GpuLoop. ctx still keeps cross-frame per-channel state (stride counter +
// last detections) because those must not bleed across channels.
//
// Pipelined-route thread ownership: two frames of the SAME channel can be in
// flight at once, so stride_counter is touched only by Phase1Prepare (PreLoop
// thread) and last_detections only by Phase3Post/ApplyRoiFilter (GpuLoop
// thread). Do not clear or read either field from the other thread.
struct PersonDetectorContext : public gai::DetectorContext {
    int stride_counter = 0;
    std::vector<DetectionRect> last_detections;
    // Per-channel ai_settings class filter (class_table.h supported index bitmask;
    // <0 = all). Set on the prepare side (Detect / Phase1Prepare) and read by
    // ApplyRoiFilter on the post side. last_class_counts is the per-supported-class
    // tally of the kept detections, fed to the C ABI callback.
    int class_mask = -1;
    int last_class_counts[gai::kNumSupportedClasses] = { 0 };
    // Channel-wide ai_settings fallbacks, set on the prepare side (Detect /
    // Phase1Prepare) and read by ApplyRoiFilter on the post side (which has no
    // params). A ROI whose per-ROI value is unset (-1) inherits these. frame_w/h
    // are this frame's dimensions, needed to turn object_size % into a pixel area.
    float channel_conf = -1.0f;       // effective channel confidence (0..1)
    float channel_size_min = -1.0f;   // % of frame area (0..100), <0 = no lower limit
    float channel_size_max = -1.0f;   // % of frame area (0..100), <0/>=100 = no upper limit
    int   frame_w = 0;
    int   frame_h = 0;
};

// Person detector backed by ONNX Runtime. Auto-detects YOLOX / YOLOv6 /
// DAMO-YOLO from the model graph. Tries DirectML EP first, falls back to CPU
// EP transparently when DirectML is unavailable.
//
// Stateless w.r.t. channels: stride counter / last detections live in
// PersonDetectorContext; input_buffer / GPU outputs live in a process-wide
// pool of InFlight slots owned by the detector itself. Confidence threshold
// is derived per call from DetectorParams.
class PersonDetector {
public:
    PersonDetector(const std::string& model_path,
                   float nms_iou = 0.5f,
                   int target_class = 0,
                   bool try_gpu = true);
    ~PersonDetector();

    std::unique_ptr<gai::DetectorContext> CreateContext();

    // Single-shot path used by single-thread InferLoop and by adapters that
    // bypass the pipelined route. Acquires one pool slot internally and
    // releases it before returning.
    int Detect(PersonDetectorContext& ctx,
               const unsigned char* yuv420_frame, int width, int height,
               const ROIRect* roi_rects, int roi_count,
               const std::vector<std::vector<GAI_Roi>>& original_roi_points,
               std::vector<int>& detected_roi_indices,
               const gai::DetectorParams& params);

    // Pipelined path (route B). Phase1 reserves a pool slot and writes the
    // preprocessed input tensor. Phase2 runs session.Run on that slot.
    // Phase3 decodes + ROI-filters + releases the slot.
    //
    // Phase1Prepare returns:
    //   >= 0  pool slot index (must be carried to Phase2/Phase3)
    //   -1    stride decimation skipped this frame (scheduler should bypass
    //         Phase2/Phase3 and CommitEmpty)
    //   -2    pool was closed (shutdown) before a slot was available
    int Phase1Prepare(PersonDetectorContext& ctx,
                      const unsigned char* yuv420_frame, int width, int height,
                      const ROIRect* roi_rects, int roi_count,
                      const gai::DetectorParams& params);
    void Phase2Gpu(int detector_slot);
    int Phase3Post(PersonDetectorContext& ctx, int detector_slot,
                   const ROIRect* roi_rects, int roi_count,
                   const std::vector<std::vector<GAI_Roi>>& original_roi_points,
                   std::vector<int>& detected_roi_indices);

    // Wakes any thread blocked inside Phase1Prepare's slot acquire so the
    // pipelined PreLoop can exit during shutdown.
    void ClosePipelinedPool();

    // "DirectML(0)" / "CPU" — used to confirm the current EP in the log/console.
    std::string GetBackendLabel() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
