#pragma once
#include "PortraitPrintFlow.hpp"
#include <optional>
#include <utility>

namespace Slic3r::GUI::PortraitPrint {
// Only a later explicit card application or Save action may retry this state.
// A paused owner with an invalid contract is blocked, never a generic Beauty save.
inline bool owns_paused_color_save(const State& flow, bool exact_owner)
{
    return exact_owner && flow.from_workbench() && flow.active() &&
        flow.stage == Stage::Color && flow.paused;
}

template<class Preview>
bool eligible_paused_color_save_retry(const State& flow, bool exact_owner, const Preview& preview)
{
    if (!owns_paused_color_save(flow, exact_owner) || flow.failed || flow.authorized ||
        flow.record_version < 7 || flow.partition_needs_rebuild(12) ||
        preview.color_trial_state().suite_id != "honor.people.six" ||
        preview.has_native_portrait_partition_conflict()) return false;
    const auto& reference = flow.native_portrait_partition;
    if (!same_model_path(reference.at("carrier_path").get<std::string>(), flow.model_path) ||
        reference.at("carrier_sha256") != flow.model_hash ||
        reference.at("geometry_id") != flow.geometry_id ||
        reference.at("appearance_id") != flow.appearance_id) return false;
    const auto native = preview.native_portrait_partition();
    const auto colors = preview.portrait_color_blocks();
    const size_t faces = preview.triangle_count();
    if (!native || !native->valid() || native->geometry_id != flow.geometry_id ||
        native->partition_digest != flow.basis_signature || !colors ||
        colors->algorithm_version != 12 || colors->geometry_id != flow.geometry_id ||
        colors->source_basis_signature != flow.basis_signature || colors->face_count != faces ||
        faces == 0 || !colors->native_partition_identity ||
        !colors->native_partition_identity->valid() ||
        colors->native_partition_identity->algorithm != 12 ||
        colors->native_partition_identity->geometry_id != flow.geometry_id ||
        colors->native_partition_identity->partition_digest != flow.basis_signature ||
        colors->native_partition_identity->face_count != faces ||
        !preview.import_subface_color_overrides(true).empty()) return false;
    const auto exported = preview.printer_filament_export_faces();
    if (exported.size() != faces) return false;
    for (const auto& face : exported) if (face.first >= faces) return false;
    return true;
}

template<class Preview, class Resume>
std::optional<bool> resume_paused_color_save_after_explicit_action(
    const State& flow, bool exact_owner, const Preview& preview, Resume&& resume)
{
    if (!owns_paused_color_save(flow, exact_owner)) return std::nullopt;
    if (!eligible_paused_color_save_retry(flow, exact_owner, preview)) return false;
    // The existing entry rechecks file hashes, checkpoints and certificates.
    return std::forward<Resume>(resume)();
}
}
