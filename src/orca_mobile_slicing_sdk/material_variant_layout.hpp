#ifndef ORCA_MOBILE_MATERIAL_VARIANT_LAYOUT_HPP
#define ORCA_MOBILE_MATERIAL_VARIANT_LAYOUT_HPP

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <stdexcept>
#include <string>

namespace orca::mobile::slicing::detail {

// This boundary consumes a resolved native preset, never a client variant claim.
// The output has one row per logical material, rather than one row per variant.
inline size_t authorized_material_variant(const Slic3r::DynamicPrintConfig& material,
                                         const std::string& required)
{
    using namespace Slic3r;
    const auto* variants=material.option<ConfigOptionStrings>("filament_extruder_variant");
    const auto* ids=material.option<ConfigOptionInts>("filament_self_index");
    if(!variants || variants->values.empty() || !ids || ids->values.empty())
        throw std::runtime_error("COLOR_CONFIG_MATERIAL_VARIANT_REQUIRED");
    if(ids->values.size()!=1 && ids->values.size()!=variants->values.size())
        throw std::runtime_error("COLOR_CONFIG_MATERIAL_VARIANT_LAYOUT_INVALID");
    for(int id:ids->values) if(id!=1)
        throw std::runtime_error("COLOR_CONFIG_MATERIAL_VARIANT_LAYOUT_INVALID");
    size_t selected=0,matches=0;
    for(size_t row=0;row<variants->values.size();++row){
        const auto& variant=variants->values[row];
        bool known=false;
        for(auto type:{etDirectDrive,etBowden})
            for(auto volume:{nvtStandard,nvtHighFlow,nvtTPUHighFlow,nvtE3DHighFlow,nvtExtraHighFlow})
                known=known || variant==get_extruder_variant_string(type,volume);
        if(!known) throw std::runtime_error("COLOR_CONFIG_MATERIAL_VARIANT_UNKNOWN");
        if(variant==required){selected=row;++matches;}
    }
    if(matches!=1) throw std::runtime_error(matches==0 ?
        "COLOR_CONFIG_MATERIAL_VARIANT_UNSUPPORTED" : "COLOR_CONFIG_MATERIAL_VARIANT_AMBIGUOUS");
    return selected;
}

inline std::string authorized_tool_variant(const Slic3r::DynamicPrintConfig& machine,size_t tool)
{
    using namespace Slic3r;
    const auto* nozzles=machine.option<ConfigOptionFloats>("nozzle_diameter");
    const auto* types=dynamic_cast<const ConfigOptionEnumsGeneric*>(machine.option("extruder_type"));
    const auto* volumes=dynamic_cast<const ConfigOptionEnumsGeneric*>(machine.option("nozzle_volume_type"));
    if(!nozzles || tool>=nozzles->values.size() || !types || !volumes ||
       (types->values.size()!=1 && types->values.size()!=nozzles->values.size()) ||
       (volumes->values.size()!=1 && volumes->values.size()!=nozzles->values.size()))
        throw std::runtime_error("COLOR_CONFIG_DEVICE_VARIANT_LAYOUT_INVALID");
    const int type=types->values[types->values.size()==1 ? 0 : tool];
    int volume=volumes->values[volumes->values.size()==1 ? 0 : tool];
    // The producer assigns filament_volume_map=Standard (0). Core resolves a
    // Hybrid physical nozzle through that explicit map to the same variant.
    if(volume==nvtHybrid) volume=nvtStandard;
    if((type!=etDirectDrive && type!=etBowden) ||
       (volume!=nvtStandard && volume!=nvtHighFlow && volume!=nvtTPUHighFlow &&
        volume!=nvtE3DHighFlow && volume!=nvtExtraHighFlow))
        throw std::runtime_error("COLOR_CONFIG_DEVICE_VARIANT_UNKNOWN");
    return get_extruder_variant_string(ExtruderType(type),NozzleVolumeType(volume));
}

inline void flatten_authorized_material(Slic3r::DynamicPrintConfig& output,
                                       const Slic3r::DynamicPrintConfig& native,
                                       const std::string& required,size_t logical,size_t count)
{
    using namespace Slic3r;
    if(logical>=count) throw std::runtime_error("COLOR_CONFIG_MATERIAL_ROW_INVALID");
    const size_t row=authorized_material_variant(native,required);
    const size_t variantCount=native.option<ConfigOptionStrings>("filament_extruder_variant")->values.size();
    for(const auto& key:Preset::filament_options()){
        const auto* option=native.option(key);
        const auto* vector=dynamic_cast<const ConfigOptionVectorBase*>(option);
        if(!vector || vector->empty()) continue;
        const bool variant=filament_options_with_variant.count(key)!=0;
        // A registered/native singleton is shared by that preset's variants.
        // A non-variant value describes this one preset material, not its tool.
        if(vector->size()!=1 && (!variant || vector->size()!=variantCount))
            throw std::runtime_error("COLOR_CONFIG_MATERIAL_OPTION_LAYOUT_INVALID:"+key);
        auto* target=dynamic_cast<ConfigOptionVectorBase*>(output.option(key));
        if(!target) throw std::runtime_error("COLOR_CONFIG_MATERIAL_OPTION_MISSING:"+key);
        target->resize(count,option);
        target->set_at(option,logical,vector->size()==1 ? 0 : row);
    }
    auto* ids=output.option<ConfigOptionInts>("filament_self_index");
    if(!ids) throw std::runtime_error("COLOR_CONFIG_MATERIAL_VARIANT_REQUIRED");
    ids->values.resize(count);
    for(size_t i=0;i<count;++i) ids->values[i]=int(i+1);
}

inline void verify_authorized_materials(const Slic3r::DynamicPrintConfig& expected,
                                        const Slic3r::DynamicPrintConfig& effective)
{
    using namespace Slic3r;
    auto keys=Preset::filament_options();
    keys.push_back("filament_self_index");
    for(const auto& key:keys){
        const auto* before=expected.option(key);
        const auto* vector=dynamic_cast<const ConfigOptionVectorBase*>(before);
        if(!vector || vector->empty()) continue;
        const auto* after=effective.option(key);
        if(!after || before->serialize()!=after->serialize())
            throw std::runtime_error("ACTUAL_FILAMENT_NORMALIZATION_CHANGED:"+key);
    }
}

} // namespace orca::mobile::slicing::detail
#endif
