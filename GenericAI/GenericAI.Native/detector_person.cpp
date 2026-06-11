#include "pch.h"
#include "detector_person.h"
#include "gai_config.h"
#include "timing_recorder.h"

#include <onnxruntime_cxx_api.h>

// Forward-declare the DirectML EP entry point instead of including
// <dml_provider_factory.h>, which pulls in <d3d12.h> + <DirectML.h> and
// requires the Windows 10 SDK. The project targets Win 8.1 SDK, but the
// runtime check still works: when DirectML.dll is unavailable, the call
// returns a non-null OrtStatus and we fall back to CPU.
extern "C" {
ORT_API_STATUS(OrtSessionOptionsAppendExecutionProvider_DML,
               _In_ OrtSessionOptions* options, int device_id);
}

// pch.h pulls in Windows.h which defines min/max as macros and breaks
// std::min/std::max at call sites. Drop the macros for this TU.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>

// GridStride/Letterbox/ScoredBox and the pure geometry helpers
// (GenerateGridStrides/ComputeLetterbox/Iou/Nms) live in yolox_geometry.h so
// the unit tests can share them.
#include "yolox_geometry.h"

namespace {

std::wstring Widen(const std::string& s) {
    // Proper ANSI-codepage conversion: the naive char-by-char widening only
    // works for ASCII and would mangle a model path containing CJK characters.
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring(s.begin(), s.end());
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()),
                        &w[0], n);
    return w;
}

inline unsigned char ClampU8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<unsigned char>(v);
}

inline void YuvToRgb(int Y, int U, int V,
                     unsigned char& R, unsigned char& G, unsigned char& B) {
    // BT.601 limited-range (typical for surveillance YUV420).
    const int c = Y;
    const int d = U - 128;
    const int e = V - 128;
    R = ClampU8(c + ((359 * e) >> 8));            // 1.402
    G = ClampU8(c - ((88 * d + 183 * e) >> 8));   // -0.344, -0.714
    B = ClampU8(c + ((454 * d) >> 8));            // 1.772
}

// Preprocess I420 planar YUV -> letterboxed RGB CHW float32 (no normalize).
// All row-invariant work (sy / uvy / plane row pointers) is hoisted out of
// the inner loop, and the source-x per content column comes from sx_lut —
// caller-provided scratch of >= input_w ints, filled here once per frame.
// Output is bit-identical to the per-pixel formulation (same expressions,
// same clamp order); this throughput matters because PreLoop's CPU time
// gates the pipelined fps.
void PreprocessYoloxI420(const unsigned char* yuv, int orig_w, int orig_h,
                         int input_w, int input_h,
                         const Letterbox& lb,
                         int* sx_lut,
                         float* out_chw) {
    const unsigned char* Yp = yuv;
    const int uv_w = orig_w / 2;
    const int uv_h = orig_h / 2;
    const unsigned char* Up = yuv + orig_w * orig_h;
    const unsigned char* Vp = Up + uv_w * uv_h;

    const float inv_scale = 1.0f / lb.scale;
    const int chw_stride = input_h * input_w;
    const int orig_w_minus_1 = orig_w - 1;
    const int orig_h_minus_1 = orig_h - 1;
    const int uv_w_minus_1 = uv_w - 1;
    const int uv_h_minus_1 = uv_h - 1;

    // Content columns [x_begin, x_end) in output space; everything outside
    // is letterbox padding (114).
    int x_begin = lb.pad_x;
    if (x_begin < 0) x_begin = 0;
    int x_end = lb.pad_x + lb.new_w;
    if (x_end > input_w) x_end = input_w;

    for (int x = x_begin; x < x_end; ++x) {
        int sx = static_cast<int>((x - lb.pad_x) * inv_scale);
        if (sx > orig_w_minus_1) sx = orig_w_minus_1;
        sx_lut[x] = sx;
    }

    for (int y = 0; y < input_h; ++y) {
        float* rowR = out_chw + y * input_w;
        float* rowG = rowR + chw_stride;
        float* rowB = rowG + chw_stride;

        const int ry = y - lb.pad_y;
        if (ry < 0 || ry >= lb.new_h) {
            for (int x = 0; x < input_w; ++x) {
                rowR[x] = 114.f; rowG[x] = 114.f; rowB[x] = 114.f;
            }
            continue;
        }

        int sy = static_cast<int>(ry * inv_scale);
        if (sy > orig_h_minus_1) sy = orig_h_minus_1;
        int uvy = sy >> 1;
        if (uvy > uv_h_minus_1) uvy = uv_h_minus_1;
        const unsigned char* Yrow = Yp + sy * orig_w;
        const unsigned char* Urow = Up + uvy * uv_w;
        const unsigned char* Vrow = Vp + uvy * uv_w;

        for (int x = 0; x < x_begin; ++x) {
            rowR[x] = 114.f; rowG[x] = 114.f; rowB[x] = 114.f;
        }
        for (int x = x_begin; x < x_end; ++x) {
            const int sx = sx_lut[x];
            int uvx = sx >> 1;
            if (uvx > uv_w_minus_1) uvx = uv_w_minus_1;
            unsigned char r, g, b;
            YuvToRgb(Yrow[sx], Urow[uvx], Vrow[uvx], r, g, b);
            rowR[x] = static_cast<float>(r);
            rowG[x] = static_cast<float>(g);
            rowB[x] = static_cast<float>(b);
        }
        for (int x = x_end; x < input_w; ++x) {
            rowR[x] = 114.f; rowG[x] = 114.f; rowB[x] = 114.f;
        }
    }
}

