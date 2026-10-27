#pragma once
#include "project_support.hpp"
namespace orca::mobile::slicing {
inline constexpr const char* SMART_SLICE_ALGORITHM_VERSION = "smart-slicing-core/1";
Json smart_slice_plans(const LoadedProject& project, const Json& request);
Slic3r::DynamicPrintConfig smart_slice_config(const LoadedProject& project,
    const Json& request, const std::string& quality);
}
