#pragma once

#include "PrintConfig.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace Slic3r::HonorForgeToolRouting {

// Exact resolved repository template. A different vendor template requires a
// reviewed adapter, rather than a broad replacement of arbitrary custom G-code.
inline const std::string& source_start_template()
{
    static const std::string value = "START_PRINT EXTRUDER=[nozzle_temperature_initial_layer] BED=[bed_temperature_initial_layer_single] CHAMBER=[chamber_temperature] INITIAL_TOOL=[initial_tool]  MESH_MIN_X={adaptive_bed_mesh_min[0]}  MESH_MIN_Y={adaptive_bed_mesh_min[1]} MESH_MAX_X={adaptive_bed_mesh_max[0]} MESH_MAX_Y={adaptive_bed_mesh_max[1]} PROBE_COUNT_X={bed_mesh_probe_count[0]} PROBE_COUNT_Y={bed_mesh_probe_count[1]}\n";
    return value;
}

inline const std::string& mapped_start_template()
{
    static const std::string value = [] {
        auto text = source_start_template();
        const std::string token = "INITIAL_TOOL=[initial_tool]";
        text.replace(text.find(token), token.size(), "INITIAL_TOOL={filament_map[initial_tool]-1}");
        return text;
    }();
    return value;
}

inline bool enabled(const ConfigBase& config)
{
    const auto* model=config.option<ConfigOptionString>("printer_model");
    const auto* start=config.option<ConfigOptionString>("machine_start_gcode");
    const auto* flavor=config.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor");
    const auto* semm=config.option<ConfigOptionBool>("single_extruder_multi_material");
    return model && model->value=="Honor Forge" && start && start->value==mapped_start_template() &&
           flavor && flavor->value==gcfKlipper && semm && !semm->value;
}

inline void validate(const std::vector<int>& map)
{
    std::set<int> seen;
    if(map.empty())throw std::runtime_error("HONOR_TOOL_MAPPING_REQUIRED");
    for(const auto tool:map)
        if(tool<1 || tool>32 || !seen.insert(tool).second)
            throw std::runtime_error("HONOR_TOOL_MAPPING_AMBIGUOUS");
}

inline unsigned physical(const std::vector<int>& map,unsigned logical)
{
    validate(map);
    if(logical>=map.size())throw std::runtime_error("HONOR_LOGICAL_MATERIAL_UNMAPPED");
    return unsigned(map[logical]-1);
}

inline unsigned logical(const std::vector<int>& map,unsigned physical)
{
    validate(map);
    const auto found=std::find(map.begin(),map.end(),int(physical)+1);
    if(found==map.end())throw std::runtime_error("HONOR_PHYSICAL_TOOL_UNMAPPED");
    return unsigned(std::distance(map.begin(),found));
}

inline unsigned logical_from_zero_based(const std::vector<int>& map,unsigned physical)
{
    auto oneBased=map;
    for(auto& tool:oneBased)++tool;
    return logical(oneBased,physical);
}

inline unsigned physical_from_zero_based(const std::vector<int>& map,unsigned logical)
{
    auto oneBased=map;
    for(auto& tool:oneBased)++tool;
    return physical(oneBased,logical);
}

} // namespace Slic3r::HonorForgeToolRouting
