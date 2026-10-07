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
    // bake material/native vertex RGB into private images or corner colors;
    // they cannot be combined with relative filters.
    std::vector<std::array<float, 3>> face_target_colors;
};

struct BeautyAppearanceResult {
    bool success = false, canceled = false;
    std::string error, source_sha256, output_sha256;
    size_t changed_pixels = 0;
    size_t changed_vertices = 0;
};

// Edits embedded PNG/JPEG base-color pixels and publishes a new GLB atomically.
// Ordered triangle positions, UVs, transforms and alpha remain intact.
// Color references are cloned without changing normal/other image references.
// Absolute puzzle colors support verified dense float or normalized byte/ushort
// native colors across textured and untextured primitives. Shared untextured
// vertices use coincident corners. Texture gradients are baked at texel centers
// into private 8-bit PNGs for selected face runs; unselected runs keep original
// RGB, material and UVs. Baking introduces quantization/interpolation error;
// conflicting overlapping gradients and degenerate gradient UVs are errors.
// External images, nonzero UV sets and UVs outside one tile remain errors.
// Hue is [-360,360], saturation [0,4], brightness [-.5,.5]; other controls [0,1].
// Destination must not exist. Cancellation or source changes leave no output.
BeautyAppearanceResult edit_glb_appearance(const boost::filesystem::path& source,
    const boost::filesystem::path& destination, const BeautyAppearanceOptions& options,
    const std::function<bool()>& canceled = {});

} // namespace Slic3r::AI
