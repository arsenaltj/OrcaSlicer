#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>

namespace Slic3r::AI::ColorMatching {

using RegionRGB = std::array<float, 4>;
struct RegionMaterial { size_t slot; RegionRGB color; };
struct AutomaticRegion { uint32_t id; size_t baseline_slot; };
struct ColorRegionSplit {
    uint32_t source_region;
    size_t slot;
    RegionRGB source_color;
    std::vector<size_t> faces;
};
struct ColorRegionOptions {
    static constexpr const char* algorithm_version = "automatic-color-regions-v1";
    // Suppress weak changes and isolated texture noise. These are software
    // appearance guards, not calibrated printer resolution or colour accuracy.
    double minimum_patch_gain = 6.;
    double minimum_surface_fraction = .00005;
    double minimum_region_fraction = .002;
};

// Refine only explicitly eligible, newly automatic regions. Source arrays and
// existing assignments are read-only. Proposed islands use real supplied slots;
// every accepted face is at least as close to its source as the frozen baseline.
// The caller owns region IDs, edit/history state and applying the proposals.
std::vector<ColorRegionSplit> refine_automatic_color_regions(
    const std::vector<uint32_t>& face_regions,
    const std::vector<uint32_t>& face_patches,
    const std::vector<std::array<int32_t, 3>>& neighbors,
    const std::vector<double>& areas,
    const std::vector<RegionRGB>& source,
    const std::vector<AutomaticRegion>& eligible,
    const std::vector<RegionMaterial>& materials,
    const ColorRegionOptions& options = {},
    const std::function<bool()>& canceled = {});

} // namespace Slic3r::AI::ColorMatching
