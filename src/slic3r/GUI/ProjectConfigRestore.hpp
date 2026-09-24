#pragma once

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "slic3r/Utils/ProjectConfigUndo.hpp"

namespace Slic3r::GUI::ProjectConfigRestore {

// Check a restored slot table against the installed presets before moving the
// model history. This validates native state, not physical print capabilities.
inline bool validate(const DynamicPrintConfig& config, const std::vector<std::string>& presets,
    const PresetBundle& installed, std::string& error)
{
    error.clear();
    const size_t count=presets.size();
    auto fail=[&](const char* text){error=text;return false;};
    if(count==0 || count>32) return fail("The restored material slot count is invalid.");
    for(const auto& name:presets) if(!installed.filaments.find_preset(name,false))
        return fail("A material preset required by this operation is no longer installed.");
    for(const char* key:{"filament_colour","filament_multi_colour","filament_colour_type",
        "filament_mixed_components","filament_mixed_sublayer_ratios","filament_mixed_gradient_range","filament_mixed_gradient_curve"}) {
        const auto* option=config.option<ConfigOptionStrings>(key);
        if(!option || option->values.size()!=count) return fail("The restored material string arrays are incomplete.");
    }
    for(const char* key:{"filament_is_mixed","filament_mixed_gradient","filament_mixed_gradient_per_part"}) {
        const auto* option=config.option<ConfigOptionBools>(key);
        if(!option || option->values.size()!=count) return fail("The restored material flag arrays are incomplete.");
    }
    for(const char* key:{"filament_map","filament_nozzle_map","filament_volume_map"}) {
        const auto* option=config.option<ConfigOptionInts>(key);
        if(!option || option->values.size()!=count) return fail("The restored material routing arrays are incomplete.");
    }
    const auto* multiplier=config.option<ConfigOptionFloats>("flush_multiplier");
    const auto* matrix=config.option<ConfigOptionFloats>("flush_volumes_matrix");
    const auto* volumes=config.option<ConfigOptionFloats>("flush_volumes_vector");
    const size_t nozzles=installed.get_printer_extruder_count();
    if(!multiplier || !matrix || !volumes || nozzles==0 || count<nozzles ||
        multiplier->values.size()!=nozzles || matrix->values.size()!=count*count*nozzles || volumes->values.size()!=count*2)
        return fail("The restored purge settings do not match the printer and material slots.");
    if(!validate_mixed_filament_params(config.option<ConfigOptionBools>("filament_is_mixed")->values,
        config.option<ConfigOptionStrings>("filament_mixed_components")->values,
        config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values,
        config.option<ConfigOptionBools>("filament_mixed_gradient")->values,
        config.option<ConfigOptionStrings>("filament_mixed_gradient_range")->values,
        config.option<ConfigOptionStrings>("filament_mixed_gradient_curve")->values).empty())
        return fail("The restored mixed material definitions are invalid.");
    return true;
}

// Both swaps are allocation-free. After the model jump succeeds, commit both
// project arrays and selected names before any UI callbacks can observe them.
using ColorCache=std::vector<std::vector<std::string>>;
inline ColorCache prepare_cache(size_t count, const PresetBundle& live)
{
    auto cache=live.ams_multi_color_filment;
    cache.resize(count);
    return cache;
}

inline void commit(UndoRedo::ProjectConfigUndo::Prepared& prepared, ColorCache& cache, PresetBundle& live) noexcept
{
    if(!prepared.changed) return;
    prepared.config.swap(live.project_config);
    prepared.filament_presets.swap(live.filament_presets);
    cache.swap(live.ams_multi_color_filment);
    prepared.changed=false;
}
} // namespace Slic3r::GUI::ProjectConfigRestore
