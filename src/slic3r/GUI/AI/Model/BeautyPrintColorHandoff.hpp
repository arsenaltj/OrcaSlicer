#pragma once

#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include "slic3r/AI/Contracts/LocalPrintColorResult.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace Slic3r::GUI::BeautyPrintColorHandoff {

// Transfer accepted physical assignments as explicit locks, not RGB samples.
// Build the replacement first so a stale source/palette cannot partially edit
// the destination draft. Native mixed assignments use their existing importer.
inline void seed(const AI::ModelMatchedColors& saved, AI::LocalPrintColorResult& draft)
{
    if (!saved.valid() || saved.source_sha256 != draft.source_sha256 ||
        saved.geometry_id != draft.geometry_id || saved.face_slots.size() != draft.face_count)
        throw std::invalid_argument("Saved beauty colors do not match this model. Reopen the accepted version.");
    if (!saved.mixed_recipes.empty())
        throw std::invalid_argument("This beauty version uses native mixed filaments. Import it directly from the beauty workbench.");
    if (saved.palette.size() != draft.physical_channels.size() ||
        !AI::is_valid_physical_channel_set(draft.physical_channels))
        throw std::invalid_argument("Materials changed since beauty matching. Reopen the workbench and match explicitly.");
    for (const auto& channel : saved.palette) {
        if (std::none_of(draft.physical_channels.begin(), draft.physical_channels.end(), [&](const auto& current) {
            return current.slot == channel.slot && current.display_color == channel.display_color &&
                current.material_type == channel.material_type && current.compatible == channel.compatible;
        })) throw std::invalid_argument("Materials changed since beauty matching. Reopen the workbench and match explicitly.");
    }
    if (!draft.regions.empty() || !draft.user_overrides.empty())
        throw std::invalid_argument("Beauty assignments cannot replace an existing color draft.");

    std::map<size_t, AI::PrintColorRegion> grouped;
    for (size_t face = 0; face < saved.face_slots.size(); ++face) {
        const auto slot = saved.face_slots[face];
        auto& region = grouped[slot];
        region.faces.push_back(face);
        region.locked_physical_slot = slot;
    }
    std::vector<AI::PrintColorRegion> regions;
    for (auto& entry : grouped) {
        auto& region = entry.second;
        region.id = "beauty-material-" + std::to_string(entry.first);
        region.label = "accepted beauty material";
        region.confidence = 1;
        region.user_protected = true;
        regions.push_back(std::move(region));
    }
    draft.regions = std::move(regions);
    draft.requested_color_count = saved.palette.size();
    draft.confirmed = false;
}

// An explicit new target color supersedes material locks only on those faces.
// Region protection elsewhere, including equal-RGB material identities, stays.
inline void unlock_faces(std::vector<AI::PrintColorRegion>& regions, const std::set<size_t>& faces)
{
    for (auto& region : regions) if (region.locked_physical_slot)
        region.faces.erase(std::remove_if(region.faces.begin(), region.faces.end(),
            [&](size_t face) { return faces.count(face) != 0; }), region.faces.end());
}

} // namespace Slic3r::GUI::BeautyPrintColorHandoff