// BoxOverlapsRoi moved to roi_geometry.h so it can be shared with other
// detectors and adapters.

// ----- Execution-provider helpers ----------------------------------------
// DirectML EP needs mem-pattern off and sequential execution mode (per ORT docs).
Ort::SessionOptions MakeDmlOptions() {
    Ort::SessionOptions opts;
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    opts.DisableMemPattern();
    opts.SetExecutionMode(ORT_SEQUENTIAL);
    // One exe = one channel = one session. Cap inter-op so we don't
    // over-subscribe the box when spark.recorder spawns many of us in parallel.
    opts.SetInterOpNumThreads(1);
    // GPU does the compute; the CPU intra-op pool would otherwise spin up one
    // thread per core per process and just sit idle. Pin to 1 to keep the
    // per-process thread-stack footprint flat when many of us run in parallel.
    opts.SetIntraOpNumThreads(1);
    return opts;
}

Ort::SessionOptions MakeCpuOptions(int intra_threads) {
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(intra_threads);
    opts.SetInterOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    return opts;
}

// Try to append the DirectML EP. Returns false (and fills err) if the
// provider is not available — typically when no DX12 adapter is present
// or DirectML.dll fails to load (Win 8.1, headless VM, etc.).
bool TryAppendDml(Ort::SessionOptions& opts, int device_id, std::string& err) {
    try {
        OrtStatus* status =
            OrtSessionOptionsAppendExecutionProvider_DML(opts, device_id);
        if (status != nullptr) {
            const char* msg = Ort::GetApi().GetErrorMessage(status);
            err = msg ? msg : "(no message)";
            Ort::GetApi().ReleaseStatus(status);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    } catch (...) {
        err = "unknown error";
        return false;
    }
}

}  // namespace

struct PersonDetector::Impl {
    Ort::Env env;
    Ort::Session session;
    Ort::AllocatorWithDefaultOptions allocator;
    // Shared by Warmup and every GpuFrame call; describes the same CPU arena
    // each time, so build it once instead of per inference.
    Ort::MemoryInfo mem_info{nullptr};

    int input_h = 0;
    int input_w = 0;
    int num_classes = 0;

    float nms_iou;
    int target_class;

    int stride_n = 1;

    std::string input_name;
    std::vector<std::string> output_names;
    // c_str() cache for output_names, rebuilt at the end of LoadMetadata so we
    // don't allocate a fresh vector<const char*> on every Detect call.
    std::vector<const char*> output_names_c;

    std::vector<GridStride> grid_strides;   // YOLOX only

    std::string backend_label = "CPU";   // "DirectML(<id>)" or "CPU"
    static constexpr int kCpuIntraThreads = 8;

    bool try_gpu = true;

