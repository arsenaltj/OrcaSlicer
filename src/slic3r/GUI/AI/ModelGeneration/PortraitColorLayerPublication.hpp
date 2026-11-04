#pragma once

#include "PortraitColorBlockCache.hpp"

namespace Slic3r::GUI::PortraitColorBlocks {

struct PublishedColorLayer {
    nlohmann::json reference;
    size_t painted_faces {0};
    size_t painted_subfaces {0};

    nlohmann::json encode() const {
        return {{"schema", "orca.portrait-applied-color-layer/v1"}, {"reference", reference},
            {"painted_faces", painted_faces}, {"painted_subfaces", painted_subfaces}};
    }
};

// The canonical geometry carrier alone is not a colored result. Require the
// actual face/child appearance and region bindings to survive a disk round trip.
inline PublishedColorLayer publish_verified_color_layer(
    const Result& state, const boost::filesystem::path& cache)
{
    if (!state.face_count || state.units.empty())
        throw std::runtime_error("人物配色没有可保存的面或子面颜色。");
    const auto reference = save(state, cache);
    std::string error;
    const auto restored = load(reference, cache, state.geometry_id, state.face_count,
        state.source_signature, error);
    if (!restored) throw std::runtime_error(error.empty() ? "人物配色颜色层无法读回。" : error);
    SC::FaceColors expected_faces, saved_faces;
    SC::SubfaceColors expected_children, saved_children;
    state.appearance(expected_faces, expected_children);
    restored->appearance(saved_faces, saved_children);
    if ((saved_faces.empty() && saved_children.empty()) || saved_faces != expected_faces ||
        saved_children != expected_children || restored->role_regions != state.role_regions ||
        restored->role_slot_locks != state.role_slot_locks)
        throw std::runtime_error("人物配色的面、子面或区域绑定保存核验失败。");
    return {reference, saved_faces.size(), saved_children.size()};
}

} // namespace Slic3r::GUI::PortraitColorBlocks
