#pragma once

#include "PortraitPrintFlow.hpp"
#include <boost/filesystem/path.hpp>
#include <boost/nowide/convert.hpp>

namespace Slic3r::GUI {

inline bool portrait_partition_metadata_matches(const nlohmann::json& metadata,
                                         const boost::filesystem::path& metadata_path,
                                         const boost::filesystem::path& model_path,
                                         const std::string& model_sha256,
                                         const std::string& geometry_id,
                                         const std::string& appearance_id,
                                         bool require_post_color,
                                         const std::string& flow_id,
                                         nlohmann::json& reference)
{
    try {
        if (metadata.at("model_sha256") != model_sha256 ||
            (!flow_id.empty() && metadata.at("portrait_print_id") != flow_id)) return false;
        reference = metadata.at("native_portrait_partition");
        if (!PortraitPrint::valid_native_partition_reference(reference) ||
            reference.at("carrier_sha256") != model_sha256 ||
            !PortraitPrint::same_model_path(reference.at("carrier_path").get<std::string>(), boost::nowide::narrow(model_path.wstring())) ||
            !PortraitPrint::same_model_path(reference.at("metadata_path").get<std::string>(), boost::nowide::narrow(metadata_path.wstring())) ||
            reference.at("geometry_id") != geometry_id ||
            reference.at("appearance_id") != appearance_id) return false;
        const auto& topology = metadata.at("native_portrait_topology_certificate");
        if (!topology.is_object() ||
            AI::beauty_leaf_digest(topology.dump()) != reference.at("topology_hash") ||
            topology.at("carrier_sha256") != model_sha256 ||
            topology.at("geometry_id") != geometry_id ||
            topology.at("partition_digest") != reference.at("partition_digest") ||
            topology.at("coordinate_units") != reference.at("coordinate_units") ||
            topology.at("source_bounds_mm") != reference.at("source_bounds_mm") ||
            topology.at("carrier_bounds_mm") != reference.at("carrier_bounds_mm") ||
            !topology.at("topology").is_object()) return false;
        const auto& topology_report = topology.at("topology");
        if (topology_report.at("open_edges").get<size_t>() != 0 ||
            topology_report.at("nonmanifold_edges").get<size_t>() != 0 ||
            topology_report.at("nonmanifold_vertices").get<size_t>() != 0 ||
            topology_report.at("degenerate_faces").get<size_t>() != 0 ||
            topology_report.at("duplicate_faces").get<size_t>() != 0 ||
            topology_report.at("inconsistent_edges").get<size_t>() != 0 ||
            !topology_report.at("self_intersections_checked").get<bool>() ||
            topology_report.at("self_intersects").get<bool>()) return false;
        if (!require_post_color)
            return metadata.at("schema") == "orca.native-workbench-partition-commit/v1" &&
                metadata.at("transaction_status") == "committed";
        const auto& semantics = metadata.at("region_semantics");
        const auto& reduction = metadata.at("native_color_reduction");
        const auto& slots = metadata.at("final_slot_binding");
        const size_t region_count = semantics.at("regions").size();
        return PortraitPrint::valid_region_semantics(semantics,
                   reference.at("partition_digest").get<std::string>()) &&
            PortraitPrint::valid_native_color_reduction(reduction,
                reference.at("partition_digest").get<std::string>(), region_count) &&
            PortraitPrint::valid_final_slot_binding(slots,
                reference.at("partition_digest").get<std::string>(),
                reduction.at("mapping_digest").get<std::string>(), region_count) &&
            PortraitPrint::valid_post_color_check(metadata.at("post_color_check"), reference);
    } catch (const std::exception&) {
        reference = nullptr;
        return false;
    }
}

// Reopening a saved version must validate its final coloring contracts; it is
// no longer the original partition transaction document. Unknown formats may
// not acquire a native checkpoint through this restoration entry point.
inline bool restorable_native_partition_metadata_matches(const nlohmann::json& metadata,
    const boost::filesystem::path& metadata_path, const boost::filesystem::path& model_path,
    const std::string& model_sha256, const std::string& geometry_id,
    const std::string& appearance_id, nlohmann::json& reference)
{
    reference = nullptr;
    try {
        const bool commit = metadata.value("schema", std::string{}) ==
                "orca.native-workbench-partition-commit/v1" &&
            metadata.value("schema_version", 0) == 6 &&
            metadata.value("source", std::string{}) == "native_workbench_partition";
        const bool saved = !metadata.contains("schema") &&
            metadata.value("schema_version", 0) == 4 &&
            metadata.value("source", std::string{}) == "local_finishing";
        if (!commit && !saved) return false;
        if (!portrait_partition_metadata_matches(metadata, metadata_path, model_path,
                model_sha256, geometry_id, appearance_id, saved, {}, reference)) {
            reference = nullptr;
            return false;
        }
        return true;
    } catch (const std::exception&) {
        reference = nullptr;
        return false;
    }
}

} // namespace Slic3r::GUI