    // ----- Process-wide pipeline pool (route B) ----------------------------
    // K=2 in-flight slots so PreLoop can fill slot A while GpuLoop reads
    // slot B. Each slot owns its preprocessed input tensor plus the ORT
    // outputs that live across Phase2→Phase3 on the GpuLoop thread.
    struct InFlight {
        std::vector<float> input_buffer;        // 3 * input_h * input_w floats
        std::vector<int> sx_lut;                // input_w ints, scratch for PreprocessYoloxI420
        std::vector<Ort::Value> gpu_outputs;    // owned across Phase2→Phase3
        Letterbox lb{};
        float conf_threshold = 0.0f;
        int orig_w = 0;
        int orig_h = 0;
    };
    static constexpr int kPoolSize = 2;
    std::array<InFlight, kPoolSize> in_flights{};
    std::array<bool, kPoolSize> in_use{};
    std::mutex pool_mtx;
    std::condition_variable pool_cv;
    bool pool_closed = false;

    // Blocks until a slot is free or the pool is closed.
    // Returns the slot index (0..kPoolSize-1), or -1 if shut down.
    int AcquireSlotBlocking() {
        std::unique_lock<std::mutex> lk(pool_mtx);
        pool_cv.wait(lk, [&] {
            if (pool_closed) return true;
            for (int i = 0; i < kPoolSize; ++i) if (!in_use[i]) return true;
            return false;
        });
        if (pool_closed) return -1;
        for (int i = 0; i < kPoolSize; ++i) {
            if (!in_use[i]) {
                in_use[i] = true;
                return i;
            }
        }
        return -1;  // unreachable: predicate guarantees a free slot
    }

    void ReleaseSlot(int idx) {
        if (idx < 0 || idx >= kPoolSize) return;
        {
            std::lock_guard<std::mutex> lk(pool_mtx);
            in_use[idx] = false;
        }
        pool_cv.notify_one();
    }

    void ClosePool() {
        {
            std::lock_guard<std::mutex> lk(pool_mtx);
            pool_closed = true;
        }
        pool_cv.notify_all();
    }

