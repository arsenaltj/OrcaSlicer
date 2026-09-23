#pragma once

#include "LocalPrintRecipeTransition.hpp"
#include "slic3r/GUI/AI/Model/LocalPrintColorState.hpp"

namespace Slic3r::GUI::LocalPrintColorRestore {

// Read-only recovery. Calibration is supplied independently by the caller;
// never promote provenance embedded in the saved record into fresh evidence.
// The native bundle must already contain every required recipe slot.
inline bool restore(const AI::LocalPrintColorResult& saved, AI::LocalPrintColorResult current,
    const AI::PrintablePaletteSnapshot& snapshot, const PresetBundle& bundle,
    const PrintColorNozzleRouting& routing, const TriangleMesh& mesh,
    const indexed_triangle_set& source, const TriangleSelector::TriangleSplittingData& actual,
    const std::vector<LocalPrintColorRecipes::Calibration>& calibration,
    AI::LocalPrintColorResult& destination, std::string& error)
{
    auto fail = [&](const char* message) { error = message; return false; };
    error.clear();
    if (!saved.valid(error)) return false;
    const bool recipes = std::any_of(saved.targets.begin(), saved.targets.end(),
        [](const auto& target) { return bool(target.recipe); });
    if (!recipes) return LocalPrintColorState::restore_confirmed(saved, current,
        LocalPrintColorApplication::matches_saved_painting(mesh, source, actual, saved), destination, error);

    auto native = OrcaPrintPaletteSnapshot::capture(bundle, routing);
    if (!native.material_metadata_complete || !snapshot.material_metadata_complete ||
        native.material_fingerprint != snapshot.material_fingerprint ||
        native.process_fingerprint != snapshot.process_fingerprint ||
        current.material_fingerprint != snapshot.material_fingerprint ||
        current.process_fingerprint != snapshot.process_fingerprint)
        return fail("Saved recipes require the current native material and process identity.");
    LocalPrintRecipeTransition::retain_surface_constraints(native.sublayer_process, snapshot.sublayer_process);
    if (native.sublayer_process.sublayers_enabled != snapshot.sublayer_process.sublayers_enabled ||
        LocalPrintRecipeProofState::context(native.sublayer_materials, native.sublayer_process) !=
            LocalPrintRecipeProofState::context(snapshot.sublayer_materials, snapshot.sublayer_process))
        return fail("Recipe recovery constraints disagree with the current native configuration.");

    current.targets.clear(); // Saved target proofs cannot serve as fresh proof.
    for (const auto& target : saved.targets) {
        if (!target.recipe) continue;
        if (!LocalPrintRecipeProofState::valid(target, saved, error)) return false;
        auto input = LocalPrintColorRecipes::from_workspace(snapshot);
        input.calibration = calibration;
        int step = 100, minimum = 100;
        for (const auto& component : target.recipe->components) {
            const int percent = int(std::lround(component.ratio * 100));
            step = std::gcd(step, percent); minimum = std::min(minimum, percent);
        }
        input.ratio_step_percent = step; input.minimum_component_percent = minimum;
        const auto catalog = LocalPrintColorRecipes::enumerate(input);
        if (!catalog.ok()) { error = catalog.error; return false; }
        const auto candidate = std::find_if(catalog.candidates.begin(), catalog.candidates.end(), [&](const auto& value) {
            return value.id == target.candidate_id && value.proof &&
                value.proof->checksum == target.recipe_proof->checksum &&
                value.color == target.output && value.evidence == target.evidence;
        });
        if (candidate == catalog.candidates.end())
            return fail("Saved recipe color and constraints lack matching current evidence.");
        auto fresh = target;
        fresh.recipe = candidate->recipe; fresh.recipe_proof = candidate->proof;
        current.targets.push_back(std::move(fresh));
    }
    LocalPrintRecipeApplication::Prepared prepared;
    if (!LocalPrintRecipeApplication::prepare(mesh, source, saved, snapshot, bundle, prepared, error)) return false;
    if (prepared.added_slots != 0 || LocalPrintRecipeApplication::identity(*prepared.bundle) != prepared.input_identity)
        return fail("Saved recipe slots are absent or changed; recovery does not modify project materials.");
    TriangleSelector selector(mesh);
    selector.deserialize(actual);
    return LocalPrintColorState::restore_confirmed(saved, current,
        selector.serialize() == prepared.painting.data, destination, error);
}
} // namespace Slic3r::GUI::LocalPrintColorRestore
