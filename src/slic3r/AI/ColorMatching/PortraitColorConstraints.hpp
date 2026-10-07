#pragma once

#include "AutomaticColorRegions.hpp"
#include <string>

namespace Slic3r::AI::ColorMatching {
enum class PortraitFaceRole : uint8_t { Unknown, Skin, Eye, Hair, Lips, Mouth, Other };
PortraitFaceRole portrait_face_role(const std::string& name);

struct PortraitColorConstraintOptions {
    static constexpr const char* algorithm_version = "selected-portrait-material-constraints-v4";
    std::vector<size_t> lip_slots;
    double maximum_source_error_increase = 8.;
    unsigned lip_boundary_guard_rings = 2;
    // Soft software boundary cost; the source-loss limit remains a hard bound.
    double boundary_edge_penalty = 4.;
    unsigned coherence_passes = 8;
    // Joint moves escape single-face local minima; false retains v2 coherence.
    bool component_coherence = true;
    // Used by geometry-aware callers; missing geometry retains v3 costs.
    bool geometric_coherence = true;
    double boundary_scale_fraction = .001;
};

struct PortraitColorGeometry {
    // Side order must match neighbors; lengths use the same units as areas.
    const std::vector<std::array<float,3>>& edge_lengths;
    double reference_length;
};

// Optional display-colour heuristic for callers lacking explicit material roles.
// This proposes red/pink lip slots, not calibrated material semantics. The
// constraint solver consumes an explicit slot list and never invents a slot.
std::vector<size_t> suggest_lip_material_slots(const std::vector<RegionMaterial>& materials);

// Explicit selected correction after matching. Only known skin/eye/hair faces
// using a lip slot are reconsidered. Lips, their rim, mouth, unknown/other faces
// and protected paint remain unchanged. Loss of source fidelity is bounded and
// intentional; this is a semantic correction, not a lower-source-error claim.
std::vector<ColorRegionSplit> constrain_selected_portrait_materials(
    const std::vector<uint32_t>& regions, const std::vector<size_t>& slots,
    const std::vector<PortraitFaceRole>& roles, const std::vector<uint8_t>& selected,
    const std::vector<uint8_t>& protected_faces,
    const std::vector<std::array<int32_t,3>>& neighbors, const std::vector<double>& areas,
    const std::vector<RegionRGB>& original, const std::vector<RegionMaterial>& materials,
    const PortraitColorConstraintOptions& options, const std::function<bool()>& canceled = {},
    const PortraitColorGeometry* geometry = nullptr);
}
