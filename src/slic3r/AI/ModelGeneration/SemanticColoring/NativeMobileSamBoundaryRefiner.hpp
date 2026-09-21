#pragma once
#include "SemanticColoring.hpp"
#include <filesystem>
#include <memory>

namespace Slic3r::AI::SemanticColoring {
inline constexpr const char* mobile_sam_provider_id = "mobilesam.cpu.v1";
inline constexpr const char* mobile_sam_preprocess_version = "sam-resize-half-up-rgb8-normalize-zero-pad-v2";
std::unique_ptr<IBoundaryRefiner> create_mobile_sam_refiner(const std::filesystem::path& runtime_root);
// Exposed for native/reference parity tests. CHW 1x3x1024x1024; padding is zero
// after normalization, not normalized black pixels.
std::vector<float> mobile_sam_image_tensor(const RGBImage& crop, int& resized_width, int& resized_height);
}
