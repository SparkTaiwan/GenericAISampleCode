#include "pch.h"
#include "detector_factory.h"
#include "detector_motion.h"
#include "detector_person.h"
#include "gai_config.h"

#include <exception>
#include <iostream>
#include <utility>

namespace gai {

namespace {

class MotionAdapter : public IDetector {
public:
    MotionAdapter() = default;

    std::unique_ptr<DetectorContext> CreateContext() override {
        return inner_.CreateContext();
    }

    int Detect(DetectorContext* ctx,
               const unsigned char* yuv, int width, int height,
               const ROIRect* roi_rects, int roi_count,
               const std::vector<std::vector<GAI_Roi>>& original_roi_points,
               const DetectorParams& params,
               DetectionResult& out) override {
        if (!ctx) return 0;
        std::vector<int> indices;
        auto* mctx = static_cast<MotionDetectorContext*>(ctx);
        int n = inner_.Detect(*mctx, yuv, width, height, roi_rects, roi_count, indices, params);
        if (n <= 0) return n;

        out.flattened_points.clear();
        for (int idx : indices) {
            if (idx >= 0 && static_cast<std::size_t>(idx) < original_roi_points.size()) {
                for (const auto& pt : original_roi_points[idx]) {
                    out.flattened_points.push_back(pt);
                }
            }
        }
        out.rois_count = n;
        out.node_count = (n > 0) ? static_cast<int>(out.flattened_points.size()) / n : 0;
        return n;
    }

    const char* Name() const override { return "Motion"; }

private:
    MotionDetector inner_;
};

class PersonAdapter : public IDetector {
public:
    explicit PersonAdapter(const std::string& model_path)
        : inner_(model_path, 0.5f, 0, kPreferGpu) {}

    std::unique_ptr<DetectorContext> CreateContext() override {
        return inner_.CreateContext();
    }

    int Detect(DetectorContext* ctx,
               const unsigned char* yuv, int width, int height,
               const ROIRect* roi_rects, int roi_count,
               const std::vector<std::vector<GAI_Roi>>& original_roi_points,
               const DetectorParams& params,
               DetectionResult& out) override {
        (void)original_roi_points;
        if (!ctx) return 0;
        std::vector<int> indices;
        auto* pctx = static_cast<PersonDetectorContext*>(ctx);
        int n = inner_.Detect(*pctx, yuv, width, height, roi_rects, roi_count, indices, params);
        if (n <= 0) return n;
        FlattenBoxesToQuads(pctx->last_detections, out);
        for (int i = 0; i < kNumSupportedClasses; ++i) out.class_counts[i] = pctx->last_class_counts[i];
        return n;
    }

    const char* Name() const override { return "Person"; }
    std::string Backend() const override { return inner_.GetBackendLabel(); }

    bool HasPipelined() const override { return true; }

    int Phase1Prepare(DetectorContext* ctx,
                      const unsigned char* yuv, int width, int height,
                      const ROIRect* /*roi_rects*/, int /*roi_count*/,
                      const std::vector<std::vector<GAI_Roi>>& /*original_roi_points*/,
                      const DetectorParams& params) override {
        if (!ctx) return -1;
        auto* pctx = static_cast<PersonDetectorContext*>(ctx);
        return inner_.Phase1Prepare(*pctx, yuv, width, height, params);
    }

    void Phase2Gpu(DetectorContext* ctx, int detector_slot) override {
        (void)ctx;
        inner_.Phase2Gpu(detector_slot);
    }

    int Phase3Post(DetectorContext* ctx, int detector_slot,
                   const ROIRect* roi_rects, int roi_count,
                   const std::vector<std::vector<GAI_Roi>>& /*original_roi_points*/,
                   const DetectorParams& /*params*/,
                   DetectionResult& out) override {
        if (!ctx) return 0;
        std::vector<int> indices;
        auto* pctx = static_cast<PersonDetectorContext*>(ctx);
        int n = inner_.Phase3Post(*pctx, detector_slot, roi_rects, roi_count, indices);
        if (n <= 0) return n;
        FlattenBoxesToQuads(pctx->last_detections, out);
        for (int i = 0; i < kNumSupportedClasses; ++i) out.class_counts[i] = pctx->last_class_counts[i];
        return n;
    }

    void ClosePipelinedPool() override { inner_.ClosePipelinedPool(); }

private:
    static void FlattenBoxesToQuads(const std::vector<DetectionRect>& bboxes,
                                    DetectionResult& out) {
        out.flattened_points.clear();
        out.flattened_points.reserve(bboxes.size() * 4);
        for (const auto& b : bboxes) {
            GAI_Roi tl{ b.x,             b.y };
            GAI_Roi tr{ b.x + b.w,       b.y };
            GAI_Roi br{ b.x + b.w,       b.y + b.h };
            GAI_Roi bl{ b.x,             b.y + b.h };
            out.flattened_points.push_back(tl);
            out.flattened_points.push_back(tr);
            out.flattened_points.push_back(br);
            out.flattened_points.push_back(bl);
        }
        out.rois_count = static_cast<int>(bboxes.size());
        out.node_count = 4;
    }

    PersonDetector inner_;
};

}  // namespace

std::unique_ptr<IDetector> CreateDetector(DetectorKind kind, const std::string& model_path, std::string& err) {
    err.clear();
    try {
        switch (kind) {
            case DetectorKind::Motion:
                return std::unique_ptr<IDetector>(new MotionAdapter());
            case DetectorKind::Person:
                if (model_path.empty()) {
                    err = "Person/object detector requires a model path";
                    std::cerr << "[GAI] " << err << std::endl;
                    return nullptr;
                }
                return std::unique_ptr<IDetector>(new PersonAdapter(model_path));
        }
    } catch (const std::exception& e) {
        err = e.what();
        std::cerr << "[GAI] Detector construction failed: " << err << std::endl;
    } catch (...) {
        err = "Detector construction failed (unknown error)";
        std::cerr << "[GAI] " << err << std::endl;
    }
    return nullptr;
}

}  // namespace gai
