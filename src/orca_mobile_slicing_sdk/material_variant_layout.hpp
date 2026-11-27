#ifndef ORCA_MOBILE_MATERIAL_VARIANT_LAYOUT_HPP
#define ORCA_MOBILE_MATERIAL_VARIANT_LAYOUT_HPP

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <stdexcept>
#include <string>
#include <set>
#include <sstream>

namespace orca::mobile::slicing::detail {

inline bool known_material_variant(const std::string& variant)
{
    using namespace Slic3r;
    for(auto type:{etDirectDrive,etBowden})
        for(auto volume:{nvtStandard,nvtHighFlow,nvtTPUHighFlow,nvtE3DHighFlow,nvtExtraHighFlow})
            if(variant==get_extruder_variant_string(type,volume)) return true;
    return false;
}

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
        if(!known_material_variant(variant)) throw std::runtime_error("COLOR_CONFIG_MATERIAL_VARIANT_UNKNOWN");
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

// All SDK slicing entries, including ordinary/manual generic projects, need an
// interpretable layout. This is a configuration check, not material authority.
// Do not let Core's UI fallback invent a missing material row at execution.
inline Slic3r::DynamicPrintConfig checked_execution_materials(const Slic3r::DynamicPrintConfig& input)
{
    using namespace Slic3r;
    const auto* colors=input.option<ConfigOptionStrings>("filament_colour");
    const auto* ids=input.option<ConfigOptionInts>("filament_self_index");
    const auto* variants=input.option<ConfigOptionStrings>("filament_extruder_variant");
    const auto* maps=input.option<ConfigOptionInts>("filament_map");
    const auto* nozzles=input.option<ConfigOptionFloats>("nozzle_diameter");
    if(!colors || colors->empty() || !ids || ids->empty() || !variants || variants->empty() ||
       !maps || maps->empty() || !nozzles || nozzles->empty())
        throw std::runtime_error("MATERIAL_EXECUTION_LAYOUT_REQUIRED");
    const size_t count=colors->size();
    // A one-material native profile may share the registered ID1 across its
    // variant table. This cannot supply a missing ID2 in a multi-material job.
    const size_t rows=count==1 && ids->size()==1 && ids->values.front()==1 ?
        std::max(ids->size(),variants->size()) : ids->size();
    if(count>32 || rows<count || (variants->size()!=1 && variants->size()!=rows) ||
       (maps->size()!=1 && maps->size()!=count))
        throw std::runtime_error("MATERIAL_EXECUTION_LAYOUT_INVALID");
    std::set<std::pair<int,std::string>> ownership;
    std::vector<bool> seen(count,false);
    for(size_t row=0;row<rows;++row){
        const int id=ids->values[ids->size()==1 ? 0 : row];
        const auto& variant=variants->values[variants->size()==1 ? 0 : row];
        if(id<1 || size_t(id)>count || !known_material_variant(variant) ||
           !ownership.emplace(id,variant).second)
            throw std::runtime_error("MATERIAL_EXECUTION_VARIANT_INVALID");
        seen[size_t(id-1)]=true;
    }
    for(bool exists:seen) if(!exists) throw std::runtime_error("MATERIAL_EXECUTION_LAYOUT_INCOMPLETE");

    DynamicPrintConfig output=input;
    std::vector<int> physical(count),selectedIds(count);
    std::vector<size_t> selectedRows(count);
    std::vector<std::string> selectedVariants(count);
    const auto* volumeMap=input.option<ConfigOptionInts>("filament_volume_map");
    if(volumeMap && !volumeMap->empty() && volumeMap->size()!=1 && volumeMap->size()!=count)
        throw std::runtime_error("MATERIAL_EXECUTION_VOLUME_MAP_INVALID");
    // Validate before any Core helper can use get_at() on a machine vector.
    const auto* machineVariants=input.option<ConfigOptionStrings>("extruder_variant_list");
    // This GUI/profile expansion metadata is optional in a complete SDK
    // config. The actual device enums below remain authoritative either way.
    if(machineVariants && (machineVariants->empty() ||
       (machineVariants->size()!=1 && machineVariants->size()!=nozzles->size())))
        throw std::runtime_error("MATERIAL_EXECUTION_MACHINE_VARIANT_INVALID");
    std::vector<std::set<std::string>> machineChoices(nozzles->size());
    for(size_t tool=0;tool<nozzles->size();++tool){
        (void)authorized_tool_variant(input,tool);
        if(!machineVariants) continue;
        std::istringstream text(machineVariants->values[machineVariants->size()==1 ? 0 : tool]);
        std::string variant;
        while(std::getline(text,variant,',')){
            const auto first=variant.find_first_not_of(" \t\r\n"),last=variant.find_last_not_of(" \t\r\n");
            if(first==std::string::npos) throw std::runtime_error("MATERIAL_EXECUTION_MACHINE_VARIANT_INVALID");
            variant=variant.substr(first,last-first+1);
            if(!known_material_variant(variant) || !machineChoices[tool].insert(variant).second)
                throw std::runtime_error("MATERIAL_EXECUTION_MACHINE_VARIANT_INVALID");
        }
        if(machineChoices[tool].empty()) throw std::runtime_error("MATERIAL_EXECUTION_MACHINE_VARIANT_INVALID");
    }
    const int extruderCount=int(nozzles->size());
    std::vector<std::vector<NozzleVolumeType>> nozzleVolumes;
    const int volumeCount=output.get_extruder_nozzle_volume_count(extruderCount,nozzleVolumes);
    for(size_t logical=0;logical<count;++logical){
        const int tool=maps->values[maps->size()==1 ? 0 : logical];
        if(tool<1 || size_t(tool)>nozzles->size())
            throw std::runtime_error("MATERIAL_EXECUTION_TOOL_MAP_INVALID");
        physical[logical]=tool;selectedIds[logical]=int(logical+1);
        auto machine=input;
        auto* volumes=dynamic_cast<ConfigOptionEnumsGeneric*>(machine.option("nozzle_volume_type"));
        if(!volumes || (volumes->size()!=1 && volumes->size()!=nozzles->size()))
            throw std::runtime_error("MATERIAL_EXECUTION_MACHINE_VARIANT_INVALID");
        if(volumeMap && !volumeMap->empty() &&
           (volumeCount>extruderCount || volumes->get_at(size_t(tool-1))==nvtHybrid)){
            const int volume=volumeMap->values[volumeMap->size()==1 ? 0 : logical];
            if(volume==nvtHybrid || (volume!=nvtStandard && volume!=nvtHighFlow && volume!=nvtTPUHighFlow &&
               volume!=nvtE3DHighFlow && volume!=nvtExtraHighFlow))
                throw std::runtime_error("MATERIAL_EXECUTION_VOLUME_MAP_INVALID");
            volumes->values.resize(nozzles->size(),volumes->values.front());
            volumes->values[size_t(tool-1)]=volume;
        }
        selectedVariants[logical]=authorized_tool_variant(machine,size_t(tool-1));
        if(machineVariants && !machineChoices[size_t(tool-1)].count(selectedVariants[logical]))
            throw std::runtime_error("MATERIAL_EXECUTION_MACHINE_VARIANT_UNSUPPORTED");
        size_t matches=0;
        for(size_t row=0;row<rows;++row)
            if(ids->values[ids->size()==1 ? 0 : row]==int(logical+1) &&
               variants->values[variants->size()==1 ? 0 : row]==selectedVariants[logical]){
                selectedRows[logical]=row;++matches;
            }
        if(matches!=1) throw std::runtime_error("MATERIAL_EXECUTION_VARIANT_UNSUPPORTED");
    }
    for(const auto& key:Preset::filament_options()){
        const auto* source=input.option(key);
        const auto* vector=dynamic_cast<const ConfigOptionVectorBase*>(source);
        if(!vector || vector->empty()) continue;
        // Development tables have their own per-filament record format. They
        // are preserved verbatim and checked unchanged after Print::apply.
        if(filament_dev_options.count(key) || key=="compatible_printers" || key=="compatible_prints") continue;
        const bool variant=filament_options_with_variant.count(key)!=0;
        if(vector->size()!=1 && vector->size()!=count && (!variant || vector->size()!=rows))
            throw std::runtime_error("MATERIAL_EXECUTION_OPTION_LAYOUT_INVALID:"+key);
        auto* target=dynamic_cast<ConfigOptionVectorBase*>(output.option(key));
        target->resize(count,source);
        for(size_t logical=0;logical<count;++logical){
            const size_t index=vector->size()==1 ? 0 :
                (variant && vector->size()==rows ? selectedRows[logical] : logical);
            target->set_at(source,logical,index);
        }
    }
    for(const char* key:{"nozzle_temperature","nozzle_temperature_initial_layer","filament_diameter",
                         "filament_flow_ratio","fan_min_speed","fan_max_speed"}){
        const auto* vector=dynamic_cast<const ConfigOptionVectorBase*>(output.option(key));
        if(!vector || vector->empty()) throw std::runtime_error(std::string("MATERIAL_EXECUTION_PARAMETER_REQUIRED:")+key);
    }
    output.set_key_value("filament_self_index",new ConfigOptionInts(selectedIds));
    output.set_key_value("filament_extruder_variant",new ConfigOptionStrings(selectedVariants));
    output.set_key_value("filament_map",new ConfigOptionInts(physical));
    if(volumeMap && volumeMap->size()==1)
        output.set_key_value("filament_volume_map",new ConfigOptionInts(std::vector<int>(count,volumeMap->values.front())));
    // This one material-vector normalization is explicitly defined by
    // normalize_fdm_1 for spiral printing; it does not change material identity.
    const auto* spiral=input.option<ConfigOptionBool>("spiral_mode");
    if(spiral && spiral->value){
        auto* retract=output.option<ConfigOptionBoolsNullable>("filament_retract_when_changing_layer");
        if(retract) retract->values.assign(count,false);
    }
    return output;
}

inline void verify_execution_materials(const Slic3r::DynamicPrintConfig& expected,
                                      const Slic3r::DynamicPrintConfig& effective)
{
    verify_authorized_materials(expected,effective);
    for(const char* key:{"filament_map","filament_volume_map"}){
        const auto* before=expected.option(key);const auto* after=effective.option(key);
        if(before && (!after || before->serialize()!=after->serialize()))
            throw std::runtime_error(std::string("MATERIAL_EXECUTION_ROUTING_CHANGED:")+key);
    }
}

} // namespace orca::mobile::slicing::detail
#endif