    Impl(const std::string& model_path, float iou, int cls, bool try_gpu_arg)
        : env(ORT_LOGGING_LEVEL_WARNING, "PersonDetector"),
          session(nullptr),
          mem_info(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)),
          nms_iou(iou),
          target_class(cls)
    {
        try_gpu = try_gpu_arg;
        const std::wstring wpath = Widen(model_path);
        BuildSessionWithFallback(wpath);
        LoadMetadata(model_path);

        // Pre-size every pool slot's input tensor so Phase1Prepare never
        // allocates on the inference hot path. Dims are stable across the
        // optional DML→CPU fallback (same model file, same input layout).
        const size_t buf_size = static_cast<size_t>(3) * input_h * input_w;
        for (auto& s : in_flights) {
            s.input_buffer.assign(buf_size, 0.f);
            s.sx_lut.assign(static_cast<size_t>(input_w), 0);
        }

        if (gai::kEnableTimingLog) {
            std::cout << "[AI] Person detector loaded: " << model_path
                      << " (input=" << input_w << "x" << input_h
                      << ", EP=" << backend_label << ")" << std::endl;
        }

        try {
            Warmup();
        } catch (const Ort::Exception& e) {
            if (backend_label != "CPU") {
                std::cerr << "[AI] DirectML warmup failed: " << e.what()
                          << ". Rebuilding session on CPU." << std::endl;
                Ort::SessionOptions cpu_opts = MakeCpuOptions(kCpuIntraThreads);
                session = Ort::Session(env, wpath.c_str(), cpu_opts);
                backend_label = "CPU";
                LoadMetadata(model_path);
                if (gai::kEnableTimingLog) {
                    std::cout << "[AI] Person detector now running on EP=CPU." << std::endl;
                }
                Warmup();
            } else {
                throw;
            }
        }
    }

    void BuildSessionWithFallback(const std::wstring& wpath) {
        Ort::SessionOptions dml_opts = MakeDmlOptions();
        std::string dml_err;
        if (try_gpu && TryAppendDml(dml_opts, 0, dml_err)) {
            try {
                session = Ort::Session(env, wpath.c_str(), dml_opts);
                backend_label = "DirectML(0)";
                return;
            } catch (const Ort::Exception& e) {
                std::cerr << "[AI] DirectML session build failed: " << e.what()
                          << ". Falling back to CPU." << std::endl;
            }
        } else {
            std::cerr << "[AI] DirectML EP unavailable: " << dml_err
                      << ". Using CPU." << std::endl;
        }
        Ort::SessionOptions cpu_opts = MakeCpuOptions(kCpuIntraThreads);
        session = Ort::Session(env, wpath.c_str(), cpu_opts);
        backend_label = "CPU";
    }

    void LoadMetadata(const std::string& model_path) {
        const size_t num_inputs = session.GetInputCount();
        if (num_inputs != 1) {
            throw std::runtime_error("Expected 1 input tensor, got " +
                                     std::to_string(num_inputs));
        }
        {
            Ort::AllocatedStringPtr iname = session.GetInputNameAllocated(0, allocator);
            input_name = iname.get();
        }
        auto in_shape = session.GetInputTypeInfo(0)
                               .GetTensorTypeAndShapeInfo().GetShape();
        if (in_shape.size() != 4) {
            throw std::runtime_error("Expected 4D input [1,3,H,W], got rank " +
                                     std::to_string(in_shape.size()));
        }
        input_h = static_cast<int>(in_shape[2]);
        input_w = static_cast<int>(in_shape[3]);
        if (input_h <= 0 || input_w <= 0) {
            throw std::runtime_error("Dynamic input shape not supported "
                                     "(H,W must be static positive integers)");
        }

        const size_t num_outputs = session.GetOutputCount();
        output_names.clear();
        output_names.reserve(num_outputs);
        std::vector<std::vector<int64_t>> out_shapes;
        out_shapes.reserve(num_outputs);
        for (size_t i = 0; i < num_outputs; ++i) {
            Ort::AllocatedStringPtr oname = session.GetOutputNameAllocated(i, allocator);
            output_names.emplace_back(oname.get());
            out_shapes.push_back(session.GetOutputTypeInfo(i)
                                        .GetTensorTypeAndShapeInfo().GetShape());
        }

        auto format_shape = [&]() {
            std::ostringstream os;
            for (size_t i = 0; i < out_shapes.size(); ++i) {
                os << " [";
                for (size_t d = 0; d < out_shapes[i].size(); ++d) {
                    if (d) os << ",";
                    os << out_shapes[i][d];
                }
                os << "]";
            }
            return os.str();
        };

        if (num_outputs != 1 || out_shapes[0].size() != 3 ||
            out_shapes[0][2] <= 5 || out_shapes[0][2] == 4) {
            throw std::runtime_error(
                "Expected YOLOX-style output [1, N, 5+num_classes], got " +
                std::to_string(num_outputs) + " outputs:" + format_shape());
        }

        num_classes = static_cast<int>(out_shapes[0][2]) - 5;
        if (num_classes <= target_class) {
            throw std::runtime_error(
                "Target class " + std::to_string(target_class) +
                " out of range (model num_classes=" +
                std::to_string(num_classes) + ")");
        }
        grid_strides.clear();
        GenerateGridStrides(input_w, input_h, grid_strides);
        const int expected_anchors = static_cast<int>(out_shapes[0][1]);
        if (static_cast<int>(grid_strides.size()) != expected_anchors) {
            throw std::runtime_error(
                "YOLOX grid mismatch: model anchors=" +
                std::to_string(expected_anchors) +
                " vs derived=" + std::to_string(grid_strides.size()));
        }

        // Rebuild the c_str cache here so it stays valid across a session
        // rebuild (DirectML→CPU fallback re-enters LoadMetadata).
        output_names_c.clear();
        output_names_c.reserve(output_names.size());
        for (auto& s : output_names) output_names_c.push_back(s.c_str());
    }

    void Warmup() {
        std::vector<float> tmp(static_cast<size_t>(3) * input_h * input_w, 0.f);
        const int64_t shape[4] = { 1, 3, input_h, input_w };
        Ort::Value input = Ort::Value::CreateTensor<float>(
            mem_info, tmp.data(), tmp.size(), shape, 4);

        const char* in_names_c[1] = { input_name.c_str() };
        session.Run(Ort::RunOptions{nullptr},
                    in_names_c, &input, 1,
                    output_names_c.data(), output_names_c.size());
    }

    // ----- Inference pipeline ------------------------------------------------
    // Each frame goes through 3 stages: PreFrame (CPU YUV→tensor) → GpuFrame
    // (ONNX session.Run) → PostFrame (decode + NMS). Single-shot Detect()
    // runs all 3 inline on the InferLoop thread. Pipelined PreLoop runs
    // PreFrame; GpuLoop runs GpuFrame + PostFrame. Either way the per-frame
    // working data lives in an InFlight pool slot, indexed by an int and
    // owned by Impl::in_flights[].

    void PreFrame(InFlight& slot,
                  const unsigned char* yuv, int orig_w, int orig_h,
                  float conf_threshold) {
        slot.orig_w = orig_w;
        slot.orig_h = orig_h;
        slot.conf_threshold = conf_threshold;
        slot.lb = ComputeLetterbox(orig_w, orig_h, input_w, input_h);
        PreprocessYoloxI420(yuv, orig_w, orig_h, input_w, input_h,
                            slot.lb, slot.sx_lut.data(), slot.input_buffer.data());
        if (gai::kEnableTimingLog) gai::TimingRecorder::Instance().MarkDetectPreDone();
    }

    void GpuFrame(InFlight& slot) {
        const int64_t in_shape[4] = { 1, 3, input_h, input_w };
        Ort::Value input = Ort::Value::CreateTensor<float>(
            mem_info, slot.input_buffer.data(), slot.input_buffer.size(), in_shape, 4);
        const char* in_names_c[1] = { input_name.c_str() };
        slot.gpu_outputs = session.Run(Ort::RunOptions{nullptr},
                                       in_names_c, &input, 1,
                                       output_names_c.data(), output_names_c.size());
        if (gai::kEnableTimingLog) gai::TimingRecorder::Instance().MarkDetectGpuDone();
    }

    void PostFrame(InFlight& slot, std::vector<DetectionRect>& out_boxes) {
        PostYolox(slot, out_boxes);
        // Free ORT-owned output tensors before the slot returns to the pool.
        slot.gpu_outputs.clear();
    }

    void PostYolox(InFlight& slot, std::vector<DetectionRect>& out_boxes) {
        const float* out_data = slot.gpu_outputs[0].GetTensorData<float>();
        const int channels = num_classes + 5;

        std::vector<ScoredBox> proposals;
        proposals.reserve(64);
        const float inv_scale = 1.0f / slot.lb.scale;
        const int num_anchors = static_cast<int>(grid_strides.size());

        for (int a = 0; a < num_anchors; ++a) {
            const float* pred = out_data + a * channels;
            const float obj = pred[4];
            const float cls = pred[5 + target_class];
            const float score = obj * cls;
            if (score < slot.conf_threshold) continue;

            const GridStride& gs = grid_strides[a];
            const float cx = (pred[0] + gs.grid_x) * gs.stride;
            const float cy = (pred[1] + gs.grid_y) * gs.stride;
            const float w  = std::exp(pred[2]) * gs.stride;
            const float h  = std::exp(pred[3]) * gs.stride;

            float x0 = (cx - w * 0.5f - slot.lb.pad_x) * inv_scale;
            float y0 = (cy - h * 0.5f - slot.lb.pad_y) * inv_scale;
            float ww = w * inv_scale;
            float hh = h * inv_scale;

            if (x0 < 0) { ww += x0; x0 = 0; }
            if (y0 < 0) { hh += y0; y0 = 0; }
            if (x0 + ww > slot.orig_w) ww = slot.orig_w - x0;
            if (y0 + hh > slot.orig_h) hh = slot.orig_h - y0;
            if (ww <= 1.f || hh <= 1.f) continue;

            ScoredBox sb;
            sb.box.x = static_cast<int>(x0);
            sb.box.y = static_cast<int>(y0);
            sb.box.w = static_cast<int>(ww);
            sb.box.h = static_cast<int>(hh);
            sb.score = score;
            proposals.push_back(sb);
        }

        Nms(proposals, nms_iou, out_boxes);
    }

    float ComputeConfThreshold(const gai::DetectorParams& params) const {
        int ti = params.threshold; if (ti < 0) ti = 0; else if (ti > 100) ti = 100;
        return 0.20f + (ti / 100.f) * 0.50f;
    }

    // Applies the bbox→ROI overlap filter; populates ctx.last_detections (kept
    // boxes) and detected_roi_indices (which ROIs were hit). Sole toucher of
    // ctx.last_detections — including the clear below: in the pipelined route
    // Phase1Prepare (PreLoop thread) must not clear it, because Phase3Post
    // (GpuLoop thread) may be writing the previous frame of the same channel
    // concurrently.
    void ApplyRoiFilter(PersonDetectorContext& ctx,
                        std::vector<DetectionRect>& raw,
                        const ROIRect* roi_rects, int roi_count,
                        std::vector<int>& detected_roi_indices) {
        ctx.last_detections.clear();
        if (roi_count <= 0 || roi_rects == nullptr) {
            ctx.last_detections = std::move(raw);
        } else {
            std::vector<char> roi_hit(roi_count, 0);
            for (const auto& b : raw) {
                bool keep = false;
                for (int r = 0; r < roi_count; ++r) {
                    if (BoxOverlapsRoi(b, roi_rects[r])) {
                        roi_hit[r] = 1;
                        keep = true;
                    }
                }
                if (keep) ctx.last_detections.push_back(b);
            }
            for (int r = 0; r < roi_count; ++r) {
                if (roi_hit[r]) detected_roi_indices.push_back(r);
            }
        }
    }

};

