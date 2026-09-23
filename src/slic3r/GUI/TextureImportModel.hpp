#pragma once

#include "TextureImportDialog.hpp"
#include "libslic3r/Model.hpp"
#include "AI/Orca/LocalPrintColorApplication.hpp"

namespace Slic3r::GUI {

// The workbench already knows glTF units; do not guess again from model size.
inline bool apply_texture_import_units(Model& model, const TextureImportOptions* options)
{
    if (!options || !options->source_units_in_meters) return false;
    model.convert_from_meters(false);
    return true;
}

// This mutates only the staged import model. Validate all assignments, current
// materials and native face correspondence before publishing any annotations.
inline void apply_matched_texture_colors(Model& model,const TextureImportOptions& options,
                                         const std::vector<TextureFilamentEntry>& current) {
    auto require=[](bool condition,const char* message){if(!condition)throw std::runtime_error(message);};
    require(options.matched_source && model.texture_mesh && model.objects.size()==1 && model.objects.front()->volumes.size()==1,
            "已保存区域配色需要完整的单部件模型。");
    auto* object=model.objects.front();auto* volume=object->volumes.front();
    // UV welding and unit conversion can produce different vertex aliases.
    // Bind paint to the same ordered physical corners, within the existing
    // bounded float tolerance. Dropped, reordered or reversed faces still fail.
    require(volume->is_model_part() && LocalPrintColorApplication::same_surface_partition(*options.matched_source,volume->mesh().its,false) &&
            options.matched_face_slots.size()==volume->mesh().its.indices.size(),"导入模型的三角面已变化，无法沿用区域配色。");
    const size_t max_slot=int(EnforcerBlockerType::ExtruderMax)-int(EnforcerBlockerType::Extruder1);
    std::set<size_t> allowed;
    for(const auto& saved:options.matched_filaments) {
        require(saved.kind==TextureFilamentKind::ExistingPhysical || saved.kind==TextureFilamentKind::ExistingMixed,"区域耗材类型无效。");
        const auto found=std::find_if(current.begin(),current.end(),[&](const auto& c){return c.kind==saved.kind &&
            c.project_config_index==saved.project_config_index && c.color_hex==saved.color_hex &&
            (saved.kind==TextureFilamentKind::ExistingMixed?(c.mixed_components==saved.mixed_components && c.mixed_ratios==saved.mixed_ratios):c.type==saved.type);});
        require(found!=current.end() && saved.project_config_index<=max_slot,"耗材已变化，请回工作台重新匹配颜色。");
        allowed.insert(saved.project_config_index);
    }
    require(!options.matched_face_slots.empty(),"区域配色为空。");
    for(size_t slot:options.matched_face_slots)require(allowed.count(slot)!=0,"区域使用了不可用的耗材。");
    TriangleSelector selector(volume->mesh());
    for(size_t f=0;f<options.matched_face_slots.size();++f)
        selector.set_facet(int(f),EnforcerBlockerType(int(EnforcerBlockerType::Extruder1)+int(options.matched_face_slots[f])));
    const int base=int(*std::min_element(options.matched_face_slots.begin(),options.matched_face_slots.end()))+1;
    volume->mmu_segmentation_facets.set(selector);volume->config.set("extruder",base);object->config.set("extruder",base);
}

} // namespace Slic3r::GUI
