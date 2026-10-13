#pragma once

#include "TextureImportDialog.hpp"
#include "TextureImportFilamentUsage.hpp"
#include "libslic3r/Model.hpp"
#include "AI/Orca/LocalPrintColorApplication.hpp"

namespace Slic3r::GUI {

// Preserve project slot identity when removing unusable workbench entries.
// The ordinary native importer keeps its existing physical/mixed behaviour.
inline std::vector<TextureFilamentEntry> workspace_texture_filaments(
    const std::vector<TextureFilamentEntry>& entries, const TextureImportOptions* options)
{
    if (!options || !options->workbench_review) return entries;
    std::vector<TextureFilamentEntry> usable;
    for (const auto& entry : entries) {
        if (entry.kind != TextureFilamentKind::ExistingPhysical || !entry.compatible ||
            entry.type.empty() || entry.preset_name.empty()) continue;
        usable.push_back(entry);
        usable.back().dialog_index = int(usable.size() - 1);
    }
    // Native mixed recipes use 1-based project slots. The popup uses its
    // filtered dialog positions, so translate references without RGB lookup.
    if (options->allow_unverified_mixed_filaments) for (const auto& entry : entries) {
        if (entry.kind != TextureFilamentKind::ExistingMixed || !entry.compatible ||
            entry.mixed_components.size() < 2 || entry.mixed_components.size() != entry.mixed_ratios.size()) continue;
        auto mixed = entry;
        mixed.mixed_components.clear();
        bool valid = true;
        int sum = 0;
        std::set<unsigned> seen;
        for (size_t i = 0; i < entry.mixed_components.size(); ++i) {
            const unsigned slot = entry.mixed_components[i];
            const auto base = std::find_if(usable.begin(), usable.end(), [slot](const auto& candidate) {
                return candidate.kind == TextureFilamentKind::ExistingPhysical && slot &&
                    candidate.project_config_index == size_t(slot - 1);
            });
            if (base == usable.end() || !seen.insert(slot).second || entry.mixed_ratios[i] <= 0 ||
                entry.mixed_ratios[i] > 100) { valid = false; break; }
            mixed.mixed_components.push_back(unsigned(base - usable.begin() + 1));
            sum += entry.mixed_ratios[i];
        }
        if (!valid || sum != 100) continue;
        mixed.dialog_index = int(usable.size());
        usable.push_back(std::move(mixed));
    }
    return usable;
}


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
        require(found!=current.end() && found->compatible && saved.project_config_index<=max_slot,"耗材已变化，请回工作台重新匹配颜色。");
        allowed.insert(saved.project_config_index);
    }
    require(!options.matched_face_slots.empty(),"区域配色为空。");
    for(size_t slot:options.matched_face_slots)require(allowed.count(slot)!=0,"区域使用了不可用的耗材。");
    std::set<size_t> physical_slots;
    for (size_t slot : options.matched_face_slots) {
        const auto entry = std::find_if(options.matched_filaments.begin(), options.matched_filaments.end(),
            [slot](const auto& candidate) { return candidate.project_config_index == slot; });
        if (entry->kind == TextureFilamentKind::ExistingPhysical) physical_slots.insert(slot);
        else for (unsigned component : entry->mixed_components) {
            const auto base = std::find_if(current.begin(), current.end(), [component](const auto& candidate) {
                return component && candidate.project_config_index == size_t(component - 1) &&
                    candidate.kind == TextureFilamentKind::ExistingPhysical && candidate.compatible;
            });
            require(base != current.end(), "叠色配方引用了不可用的实体耗材。");
            physical_slots.insert(base->project_config_index);
        }
    }
    require(options.physical_filament_limit == 0 || physical_slots.size() <= options.physical_filament_limit,
        "区域配色使用的实体耗材超过喷头数量，请减少实体耗材或为额外颜色选择叠色配方。");
    TriangleSelector selector(volume->mesh());
    for(size_t f=0;f<options.matched_face_slots.size();++f)
        selector.set_facet(int(f),EnforcerBlockerType(int(EnforcerBlockerType::Extruder1)+int(options.matched_face_slots[f])));
    const int base=int(*std::min_element(options.matched_face_slots.begin(),options.matched_face_slots.end()))+1;
    volume->mmu_segmentation_facets.set(selector);volume->config.set("extruder",base);object->config.set("extruder",base);
}

} // namespace Slic3r::GUI
