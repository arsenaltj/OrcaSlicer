#pragma once

#include <boost/filesystem/path.hpp>
#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r::AI {

struct BeautyAppearanceOptions {
    // Face order is exactly load_model_artifact's order, including scene instances.
    // The minimum weight of all faces sampling a texel wins: shared UVs never
    // let a selected face recolor an unselected face through the base texture.
    std::vector<float> face_weights;
    double hue_degrees = 0;
    double saturation = 1;
    double brightness = 0;
    double denoise = 0;
    double soften = 0;
    double detail_preserve = 0.8;
    // Optional absolute sRGB per face, in the same order as face_weights.
    // Conflicting colors at shared UV texels remain unchanged. Absolute colors
    // require neutral material/vertex RGB multipliers and no relative filters.
    std::vector<std::array<float, 3>> face_target_colors;
};

struct BeautyAppearanceResult {
    bool success = false, canceled = false;
    std::string error, source_sha256, output_sha256;
    size_t changed_pixels = 0;
};

// Edits embedded PNG/JPEG base-color pixels and publishes a new GLB atomically.
// Mesh, triangle order, UVs, transforms, alpha and existing material parameters
// remain intact. Color texture/image references are cloned so shared normal,
// roughness or emissive images are not accidentally edited. Vertex-only color,
// external images, nonzero UV sets and UVs outside one tile are explicit errors.
// Hue is [-360,360], saturation [0,4], brightness [-.5,.5]; other controls [0,1].
// Destination must not exist. Cancellation or source changes leave no output.
BeautyAppearanceResult edit_glb_appearance(const boost::filesystem::path& source,
    const boost::filesystem::path& destination, const BeautyAppearanceOptions& options,
    const std::function<bool()>& canceled = {});

} // namespace Slic3r::AI
