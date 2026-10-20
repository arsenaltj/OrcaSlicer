#ifndef ORCA_SLICING_PROJECT_SUPPORT_HPP
#define ORCA_SLICING_PROJECT_SUPPORT_HPP

#include <string>
#include <nlohmann/json.hpp>
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace orca::slicing {

using Json = nlohmann::json;

struct LoadedProject {
    Slic3r::Model model;
    Slic3r::DynamicPrintConfig config;
    std::string inputPath;
    std::string inputSha256;
    // Source identity, transforms, palette and filament/nozzle mapping; no mesh copy.
    Json metadata = Json::object();
};

// Paths are UTF-8. Overrides are a JSON object of native option values (scalars or
// arrays); native serialized strings are accepted. Throws std::exception on failure.
// Transforms: [] or [{objectIndex, instanceIndex (optional), translation:[x,y,z],
// rotation:[x,y,z] (degrees), scale:[x,y,z]}]. Components replace instance values;
// omitted components retain the source transform. Geometry and paint stay intact.
LoadedProject load_project(const std::string& inputPath,
                           const Json& configOverrides = Json::object(),
                           const Json& transforms = Json::array(),
                           const std::string& workspaceRoot = {});

// Project fragment per CONTRACT-v1; caller supplies projectId/revision/scenePath.
Json describe_project(const LoadedProject& project);

// ORCASC01, little endian, transformed mm geometry. Returns scenePath, counts,
// bounds, colors and flags. Source Model and annotations are never modified.
Json write_scene(const LoadedProject& project, const std::string& outputPath);

// Copy base and apply a bounded nozzle-aware quality layer height. smart uses
// standard as the starting point; its trial planning is in recommendation_support.
Slic3r::DynamicPrintConfig quality_config(const Slic3r::DynamicPrintConfig& base,
                                        const std::string& quality);

// Native option serialization, for durable JSON and replay through load_project.
Json config_json(const Slic3r::DynamicPrintConfig& config);

} // namespace orca::slicing
#endif
