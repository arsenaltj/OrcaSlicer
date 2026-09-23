#pragma once

#include "slic3r/GUI/AI/Orca/LocalPrintRecipeApplication.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorRecipes.hpp"

// Synthetic capabilities for reproducible tests only. They do not certify any
// installed printer, filament batch, viewing condition, or generated model.
namespace Slic3r::Test {
struct RecipeApplicationFixture {
    TriangleMesh mesh;
    PresetBundle bundle;
    AI::PrintablePaletteSnapshot snapshot;
    AI::LocalPrintColorResult result;
    RecipeApplicationFixture(size_t components, double height_mm=10.) : mesh(its_make_cube(10,10,height_mm)) {
        bundle.set_num_filaments(3);
        const std::vector<std::string> colors={"#000000","#FFFFFF","#FF0000"};
        bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values=colors;
        snapshot.material_fingerprint="synthetic-materials";snapshot.process_fingerprint="synthetic-process";
        snapshot.material_metadata_complete=true;
        auto& p=snapshot.sublayer_process;
        p.material_fingerprint=snapshot.material_fingerprint;p.process_fingerprint=snapshot.process_fingerprint;
        p.sublayers_enabled=true;p.first_layer_unsplit=true;p.layer_heights_mm={.2};
        p.z_resolution_mm=.01;p.region_width_mm=2;p.region_height_mm=height_mm;
        p.surface_condition="synthetic-vertical-wall";p.measurement_condition="synthetic-D65";
        for(size_t i=0;i<3;++i) {
            AI::PrintSublayerMaterial material;
            material.channel={i,colors[i],"PLA",true};material.identity="synthetic-batch-"+std::to_string(i);
            material.min_layer_mm=.04;material.max_layer_mm=.3;material.line_width_mm=.4;
            material.temperature_c=210;material.min_temperature_c=190;material.max_temperature_c=230;
            snapshot.physical_channels.push_back(material.channel);snapshot.sublayer_materials.push_back(material);
        }
        const auto catalog=GUI::LocalPrintColorRecipes::enumerate(GUI::LocalPrintColorRecipes::from_workspace(snapshot));
        const auto candidate=std::find_if(catalog.candidates.begin(),catalog.candidates.end(),[&](const auto& c){return c.recipe.components.size()==components;});
        if(!catalog.ok() || candidate==catalog.candidates.end()) throw std::runtime_error("No synthetic recipe fixture");
        result.algorithm_version="synthetic-recipe-application";result.source_sha256=std::string(64,'c');
        result.geometry_id=AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh.its);
        result.material_fingerprint=snapshot.material_fingerprint;result.process_fingerprint=snapshot.process_fingerprint;
        result.requested_color_count=8;result.mode=AI::PrintColorMode::Layered;result.face_count=mesh.its.indices.size();
        result.physical_channels=snapshot.physical_channels;result.confirmed=true;
        AI::PrintColorTarget target;
        target.source=target.output=candidate->color;target.area=1;target.recipe=candidate->recipe;
        target.recipe_proof=candidate->proof;target.candidate_id=candidate->id;target.evidence=candidate->evidence;
        target.executable=target.within_tolerance=true;
        // Two logical targets intentionally share a single native recipe slot.
        result.targets={target,target};
        for(size_t f=0;f<result.face_count;++f) result.face_targets.push_back(f%2);
    }
};
} // namespace Slic3r::Test
