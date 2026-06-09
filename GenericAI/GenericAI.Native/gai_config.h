#pragma once
#include "idetector.h"

// === Detector selection flag ===
//  - DetectorKind::Motion  -> frame-difference motion detection (detector_motion.cpp)
//  - DetectorKind::Person  -> ONNX person detection (detector_person.cpp, requires kDefaultModelPath to exist)
//
// To switch: edit this line -> rebuild -> restart the exe.
// Matches the behavior of kDetectorMode in the old CSharp/SampleDLL/dllmain.cpp:19.
namespace gai {

constexpr DetectorKind kDetectorKind = DetectorKind::Person;

// Relative to the exe working directory; copied from native-deps/models/ by the csproj at build time.
// The old equivalent path is g_modelPath in CSharp/SampleDLL/detectors_person.cpp:72.
constexpr const char* kDefaultModelPath = "models/yolo.onnx";

// === Inference execution device (only meaningful in Person mode) ===
//  - true  -> prefer DirectML (GPU), automatically fall back to CPU on failure
//  - false -> force CPU, never attempt GPU
// Switching steps are the same as kDetectorKind: edit one line -> rebuild -> restart the exe.
// To check the current EP after startup: see the console "[AI] Person detector loaded: ... EP=..."
// or the "detector backend = ..." line in D:\SLog-<port>\GenericAI.log.
constexpr bool kPreferGpu = true;

// === Per-frame timing log (temporary Step 10/11 instrumentation) ===
//  - true  -> TimingRecorder writes one line per frame to D:\SLog-<port>\timing.native.log
//  - false -> all TimingRecorder Mark/Flush calls short-circuit; zero log file produced
// Matching switch on the C# side: TimingRecorder.Enabled in TimingRecorder.cs.
constexpr bool kEnableTimingLog = false;

// === Pipelined inference (route B) ===
//  - true  -> SharedDetectorScheduler splits InferLoop into PreLoop (CPU
//             preprocess) + GpuLoop (session.Run + CPU postprocess) with a
//             small pre->gpu queue between them. CPU work overlaps with GPU
//             inference; throughput rises from ~24 fps to ~37 fps under
//             DirectML on the Person path.
//  - false -> single-thread InferLoop (sequential pre -> gpu -> post). Use
//             this to A/B compare or to fall back if the pipelined path
//             regresses.
// Only effective when the active detector reports HasPipelined() == true
// (PersonAdapter does; MotionAdapter does not). When false the scheduler
// uses InferLoop for both kinds, identical to the route A baseline.
constexpr bool kEnablePipelinedInference = true;

}  // namespace gai
