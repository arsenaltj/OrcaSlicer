#pragma once

#include "LocalPrintColorApplication.hpp"
#include "slic3r/GUI/AI/Model/LocalPrintRecipeProofState.hpp"
#include "libslic3r/PresetBundle.hpp"
#include <memory>

namespace Slic3r::GUI::LocalPrintRecipeApplication {

// Staged native state, never a claim that the supplied region/measurement
// constraints were obtained from this model. The host must check that evidence
// and revalidate the input identity immediately before its undo transaction.
struct Prepared {
    std::unique_ptr<PresetBundle> bundle;
    LocalPrintColorApplication::PreparedPainting painting;
    std::vector<size_t> target_slots;
    std::string input_identity;
    size_t added_slots {0};
};

inline std::string identity(const PresetBundle& bundle)
{
    nlohmann::json state={{"presets",bundle.filament_presets},{"project",nlohmann::json::object()}};
    for(const auto& key:bundle.project_config.keys())
        state["project"][key]=bundle.project_config.option(key)->serialize();
    return LocalPrintRecipeProofState::digest(state);
}

inline bool prepare(const TriangleMesh& mesh, const indexed_triangle_set& source,
    const AI::LocalPrintColorResult& result, const AI::PrintablePaletteSnapshot& current,
    const PresetBundle& bundle, Prepared& destination, std::string& error)
{
    auto fail=[&](const char* message){error=message;return false;};
    error.clear();
    if(!result.valid(error)) return false;
    if(!result.confirmed || result.material_fingerprint!=current.material_fingerprint ||
       result.process_fingerprint!=current.process_fingerprint)
        return fail("Confirm a current color result before preparing native recipe slots.");
    if(source.indices.size()!=result.face_count || result.face_count>size_t(std::numeric_limits<int>::max()) ||
       AI::SurfaceSelectionPersistence::geometry_fingerprint(source)!=result.geometry_id ||
       !LocalPrintColorApplication::same_surface_partition(source,mesh.its))
        return fail("Recipe painting belongs to different model geometry.");
    const size_t count=bundle.filament_presets.size();
    const size_t capacity=size_t(EnforcerBlockerType::ExtruderMax)-size_t(EnforcerBlockerType::Extruder1)+1;
    const auto* colors=bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    const auto* mixed=bundle.project_config.option<ConfigOptionBools>("filament_is_mixed");
    if(!colors || !mixed || count==0 || count>capacity || colors->values.size()!=count || mixed->values.size()!=count)
        return fail("Native material arrays do not match the current slot count.");
    const size_t physical=std::count(mixed->values.begin(),mixed->values.end(),false);
    if(physical!=current.physical_channels.size() || physical!=result.physical_channels.size())
        return fail("The result does not contain the current physical material set.");
    // Native component IDs address a physical prefix. Never compact a sparse
    // project or silently reinterpret a virtual slot as a physical component.
    for(size_t i=0;i<count;++i) if(bool(mixed->values[i])!=(i>=physical))
        return fail("Native recipe components require physical slots before virtual slots.");
    for(size_t i=0;i<physical;++i) {
        const auto a=std::find_if(current.physical_channels.begin(),current.physical_channels.end(),[&](const auto& c){return c.slot==i;});
        const auto b=std::find_if(result.physical_channels.begin(),result.physical_channels.end(),[&](const auto& c){return c.slot==i;});
        if(a==current.physical_channels.end() || b==result.physical_channels.end() ||
           a->material_type!=b->material_type || a->compatible!=b->compatible ||
           LocalPrintRecipeProofState::canonical_color(a->display_color)!=LocalPrintRecipeProofState::canonical_color(b->display_color) ||
           LocalPrintRecipeProofState::canonical_color(a->display_color)!=LocalPrintRecipeProofState::canonical_color(colors->values[i]))
            return fail("Native physical slots disagree with the confirmed material snapshot.");
    }
    auto strings=[&](const char* key)->std::vector<std::string>{
        const auto* v=bundle.project_config.option<ConfigOptionStrings>(key);
        return v?v->values:std::vector<std::string>{};
    };
    auto bools=[&](const char* key)->std::vector<unsigned char>{
        const auto* v=bundle.project_config.option<ConfigOptionBools>(key);
        return v?v->values:std::vector<unsigned char>{};
    };
    const auto components=strings("filament_mixed_components"), ratios=strings("filament_mixed_sublayer_ratios");
    const auto gradients=bools("filament_mixed_gradient");
    const auto ranges=strings("filament_mixed_gradient_range"), curves=strings("filament_mixed_gradient_curve");
    for(const auto* key:{"filament_multi_colour","filament_colour_type","filament_mixed_components",
                        "filament_mixed_sublayer_ratios","filament_mixed_gradient_range","filament_mixed_gradient_curve"})
        if(strings(key).size()!=count) return fail("Native per-slot string arrays are incomplete.");
    for(const auto* key:{"filament_mixed_gradient","filament_mixed_gradient_per_part"})
        if(bools(key).size()!=count) return fail("Native per-slot flags are incomplete.");
    for(const auto* key:{"filament_map","filament_nozzle_map","filament_volume_map"}) {
        const auto* values=bundle.project_config.option<ConfigOptionInts>(key);
        if(!values || values->values.size()!=count) return fail("Native per-slot routing arrays are incomplete.");
    }
    if(!validate_mixed_filament_params(mixed->values,components,ratios,gradients,ranges,curves).empty())
        return fail("Existing native mixed material definitions are invalid.");
    Prepared prepared;
    prepared.input_identity=identity(bundle);
    prepared.bundle=std::make_unique<PresetBundle>(bundle);
    auto& staged=*prepared.bundle;
    for(const auto& target:result.targets) {
        if(target.physical_slot) {prepared.target_slots.push_back(*target.physical_slot);continue;}
        if(!target.executable || !target.recipe || !LocalPrintRecipeProofState::valid(target,result,error)) return false;
        const auto& proof=*target.recipe_proof;
        if(!current.material_metadata_complete || !current.sublayer_process.sublayers_enabled ||
           !current.sublayer_process.first_layer_unsplit ||
           LocalPrintRecipeProofState::context(proof.materials,proof.process)!=
               LocalPrintRecipeProofState::context(current.sublayer_materials,current.sublayer_process))
            return fail("Recipe constraints do not match the fresh workspace evidence.");
        std::string component_text,ratio_text;
        std::vector<unsigned int> ids;std::vector<double> weights;
        for(const auto& component:target.recipe->components) {
            if(component.slot>=physical) return fail("Recipe references a virtual or absent component.");
            if(!component_text.empty()) {component_text+=',';ratio_text+=',';}
            component_text+=std::to_string(component.slot+1);
            const int percent=int(std::lround(component.ratio*100));
            ratio_text+="0."+std::string(percent<10?"0":"")+std::to_string(percent);
            ids.push_back(unsigned(component.slot+1));weights.push_back(component.ratio);
        }
        size_t slot=staged.filament_presets.size();
        for(size_t i=physical;i<slot;++i) {
            const auto& config=staged.project_config;
            if(config.opt_bool("filament_mixed_gradient",i)) continue;
            if(LocalPrintRecipeProofState::canonical_color(config.opt_string("filament_colour",i))!=
               LocalPrintRecipeProofState::canonical_color(target.recipe->target_color)) continue;
            if(parse_mixed_components(config.opt_string("filament_mixed_components",i))!=ids) continue;
            const auto existing=parse_mixed_ratios(config.opt_string("filament_mixed_sublayer_ratios",i),ids.size());
            bool same=existing.size()==weights.size();
            for(size_t j=0;same && j<weights.size();++j) same=std::abs(existing[j]-weights[j])<1e-9;
            if(same) {slot=i;break;}
        }
        if(slot==staged.filament_presets.size()) {
            if(slot>=capacity) return fail("The native painting slot limit cannot hold all selected recipes.");
            // Use the same native sizing path as the manual mixed-material UI.
            // Its multi-color normalization is irrelevant to new slots; retain
            // every pre-existing multi-color value exactly.
            const auto previous_multi=staged.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values;
            staged.set_num_filaments(unsigned(slot+1),target.recipe->target_color);
            auto* multi=staged.project_config.option<ConfigOptionStrings>("filament_multi_colour");
            std::copy(previous_multi.begin(),previous_multi.end(),multi->values.begin());
            staged.project_config.option<ConfigOptionBools>("filament_is_mixed")->values[slot]=true;
            staged.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[slot]=component_text;
            staged.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[slot]=ratio_text;
            staged.filament_presets[slot]=staged.filament_presets[target.recipe->components.front().slot];
            ++prepared.added_slots;
        }
        prepared.target_slots.push_back(slot);
    }
    TriangleSelector selector(mesh);
    for(size_t face=0;face<result.face_count;++face)
        selector.set_facet(int(face),static_cast<EnforcerBlockerType>(int(EnforcerBlockerType::Extruder1)+int(prepared.target_slots[result.face_targets[face]])));
    prepared.painting.data=selector.serialize();
    prepared.painting.geometry_id=AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh.its);
    prepared.painting.base_extruder=int(*std::min_element(prepared.target_slots.begin(),prepared.target_slots.end()))+1;
    destination=std::move(prepared);
    return true;
}

// Painting half of the host transaction. The host still owns committing the
// staged bundle, invalidating the slice, and undoing both pieces together.
inline bool apply_painting(ModelVolume& volume, LocalPrintColorApplication::PreparedPainting&& painting,
    std::string& error)
{
    const int base_extruder=painting.base_extruder;
    if(!LocalPrintColorApplication::apply(volume,std::move(painting),error)) return false;
    auto* object=volume.get_object();
    // The 3MF loader discards the per-volume override for a single-volume
    // object. Keep its effective fallback at object level as the native UI does.
    if(object && object->volumes.size()==1)
        object->config.set("extruder",base_extruder);
    return true;
}
} // namespace Slic3r::GUI::LocalPrintRecipeApplication
