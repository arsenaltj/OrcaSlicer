#pragma once

#include "WorkbenchArtifactValidation.hpp"
#include "PortraitNativePartition.hpp"
#include <boost/filesystem/path.hpp>
#include <boost/nowide/convert.hpp>
#include <algorithm>
#include <type_traits>

namespace Slic3r::GUI {

// Only a failed publication retains these already-owned immutable inputs and
// finished partition. First success does not copy a mesh or write a checkpoint.
// Retrying still decodes the real source, including external OBJ dependencies.
struct NativeWorkbenchPreparedPartition {
    std::shared_ptr<const TexturedMesh> source;
    std::shared_ptr<NativePortrait::NativeWorkbenchPartition> partition;
    std::shared_ptr<const NativePortrait::NativePortraitPartitionBuildReport> report;
    NativePortrait::NativeMillimeterBounds source_bounds;
    std::uint64_t edit_revision {0};

    bool matches(const TexturedMesh& actual, const std::function<bool()>& canceled = {}) const {
        if (canceled && canceled()) return false;
        if (!source || !partition || !report || !source_bounds.valid() ||
            !report->strict_repair.accepted() || !partition->valid() ||
            report->stage(NativePortrait::BuildStage::RegionEnumeration).status !=
                NativePortrait::BuildStageStatus::Succeeded) return false;
        const auto same = [&](const auto& left, const auto& right) {
            if (left.size() != right.size()) return false;
            using Item = typename std::decay_t<decltype(left)>::value_type;
            constexpr size_t block = std::max(size_t(1), size_t(MeshResourceBudget::block_bytes / sizeof(Item)));
            for (size_t first = 0; first < left.size();) {
                if (canceled && canceled()) return false;
                const size_t end = first + std::min(block, left.size() - first);
                if (!std::equal(left.begin() + first, left.begin() + end, right.begin() + first)) return false;
                first = end;
            }
            return true;
        };
        if (!same(source->vertices, actual.vertices) || !same(source->indices, actual.indices) ||
            !same(source->uvs, actual.uvs) || !same(source->material_ids, actual.material_ids) ||
            !same(source->material_texture_map, actual.material_texture_map) ||
            !same(source->material_colors, actual.material_colors) ||
            !same(source->uv_coords, actual.uv_coords) || !same(source->uv_indices, actual.uv_indices) ||
            !same(source->precomputed_face_colors, actual.precomputed_face_colors) ||
            !same(source->precomputed_vertex_colors, actual.precomputed_vertex_colors) ||
            source->textures.size() != actual.textures.size()) return false;
        for (size_t i = 0; i < source->textures.size(); ++i) {
            if (canceled && canceled()) return false;
            const auto& before = source->textures[i]; const auto& after = actual.textures[i];
            if (before.width != after.width || before.height != after.height ||
                before.channels != after.channels || !same(before.data, after.data)) return false;
        }
        return !canceled || !canceled();
    }

    bool restore_build_report(const TexturedMesh& actual,
        NativePortrait::NativePortraitPartitionBuildReport& attempt,
        const std::function<bool()>& canceled = {}) const {
        if (!matches(actual, canceled)) return false;
        const auto verified_source = attempt.stage(NativePortrait::BuildStage::SourceCheck);
        const auto decoded_source = attempt.stage(NativePortrait::BuildStage::Decode);
        attempt = *report;
        attempt.success = true; attempt.error.clear(); attempt.total_ms = 0;
        for (auto& stage : attempt.stages) {
            stage = {};
            stage.status = NativePortrait::BuildStageStatus::Skipped;
            stage.detail = "Reused completed in-memory partition; strict repair facts describe its original execution.";
        }
        attempt.stage(NativePortrait::BuildStage::SourceCheck) = verified_source;
        attempt.stage(NativePortrait::BuildStage::Decode) = decoded_source;
        return true;
    }
};

inline bool portrait_partition_metadata_matches(const nlohmann::json& metadata,
                                         const boost::filesystem::path& metadata_path,
                                         const boost::filesystem::path& model_path,
                                         const std::string& model_sha256,
                                         const std::string& geometry_id,
                                         const std::string& appearance_id,
                                         bool require_post_color,
                                         nlohmann::json& reference)
{
    try {
        if (metadata.at("model_sha256") != model_sha256) return false;
        reference = metadata.at("native_portrait_partition");
        if (!WorkbenchArtifacts::valid_native_partition_reference(reference) ||
            reference.at("carrier_sha256") != model_sha256 ||
            !WorkbenchArtifacts::same_model_path(reference.at("carrier_path").get<std::string>(), boost::nowide::narrow(model_path.wstring())) ||
            !WorkbenchArtifacts::same_model_path(reference.at("metadata_path").get<std::string>(), boost::nowide::narrow(metadata_path.wstring())) ||
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
            topology.value("native_repair_compatible", false) != reference.value("native_repair_compatible", false) ||
            !topology.at("topology").is_object()) return false;
        if (topology.contains("native_acceptance")) {
            if (!native_repair_topology_allowed(metadata.at("partition_build_report").at("strict_repair"), topology) ||
                !native_repair_geometry_allowed(metadata.at("partition_build_report").at("strict_repair"), topology) ||
                topology.at("native_acceptance") != reference.at("native_acceptance")) return false;
        } else {
            // Preserve valid v12 checkpoints, including older documents which
            // predate the stage report. They still require every strict fact.
            if (topology.at("schema") != "orca.native-workbench-topology-certificate/v1") return false;
            const auto& facts = topology.at("topology");
            for (const char* key : {"open_edges", "nonmanifold_edges", "nonmanifold_vertices",
                    "degenerate_faces", "duplicate_faces", "inconsistent_edges"})
                if (facts.at(key).get<size_t>() != 0) return false;
            if (!facts.at("self_intersections_checked").get<bool>() || facts.at("self_intersects").get<bool>()) return false;
        }
        if (!require_post_color)
            return metadata.at("schema") == "orca.native-workbench-partition-commit/v1" &&
                metadata.at("transaction_status") == "committed";
        const auto& semantics = metadata.at("region_semantics");
        const auto& reduction = metadata.at("native_color_reduction");
        const auto& slots = metadata.at("final_slot_binding");
        const size_t region_count = semantics.at("regions").size();
        return WorkbenchArtifacts::valid_region_semantics(semantics,
                   reference.at("partition_digest").get<std::string>()) &&
            WorkbenchArtifacts::valid_native_color_reduction(reduction,
                reference.at("partition_digest").get<std::string>(), region_count) &&
            WorkbenchArtifacts::valid_final_slot_binding(slots,
                reference.at("partition_digest").get<std::string>(),
                reduction.at("mapping_digest").get<std::string>(), region_count) &&
            WorkbenchArtifacts::valid_post_color_check(metadata.at("post_color_check"), reference);
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
                model_sha256, geometry_id, appearance_id, saved, reference)) {
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