PersonDetector::PersonDetector(const std::string& model_path,
                               float nms_iou,
                               int target_class,
                               bool try_gpu)
    : m_impl(new Impl(model_path, nms_iou, target_class, try_gpu))
{
}

PersonDetector::~PersonDetector() = default;

std::string PersonDetector::GetBackendLabel() const {
    return m_impl ? m_impl->backend_label : std::string();
}

std::unique_ptr<gai::DetectorContext> PersonDetector::CreateContext() {
    if (m_impl->input_h <= 0 || m_impl->input_w <= 0) {
        throw std::runtime_error("PersonDetector::CreateContext: model dims not initialized");
    }
    // input_buffer no longer lives in ctx — it moved to Impl::in_flights[]
    // (the pipelined pool). ctx only carries cross-frame per-channel state.
    return std::unique_ptr<PersonDetectorContext>(new PersonDetectorContext());
}

int PersonDetector::Detect(PersonDetectorContext& ctx,
                           const unsigned char* yuv420_frame, int width, int height,
                           const ROIRect* roi_rects, int roi_count,
                           std::vector<int>& detected_roi_indices,
                           const gai::DetectorParams& params) {
    // Single-shot path. Runs PreFrame → GpuFrame → PostFrame on one acquired
    // pool slot. The pipelined PreLoop/GpuLoop split the same 3 calls across
    // two threads via Phase1Prepare / Phase2Gpu / Phase3Post; behaviour is
    // identical, only the threading differs.
    detected_roi_indices.clear();
    ctx.last_detections.clear();

    if (yuv420_frame == nullptr || width <= 0 || height <= 0) return 0;

    // Frame decimation: run inference once per stride_n frames.
    if (++ctx.stride_counter < m_impl->stride_n) return 0;
    ctx.stride_counter = 0;

    const float conf_threshold = m_impl->ComputeConfThreshold(params);

    const int slot_idx = m_impl->AcquireSlotBlocking();
    if (slot_idx < 0) return 0;   // pool closed (shutdown)
    auto& slot = m_impl->in_flights[slot_idx];

    std::vector<DetectionRect> raw;
    try {
        m_impl->PreFrame(slot, yuv420_frame, width, height, conf_threshold);
        m_impl->GpuFrame(slot);
        m_impl->PostFrame(slot, raw);
    } catch (const Ort::Exception& e) {
        std::cerr << "[AI] ORT inference failed: " << e.what() << std::endl;
        m_impl->ReleaseSlot(slot_idx);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[AI] Inference failed: " << e.what() << std::endl;
        m_impl->ReleaseSlot(slot_idx);
        return 0;
    }
    m_impl->ReleaseSlot(slot_idx);

    m_impl->ApplyRoiFilter(ctx, raw, roi_rects, roi_count, detected_roi_indices);

    if (gai::kEnableTimingLog) {
        if (!ctx.last_detections.empty()) {
            std::cout << "[PersonDetector] " << static_cast<int>(ctx.last_detections.size())
                      << " person detection(s) across " << detected_roi_indices.size() << " ROI(s)" << std::endl;
        }
    }

    return static_cast<int>(ctx.last_detections.size());
}

