#ifndef ORCA_SLICING_PROJECT_SUPPORT_HPP
#define ORCA_SLICING_PROJECT_SUPPORT_HPP

#include <string>
#include <nlohmann/json.hpp>
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r { struct GCodeProcessorResult; struct StringObjectException; }

namespace orca::mobile::slicing {

using Json = nlohmann::json;

struct LoadedProject {
    Slic3r::Model model;
    Slic3r::DynamicPrintConfig config;
    std::string inputPath;
    std::string inputSha256;
    // Source identity, transforms, palette and filament/nozzle mapping; no mesh copy.
    Json metadata = Json::object();
    // Original profile before overrides/commands; saved as a separate read-only
    // JSON sidecar so catalog reads never need to parse a model under worker load.
    Json sourceConfig = Json::object();
};

// Read-only archive feature/plate inspection; original ZIP is never rewritten.
Json inspect_three_mf(const std::string& inputPath);

// Paths are UTF-8. Overrides are a JSON object of native option values (scalars or
// arrays); native serialized strings are accepted. Throws std::exception on failure.
// Transforms: [] or [{objectIndex, instanceIndex (optional), translation:[x,y,z],
// rotation:[x,y,z] (radians), scale:[x,y,z]}]. Components replace instance values;
// omitted components retain the source transform. Geometry and paint stay intact.
LoadedProject load_project(const std::string& inputPath,
                           const Json& configOverrides = Json::object(),
                           const Json& transforms = Json::array(),
                           const std::string& workspaceRoot = {},
                           const Json& projectCommands = Json::array());

// Replay against the unchanged original, preserving deterministic object/instance IDs.
void apply_project_commands(LoadedProject& project, const Json& commands);
void apply_config_overrides(Slic3r::DynamicPrintConfig& config, const Json& overrides);
std::string file_sha256(const std::string& path);
std::string json_sha256(const Json& value);
// A source-preserving copy for Print; explicit material assignments override paint
// only on this copy, never on the source scene or original input.
Slic3r::Model slicing_model(const LoadedProject& project);
void validate_output_capabilities(const LoadedProject& project);
// Full Print/GCodeProcessor logical IDs, including support and custom tool routing.
void validate_material_plan(const LoadedProject& project, const std::vector<unsigned int>& logicalIds);
// Reject whole/partial XY overflow, exclusion intersection, below-bed and height overflow.
void validate_project_bounds(const LoadedProject& project);
// Native-derived v2 project fragment; caller supplies projectId/revision/scenePath.
Json describe_project(const LoadedProject& project);

// Pure immutable compiled catalog; no model load, temp dir or worker lock.
Json offline_presets();
Json project_presets(const Json& request);
Json describe_warning(const LoadedProject& project, const Slic3r::StringObjectException& warning);
void apply_preset(LoadedProject& project, const std::string& kind, const std::string& presetId);
// Mutates only the caller's temporary LoadedProject, never the persisted source.
Json orientation_preview(LoadedProject& temporary, const Json& request);
Json print_volume(const Slic3r::DynamicPrintConfig& config);
Json filament_cost(const Slic3r::DynamicPrintConfig& config, const Json& volumePerFilament,
                   const std::string& currency = {});
void validate_material_assignments(const LoadedProject& project);

// ORCASC01, little endian, transformed mm geometry. Returns scenePath, counts,
// bounds, colors and flags. Source Model and annotations are never modified.
Json write_scene(const LoadedProject& project, const std::string& outputPath);

// ORCAPV01 from the same native processor result used for statistics.
Json write_preview(const Slic3r::GCodeProcessorResult& gcode, const std::string& path);

// Copy base and apply a nozzle-aware fixed preset. smart requires a frozen config
// and is deliberately rejected here; never falls back to standard.
Slic3r::DynamicPrintConfig quality_config(const Slic3r::DynamicPrintConfig& base,
                                        const std::string& quality);

// Native option serialization, for durable JSON and replay through load_project.
Json config_json(const Slic3r::DynamicPrintConfig& config);

} // namespace orca::mobile::slicing
#endif
