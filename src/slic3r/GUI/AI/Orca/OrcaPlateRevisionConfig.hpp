#pragma once

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::GUI {

inline DynamicPrintConfig slice_input_plate_config(DynamicPrintConfig config, FilamentMapMode effective_mode)
{
    // Automatic maps are slice outputs, written back by BackgroundSlicingProcess.
    if (is_auto_filament_map_mode(effective_mode)) {
        config.erase("filament_map");
        config.erase("filament_volume_map");
    }
    if (effective_mode != fmmNozzleManual)
        config.erase("filament_nozzle_map");
    return config;
}

} // namespace Slic3r::GUI