int PersonDetector::Phase1Prepare(PersonDetectorContext& ctx,
                                  const unsigned char* yuv420_frame, int width, int height,
                                  const gai::DetectorParams& params) {
    // Mirrors Detect's preamble: validate inputs, honour stride decimation.
    // Returns negative codes for shortcuts so the caller (PreLoop) can route
    // the queue item correctly. ctx.last_detections is NOT cleared here —
    // this runs on the PreLoop thread while Phase3Post (GpuLoop thread) may
    // still be writing it for the previous frame of the same channel;
    // ApplyRoiFilter clears it on the consumer side instead.
    if (yuv420_frame == nullptr || width <= 0 || height <= 0) return -1;

    if (++ctx.stride_counter < m_impl->stride_n) return -1;
    ctx.stride_counter = 0;

    const float conf_threshold = m_impl->ComputeConfThreshold(params);
    const int slot_idx = m_impl->AcquireSlotBlocking();
    if (slot_idx < 0) return -2;   // pool closed mid-acquire (shutdown)

    auto& slot = m_impl->in_flights[slot_idx];
    try {
        m_impl->PreFrame(slot, yuv420_frame, width, height, conf_threshold);
    } catch (const std::exception& e) {
        std::cerr << "[AI] Preprocess failed: " << e.what() << std::endl;
        m_impl->ReleaseSlot(slot_idx);
        return -1;
    }
    return slot_idx;
}

