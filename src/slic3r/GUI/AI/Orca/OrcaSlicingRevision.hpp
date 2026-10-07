#pragma once

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::GUI {

inline DynamicPrintConfig slicing_revision_plate_config(const DynamicPrintConfig& config, FilamentMapMode mode)
{
    DynamicPrintConfig inputs = config;
    // BackgroundSlicingProcess writes these results back after a successful
    // slice. They are outputs only in these modes; manual assignments remain
    // inputs and must still invalidate candidates and guarded undo.
    if (is_auto_filament_map_mode(mode)) {
        inputs.erase("filament_map");
        inputs.erase("filament_volume_map");
    }
    if (mode != fmmNozzleManual && mode != fmmDefault)
        inputs.erase("filament_nozzle_map");
    return inputs;
}

} // namespace Slic3r::GUI
