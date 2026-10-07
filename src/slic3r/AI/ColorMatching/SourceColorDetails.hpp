#pragma once

#include "AutomaticColorRegions.hpp"

namespace Slic3r::AI::ColorMatching {
struct SourceColorDetailOptions {
    static constexpr const char* algorithm_version = "selected-source-color-details-v1";
    double minimum_face_gain = 2.5;
    double minimum_selection_fraction = .00002;
    double minimum_region_fraction = .00025;
};

// Explicit local recovery from immutable source colors, after initial matching.
// Decisions compare each original face with its current real material. Protected
// and unselected faces never contribute proposals or the local area budget.
// The existing coherent-region/remnant guards filter proposals before applying.
std::vector<ColorRegionSplit> recover_selected_source_color_details(
    const std::vector<uint32_t>& regions,
    const std::vector<size_t>& slots,
    const std::vector<uint8_t>& selected,
    const std::vector<uint8_t>& protected_faces,
    const std::vector<std::array<int32_t, 3>>& neighbors,
    const std::vector<double>& areas,
    const std::vector<RegionRGB>& original,
    const std::vector<RegionMaterial>& materials,
    const SourceColorDetailOptions& options = {},
    const std::function<bool()>& canceled = {});
}
