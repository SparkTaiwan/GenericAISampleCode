#pragma once

#include "idetector.h"

#include <memory>
#include <string>

namespace gai {

// Builds an IDetector by kind. For DetectorKind::Person the model_path must
// point at an .onnx file readable by ONNX Runtime. For DetectorKind::Motion
// model_path is ignored.
//
// Returns nullptr if the requested detector cannot be constructed (model
// missing, ONNX session build failed, etc.); on failure `err` is filled with a
// human-readable reason so the host can report it on /Alive instead of crashing.
std::unique_ptr<IDetector> CreateDetector(DetectorKind kind, const std::string& model_path, std::string& err);

}  // namespace gai
