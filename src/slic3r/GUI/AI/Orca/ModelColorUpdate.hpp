#pragma once
#include "libslic3r/Model.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace Slic3r::GUI {
// 3MF stores a portable source basename. Only accept our UUID or GLB hash names;
// the complete mesh is still compared before transferring any paint.
inline bool same_generated_artifact_name(const std::string& saved, const std::string& incoming)
{
    auto basename = [](const std::string& value) { return value.substr(value.find_last_of("/\\") + 1); };
    const auto name = basename(incoming);
    const std::string glb_prefix = name.rfind("orcaslicer-ai-materials-", 0) == 0
        ? "orcaslicer-ai-materials-" : "orcaslicer-ai-glb-";
    if (name.rfind(glb_prefix, 0) == 0) {
        if (name.size() != glb_prefix.size() + 64 + 4 ||
            name.substr(name.size() - 4) != ".obj" || basename(saved) != name) return false;
        return std::all_of(name.begin() + glb_prefix.size(), name.end() - 4, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    }
    const std::string prefix = name.rfind("orcaslicer-ai-finish-", 0) == 0 ? "orcaslicer-ai-finish-" : "orcaslicer-ai-";
    if (name.rfind(prefix, 0) != 0 || name.size() != prefix.size() + 40 ||
        name.substr(name.size() - 4) != ".obj" || basename(saved) != name) return false;
    for (size_t i = 0; i < 36; ++i) {
        const char c = name[prefix.size() + i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// Face annotations are indexed by topology. Reject a changed mesh before any
// assignment; preserve object identity, transformations and non-color settings.
inline bool update_compatible_model_colors(ModelObject& target, const ModelObject& source)
{
    if (target.volumes.size() != 1 || source.volumes.size() != 1 ||
        !target.volumes.front()->is_model_part() || !source.volumes.front()->is_model_part()) return false;
    auto& destination = *target.volumes.front();
    const auto& incoming = *source.volumes.front();
    const auto& a = destination.mesh().its;
    const auto& b = incoming.mesh().its;
    if (a.vertices.size() != b.vertices.size() || a.indices.size() != b.indices.size() ||
        !std::equal(a.vertices.begin(), a.vertices.end(), b.vertices.begin(), [](const auto& x, const auto& y) { return (x.array() == y.array()).all(); }) ||
        !std::equal(a.indices.begin(), a.indices.end(), b.indices.begin(), [](const auto& x, const auto& y) { return (x.array() == y.array()).all(); })) return false;
    destination.mmu_segmentation_facets.assign(incoming.mmu_segmentation_facets);
    destination.config.set("extruder", incoming.config.extruder());
    target.config.set("extruder", source.config.extruder());
    return true;
}

// Import records this exact centering translation when it creates the volume.
// Apply only that known transform: never guess correspondences from positions.
inline bool matches_source_topology(const indexed_triangle_set& source, const ModelVolume& volume)
{
    if (volume.source.is_converted_from_meters || volume.source.is_converted_from_inches ||
        !volume.source.mesh_offset.allFinite()) return false;
    const auto& actual = volume.mesh().its;
    if (source.vertices.size() != actual.vertices.size() || source.indices.size() != actual.indices.size() ||
        !std::equal(source.indices.begin(), source.indices.end(), actual.indices.begin(),
            [](const auto& lhs, const auto& rhs) { return (lhs.array() == rhs.array()).all(); })) return false;
    const Vec3f translation = volume.source.mesh_offset.cast<float>();
    return std::equal(source.vertices.begin(), source.vertices.end(), actual.vertices.begin(),
        [&translation](const auto& lhs, const auto& rhs) { return ((lhs - translation).array() == rhs.array()).all(); });
}

// Apply semantic midpoint leaves directly to the existing MMU tree. This is
// transactional and requires the exact source topology; no remeshing or
// nearest-face recovery is permitted.
inline bool apply_subface_color_overrides(
    const indexed_triangle_set& expected, const indexed_triangle_set& actual,
    TriangleSelector::TriangleSplittingData& painting,
    const std::vector<std::pair<size_t, std::array<float, 3>>>& whole_faces,
    const std::vector<AI::ModelSubfaceColorOverride>& subfaces, std::string& error,
    const std::vector<AI::ModelPaletteSlot>& material_slots = {},
    const std::vector<AI::ModelFaceSlotOverride>& face_slots = {})
{
    error.clear();
    auto fail = [&error](const char* message) { error = message; return false; };
    if (subfaces.empty() && material_slots.empty() && face_slots.empty()) return true;
    if (expected.vertices.size() != actual.vertices.size() || expected.indices.size() != actual.indices.size() ||
        !std::equal(expected.vertices.begin(), expected.vertices.end(), actual.vertices.begin(),
            [](const auto& lhs, const auto& rhs) { return (lhs.array() == rhs.array()).all(); }) ||
        !std::equal(expected.indices.begin(), expected.indices.end(), actual.indices.begin(),
            [](const auto& lhs, const auto& rhs) { return (lhs.array() == rhs.array()).all(); }))
        return fail("The imported topology changed before semantic subfaces were applied.");

    TriangleMesh mesh(actual);
    TriangleSelector selector(mesh);
    selector.deserialize(painting);
    std::map<std::string, EnforcerBlockerType> slot_states;
    for (const auto& slot : material_slots) {
        if (slot.slot_id.empty() || slot.project_slot >= size_t(EnforcerBlockerType::ExtruderMax) ||
            std::any_of(slot.color.begin(), slot.color.end(), [](float value) {
                return !std::isfinite(value) || value < 0.f || value > 1.f;
            }) || !slot_states.emplace(slot.slot_id, EnforcerBlockerType(slot.project_slot + 1)).second)
            return fail("An explicit palette slot is invalid or duplicated.");
    }
    if (!material_slots.empty() || !face_slots.empty()) {
        if (material_slots.empty() || face_slots.size() != actual.indices.size())
            return fail("Explicit material import requires a slot for every source face.");
        std::vector<bool> assigned(actual.indices.size(), false);
        for (const auto& face : face_slots) {
            const auto slot = slot_states.find(face.slot_id);
            if (face.face_id >= assigned.size() || assigned[face.face_id] || slot == slot_states.end())
                return fail("An explicit whole-face slot is missing, invalid or duplicated.");
            assigned[face.face_id] = true;
            selector.set_facet(int(face.face_id), slot->second);
        }
    }
    std::map<size_t, std::array<float, 3>> root_colors;
    for (const auto& item : whole_faces) {
        if (item.first >= actual.indices.size() || std::any_of(item.second.begin(), item.second.end(), [](float value) {
                return !std::isfinite(value) || value < 0.f || value > 1.f;
            })) return fail("A whole-face material reference is invalid.");
        root_colors[item.first] = item.second;
    }
    std::map<std::array<float, 3>, EnforcerBlockerType> color_states;
    for (const auto& item : root_colors) {
        if (!material_slots.empty()) break;
        EnforcerBlockerType state;
        if (!selector.facet_state(int(item.first), state))
            return fail("Whole-face painting was split before semantic subfaces were applied.");
        const auto found = color_states.find(item.second);
        if (found != color_states.end() && found->second != state)
            return fail("One target color maps to multiple imported material states.");
        color_states[item.second] = state;
    }

    std::map<size_t, std::vector<TriangleSelector::MidpointSubfaceState>> grouped;
    for (const AI::ModelSubfaceColorOverride& item : subfaces) {
        if (item.face_id >= actual.indices.size() || item.depth == 0 || item.depth > 3 ||
            unsigned(item.path) >= (1u << (2u * item.depth)) ||
            std::any_of(item.color.begin(), item.color.end(), [](float value) {
                return !std::isfinite(value) || value < 0.f || value > 1.f;
            })) return fail("A semantic subface material reference is invalid.");
        EnforcerBlockerType state;
        if (!material_slots.empty()) {
            const auto slot = slot_states.find(item.slot_id);
            if (slot == slot_states.end())
                return fail("A semantic subface target has no explicit palette slot.");
            state = slot->second;
        } else {
            if (!item.slot_id.empty())
                return fail("A semantic subface slot needs the complete explicit palette.");
            const auto legacy = color_states.find(item.color);
            if (legacy == color_states.end())
                return fail("A semantic subface target has no imported material state.");
            state = legacy->second;
        }
        grouped[item.face_id].push_back({item.depth, item.path, state});
    }
    for (const auto& face : grouped) {
        EnforcerBlockerType root_state;
        if (!selector.facet_state(int(face.first), root_state) ||
            !selector.set_facet_midpoint_subfaces(int(face.first), root_state, face.second))
            return fail("The semantic subface tree is invalid for the imported face.");
    }
    painting = selector.serialize();
    return true;
}
} // namespace Slic3r::GUI