void PersonDetector::Phase2Gpu(int detector_slot) {
    if (detector_slot < 0 || detector_slot >= PersonDetector::Impl::kPoolSize) return;
    auto& slot = m_impl->in_flights[detector_slot];
    // Caller (GpuLoop) traps exceptions; let Ort::Exception / std::exception
    // propagate so the scheduler can CommitError and release the slot via
    // Phase3Post not being called — but Phase2 cannot know to release the
    // slot itself, so we release on the exception path and rethrow.
    try {
        m_impl->GpuFrame(slot);
    } catch (...) {
        // Free GPU outputs (likely empty) before yielding the slot so the
        // next frame can claim it; rethrow so scheduler routes the error.
        slot.gpu_outputs.clear();
        m_impl->ReleaseSlot(detector_slot);
        throw;
    }
}

int PersonDetector::Phase3Post(PersonDetectorContext& ctx, int detector_slot,
                               const ROIRect* roi_rects, int roi_count,
                               std::vector<int>& detected_roi_indices) {
    if (detector_slot < 0 || detector_slot >= PersonDetector::Impl::kPoolSize) return 0;
    auto& slot = m_impl->in_flights[detector_slot];
    std::vector<DetectionRect> raw;
    try {
        m_impl->PostFrame(slot, raw);
    } catch (const std::exception& e) {
        std::cerr << "[AI] Postprocess failed: " << e.what() << std::endl;
        slot.gpu_outputs.clear();
        m_impl->ReleaseSlot(detector_slot);
        return 0;
    }
    m_impl->ReleaseSlot(detector_slot);

    m_impl->ApplyRoiFilter(ctx, raw, roi_rects, roi_count, detected_roi_indices);

    if (gai::kEnableTimingLog) {
        if (!ctx.last_detections.empty()) {
            std::cout << "[PersonDetector] " << static_cast<int>(ctx.last_detections.size())
                      << " person detection(s) across " << detected_roi_indices.size() << " ROI(s)" << std::endl;
        }
    }
    return static_cast<int>(ctx.last_detections.size());
}

void PersonDetector::ClosePipelinedPool() {
    if (m_impl) m_impl->ClosePool();
}
