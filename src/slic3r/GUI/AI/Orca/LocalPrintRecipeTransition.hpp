#pragma once

#include "LocalPrintRecipeApplication.hpp"
#include "OrcaPrintPaletteSnapshot.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorRecipes.hpp"
#include <numeric>

namespace Slic3r::GUI::LocalPrintRecipeTransition {

struct Prepared {
    LocalPrintRecipeApplication::Prepared native;
    AI::PrintablePaletteSnapshot snapshot;
    AI::LocalPrintColorResult result;
};

// These five fields are absent from native profile capture. Preserve supplied
// evidence only for this unchanged source surface and the internally staged
// slot append. This does not acquire or authenticate a measurement, Z limit,
// sliced-region dimension, or per-object layer schedule for the GUI host.
inline void retain_surface_constraints(AI::PrintSublayerProcess& to, const AI::PrintSublayerProcess& from)
{
    to.z_resolution_mm=from.z_resolution_mm;
    to.region_width_mm=from.region_width_mm;to.region_height_mm=from.region_height_mm;
    to.surface_condition=from.surface_condition;to.measurement_condition=from.measurement_condition;
}

// Own the staging operation: callers cannot use an arbitrary modified bundle
// to migrate a confirmation. Native material/process values must match the
// supplied pre-application evidence before prepare is allowed to add slots.
inline bool prepare(const TriangleMesh& mesh, const indexed_triangle_set& source,
    const AI::LocalPrintColorResult& confirmed, const AI::PrintablePaletteSnapshot& current,
    const PresetBundle& bundle, const PrintColorNozzleRouting& routing,
    Prepared& destination, std::string& error)
{
    auto fail=[&](const char* message){error=message;return false;};
    error.clear();
    auto before=OrcaPrintPaletteSnapshot::capture(bundle,routing);
    if(current.material_fingerprint!=before.material_fingerprint || current.process_fingerprint!=before.process_fingerprint ||
       !current.material_metadata_complete || !before.material_metadata_complete)
        return fail("Recipe transition requires the current native material and process identity.");
    retain_surface_constraints(before.sublayer_process,current.sublayer_process);
    if(current.sublayer_process.sublayers_enabled!=before.sublayer_process.sublayers_enabled ||
       LocalPrintRecipeProofState::context(current.sublayer_materials,current.sublayer_process)!=
           LocalPrintRecipeProofState::context(before.sublayer_materials,before.sublayer_process))
        return fail("Supplied recipe constraints disagree with native material or process values.");
    Prepared prepared;
    if(!LocalPrintRecipeApplication::prepare(mesh,source,confirmed,current,bundle,prepared.native,error)) return false;
    prepared.snapshot=OrcaPrintPaletteSnapshot::capture(*prepared.native.bundle,routing);
    auto& after=prepared.snapshot;
    retain_surface_constraints(after.sublayer_process,current.sublayer_process);
    auto old_context=LocalPrintRecipeProofState::context(before.sublayer_materials,before.sublayer_process);
    auto new_context=LocalPrintRecipeProofState::context(after.sublayer_materials,after.sublayer_process);
    for(const char* key:{"material_fingerprint","process_fingerprint"}) {old_context.erase(key);new_context.erase(key);}
    if(old_context!=new_context || !after.material_metadata_complete ||
       after.sublayer_process.sublayers_enabled!=before.sublayer_process.sublayers_enabled)
        return fail("Adding recipe slots changed the physical constraints; match the model again.");

    prepared.result=confirmed;
    prepared.result.material_fingerprint=after.material_fingerprint;
    prepared.result.process_fingerprint=after.process_fingerprint;
    // Re-enumerate under the actual staged fingerprints. Repeated targets may
    // share a candidate, but each retains its original face/region assignment.
    for(auto& target:prepared.result.targets) {
        if(!target.recipe) continue;
        const auto old=target;
        auto input=LocalPrintColorRecipes::from_workspace(after);
        int step=100,minimum=100;
        for(const auto& component:old.recipe->components) {
            const int weight=int(std::lround(component.ratio*100));
            step=std::gcd(step,weight);minimum=std::min(minimum,weight);
        }
        input.ratio_step_percent=step;input.minimum_component_percent=minimum;
        // Calibration identity contains project fingerprints. Carry its
        // provenance only across the verified unchanged physical context;
        // rebuild the key for precisely the same ordered proportions.
        if(old.evidence==AI::ColorEvidence::Measured || old.evidence==AI::ColorEvidence::Interpolated) {
            auto key=LocalPrintRecipeProofState::context(input.materials,input.process);
            key["components"]=nlohmann::json::array();
            for(const auto& c:old.recipe->components) key["components"].push_back({c.slot,int(std::lround(c.ratio*100))});
            const auto& proof=*old.recipe_proof;
            input.calibration.push_back({LocalPrintRecipeProofState::digest(key),proof.evidence_source,
                proof.evidence_sha256,old.evidence,old.output,proof.uncertainty_delta_e});
        }
        const auto catalog=LocalPrintColorRecipes::enumerate(input);
        if(!catalog.ok()) {error=catalog.error;return false;}
        const auto candidate=std::find_if(catalog.candidates.begin(),catalog.candidates.end(),[&](const auto& c){
            if(c.recipe.components.size()!=old.recipe->components.size()) return false;
            for(size_t i=0;i<c.recipe.components.size();++i)
                if(c.recipe.components[i].slot!=old.recipe->components[i].slot ||
                   std::abs(c.recipe.components[i].ratio-old.recipe->components[i].ratio)>1e-9) return false;
            return true;
        });
        if(candidate==catalog.candidates.end() || candidate->color!=old.output || candidate->evidence!=old.evidence ||
           candidate->evidence_source!=old.recipe_proof->evidence_source ||
           candidate->evidence_sha256!=old.recipe_proof->evidence_sha256 ||
           candidate->uncertainty_delta_e!=old.recipe_proof->uncertainty_delta_e ||
           candidate->sublayer_heights_mm!=old.recipe_proof->sublayer_heights_mm)
            return fail("The staged recipe no longer reproduces the confirmed output and layer constraints.");
        target.recipe=candidate->recipe;target.recipe_proof=candidate->proof;target.candidate_id=candidate->id;
        if(!LocalPrintRecipeProofState::valid(target,prepared.result,error)) return false;
    }
    if(!prepared.result.valid(error)) return false;
    destination=std::move(prepared);
    return true;
}
} // namespace Slic3r::GUI::LocalPrintRecipeTransition
