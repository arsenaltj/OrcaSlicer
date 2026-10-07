#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace Slic3r::AI::ColorMatching {
struct ColorIslandCleanupOptions {
    static constexpr const char* algorithm_version = "enclosed-color-islands-v1";
    double maximum_island_fraction = .003;
    double minimum_surrounding_area_ratio = 20.;
};
struct ColorIslandReplacement {
    size_t slot;
    std::vector<size_t> faces;
};
// Explicit local cleanup of matched colors, never a whole-model automatic pass.
// Proposals use frozen inputs and existing slot identities, including equal-RGB
// slots. Selection edges, open edges, creases and protected faces are barriers.
std::vector<ColorIslandReplacement> find_enclosed_color_islands(
    const std::vector<size_t>& face_slots,
    const std::vector<uint8_t>& selected,
    const std::vector<uint8_t>& protected_faces,
    const std::vector<std::array<int32_t, 3>>& neighbors,
    const std::vector<double>& areas,
    const std::vector<std::array<uint8_t, 3>>& smooth_edges,
    const ColorIslandCleanupOptions& options = {},
    const std::function<bool()>& canceled = {});
}
