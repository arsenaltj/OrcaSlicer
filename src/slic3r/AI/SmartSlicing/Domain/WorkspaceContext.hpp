#pragma once

#include "WorkspaceRevision.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

struct WorkspaceObjectSnapshot
{
    uint64_t object_id{0};
    std::string name;
    size_t instance_count{0};
    size_t facet_count{0};
    size_t open_edge_count{0};
    bool outside_build_volume{false};
};

struct MaterialSnapshot
{
    std::string preset_id;
    std::string color;
};

struct WorkspaceContext
{
    WorkspaceRevision revision;
    int plate_index{-1};
    std::string printer_preset_id;
    std::string process_preset_id;
    std::string bed_type;
    std::vector<double> nozzle_diameters;
    std::vector<MaterialSnapshot> materials;
    std::vector<WorkspaceObjectSnapshot> objects;
    bool native_validation_available{false};
    std::vector<std::string> validation_errors;
    std::vector<std::string> validation_warnings;
};

} // namespace Slic3r::AI::SmartSlicing
