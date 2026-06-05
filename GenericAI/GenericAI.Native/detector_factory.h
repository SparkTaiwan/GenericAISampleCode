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
// missing, ONNX session build failed, etc.). Callers are expected to surface
// that as exit code 4/5.
std::unique_ptr<IDetector> CreateDetector(DetectorKind kind, const std::string& model_path);

}  // namespace gai
