#pragma once

#include "slic3r/GUI/AI/ModelGeneration/WorkbenchModelInspection.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitNativePartitionCache.hpp"

#include <boost/nowide/convert.hpp>

namespace Slic3r::GUI {

namespace NativeImportTopologyDetail {

inline void require(bool valid, const char* message)
{
    if (!valid) throw std::runtime_error(message);
}

inline boost::filesystem::path path8(const std::string& path)
{
    return boost::filesystem::path(boost::nowide::widen(path));
}

inline std::string bounded_metadata_bytes(const boost::filesystem::path& path)
{
    boost::system::error_code error;
    const auto bytes = boost::filesystem::file_size(path, error);
    require(!error && bytes != 0 && bytes <= 128ULL * 1024 * 1024 &&
        !boost::filesystem::is_symlink(path, error) && !error &&
        boost::filesystem::is_regular_file(path, error) && !error,
        "Native import metadata is unavailable.");
    boost::filesystem::ifstream input(path, std::ios::binary);
    std::string contents(size_t(bytes), '\0');
    require(bool(input.read(contents.data(), std::streamsize(contents.size()))) &&
        input.peek() == std::char_traits<char>::eof(), "Native import metadata changed while reading.");
    return contents;
}

inline const nlohmann::json* claimed_proof(const AI::ModelImportRequest& request)
{
    const nlohmann::json* result = nullptr;
    for (const auto& cache : request.native_portrait_partition_references) {
        if (!cache.is_object() || !cache.contains("import_topology_proof")) continue;
        // Keep any malformed or duplicate claim claimed, so it cannot fall
        // through to an ordinary import or an explicit risk exception.
        if (result) throw std::runtime_error("Duplicate native import topology claims.");
        result = &cache.at("import_topology_proof");
    }
    return result;
}

inline void load_verified_native_mesh(const AI::ModelImportRequest& request,
    const nlohmann::json& proof, TriangleMesh& exact_mesh,
    const std::function<bool()>& canceled)
{
    const auto checkpoint = [&] {
        if (canceled && canceled()) throw std::runtime_error("Native import verification canceled.");
    };
    checkpoint();
    require(proof.is_object() && proof.at("schema") == "orca.native-import-topology-proof/v1",
        "Unsupported native import topology proof.");
    auto unhashed = proof;
    const auto proof_sha256 = unhashed.at("proof_sha256").get<std::string>();
    unhashed.erase("proof_sha256");
    require(WorkbenchArtifacts::valid_sha256(proof_sha256) &&
        AI::beauty_leaf_digest(unhashed.dump()) == proof_sha256,
        "Native import topology proof changed.");
    const auto& reference = proof.at("native_reference");
    require(WorkbenchArtifacts::native_partition_has_exact_mesh(reference),
        "Native import cache has no certified original indices; rebuild from the immutable source.");
    const auto& cache_reference = reference.at("cache_reference");
    const auto carrier_sha256 = reference.at("carrier_sha256").get<std::string>();
    const auto geometry_id = reference.at("geometry_id").get<std::string>();
    const auto appearance_id = reference.at("appearance_id").get<std::string>();
    const auto partition_digest = reference.at("partition_digest").get<std::string>();
    const auto topology_hash = reference.at("topology_hash").get<std::string>();
    const auto face_count = cache_reference.at("face_count").get<size_t>();
    require(request.color_mode == AI::ImportColorMode::NativeMatch &&
        !request.native_repair_before_slice && !request.color_trial &&
        request.face_color_overrides.empty() && request.subface_color_overrides.empty() &&
        request.face_color_geometry_id == geometry_id &&
        request.topology_check_sha256 == carrier_sha256 &&
        request.matched_colors && request.matched_colors->valid() &&
        request.matched_colors->source_sha256 == carrier_sha256 &&
        request.matched_colors->geometry_id == geometry_id &&
        request.matched_colors->face_slots.size() == face_count &&
        request.matched_colors->subface_slots.empty() &&
        AI::beauty_leaf_digest(AI::encode_model_matched_colors(*request.matched_colors).dump()) ==
            proof.at("matched_colors_sha256"),
        "Native import has changed geometry, material identity, manual edits or subfaces.");
    size_t matching_caches = 0;
    for (auto cache : request.native_portrait_partition_references) {
        if (cache.is_object()) cache.erase("import_topology_proof");
        if (cache == cache_reference) ++matching_caches;
    }
    require(matching_caches == 1, "Native import cache reference changed.");

    const auto verify_files = [&] {
        checkpoint();
        require(AI::model_artifact_sha256(request.artifact.local_path) == carrier_sha256,
            "Native import artifact identity changed.");
        for (const auto& keys : {std::pair{"carrier_path", "carrier_sha256"},
                std::pair{"source_path", "source_sha256"},
                std::pair{"appearance_path", "appearance_sha256"},
                std::pair{"cache_path", "cache_sha256"}}) {
            const auto file = path8(reference.at(keys.first).get<std::string>());
            require(boost::filesystem::is_regular_file(file) && !boost::filesystem::is_symlink(file) &&
                AI::model_artifact_sha256(file) == reference.at(keys.second).get<std::string>(),
                "Native import source, carrier, appearance or cache identity changed.");
            checkpoint();
        }
    };
    verify_files();
    const auto metadata_path = path8(reference.at("metadata_path").get<std::string>());
    const auto metadata_bytes = bounded_metadata_bytes(metadata_path);
    require(AI::beauty_leaf_digest(metadata_bytes) == proof.at("metadata_file_sha256"),
        "Native import metadata identity changed.");
    const auto metadata = nlohmann::json::parse(metadata_bytes);
    const auto local_root = request.artifact.local_path.parent_path().filename() == "downloads"
        ? request.artifact.local_path.parent_path().parent_path() : request.artifact.local_path.parent_path();
    const auto evidence = workbench_history_evidence_location(request.artifact.local_path,
        AI::beauty_metadata_path(request.artifact.local_path, local_root), metadata, canceled);
    require(WorkbenchArtifacts::same_model_path(boost::nowide::narrow(evidence.model.wstring()),
        reference.at("carrier_path").get<std::string>()),
        "Native import artifact differs from the owned carrier.");
    require(metadata.at("native_portrait_partition") == reference &&
        metadata.at("workbench_check_certificate") == proof.at("workbench_certificate") &&
        metadata.at("model_sha256") == carrier_sha256,
        "Native import metadata differs from the owned accepted state.");
    // Both the committed native checkpoint and the saved accepted version use
    // this exact read-only portrait certificate. General import is ineligible.
    const auto& certificate = proof.at("workbench_certificate");
    WorkbenchCheckResult checked;
    require(WorkbenchArtifacts::valid_workbench_check_certificate(certificate,
            WorkbenchCheckPurpose::PortraitPostColorReadOnly, carrier_sha256,
            geometry_id, appearance_id, reference.at("metadata_path").get<std::string>(),
            metadata, partition_digest, topology_hash) &&
        WorkbenchArtifacts::decode_workbench_check_certificate(certificate, checked) &&
        workbench_topology_safe(checked) && checked.before_faces == face_count,
        "Native import workbench certificate mismatch.");

    const auto& native = metadata.at("native_portrait_topology_certificate");
    require(native.at("schema") == "orca.native-workbench-topology-certificate/v1" &&
        AI::beauty_leaf_digest(native.dump()) == topology_hash &&
        native.at("carrier_sha256") == carrier_sha256 &&
        native.at("geometry_id") == geometry_id &&
        native.at("partition_digest") == partition_digest &&
        native.at("native_mesh_sha256") == cache_reference.at("native_mesh_sha256") &&
        native.at("coordinate_units") == reference.at("coordinate_units") &&
        native.at("source_bounds_mm") == reference.at("source_bounds_mm") &&
        native.at("carrier_bounds_mm") == reference.at("carrier_bounds_mm"),
        "Native import strict topology identity mismatch.");
    const auto& report = metadata.at("partition_build_report");
    const auto& strict = report.at("strict_repair");
    require(report.at("schema") == "orca.native-workbench-partition-build-report/v1" &&
        report.at("success").get<bool>() && strict.at("success").get<bool>() &&
        !strict.at("canceled").get<bool>() && strict.at("geometry_verified").get<bool>() &&
        strict.at("output_topology") == native.at("topology"),
        "Native import strict repair report mismatch.");
    const auto deviation = strict.at("sampled_deviation_ratio").get<double>();
    const auto area_change = strict.at("area_change_ratio").get<double>();
    const StrictMeshRepairOptions limits;
    require(std::isfinite(deviation) && deviation >= 0.0 &&
        deviation <= limits.maximum_sampled_deviation_ratio &&
        std::isfinite(area_change) && area_change >= 0.0 &&
        area_change <= limits.maximum_area_change_ratio &&
        native.at("sampled_deviation_ratio").get<double>() == deviation &&
        native.at("area_change_ratio").get<double>() == area_change,
        "Native import strict geometry report mismatch.");
    const auto& topology = native.at("topology");
    require(checked.boundary_edges == topology.at("open_edges").get<size_t>() &&
        checked.nonmanifold_edges == topology.at("nonmanifold_edges").get<size_t>() &&
        checked.nonmanifold_vertices == topology.at("nonmanifold_vertices").get<size_t>() &&
        checked.degenerate_faces == topology.at("degenerate_faces").get<size_t>() &&
        checked.duplicate_faces == topology.at("duplicate_faces").get<size_t>() &&
        checked.inconsistent_edges == topology.at("inconsistent_edges").get<size_t>() &&
        checked.self_intersections_checked == topology.at("self_intersections_checked").get<bool>() &&
        (checked.self_intersections != 0) == topology.at("self_intersects").get<bool>(),
        "Native import topology reports disagree.");

    const auto cache_path = path8(reference.at("cache_path").get<std::string>());
    require(cache_path.filename().string() == cache_reference.at("content_id").get<std::string>() + ".json",
        "Native import cache path differs from its content identity.");
    NativePortrait::NativePortraitPartitionIdentity identity;
    std::string error;
    require(NativePortrait::load_native_portrait_partition_cache(
        cache_reference, cache_path.parent_path(), identity, error, &exact_mesh),
        "Native import exact mesh payload is missing, corrupt or open.");
    require(identity.geometry_id == geometry_id && identity.partition_digest == partition_digest &&
        checked.before_vertices == exact_mesh.its.vertices.size(),
        "Native import exact mesh identity or vertex count changed.");
    checkpoint();
    verify_files();
    require(AI::model_artifact_sha256(metadata_path) == proof.at("metadata_file_sha256"),
        "Native import metadata changed during verification.");
    if (!WorkbenchArtifacts::same_model_path(boost::nowide::narrow(request.artifact.local_path.wstring()),
            reference.at("carrier_path").get<std::string>()))
        require(AI::model_artifact_sha256(AI::beauty_metadata_path(request.artifact.local_path, local_root)) ==
            proof.at("metadata_file_sha256"), "Native import local history changed during verification.");
    checkpoint();
}

} // namespace NativeImportTopologyDetail

inline bool has_native_import_topology_proof(const AI::ModelImportRequest& request)
{
    for (const auto& cache : request.native_portrait_partition_references)
        if (cache.is_object() && cache.contains("import_topology_proof")) return true;
    return false;
}

// The host supplies its live Import state or its validated reopened native
// checkpoint. Metadata cannot choose its own authority or certificate.
inline void freeze_native_import_topology_proof(AI::ModelImportRequest& request,
    const nlohmann::json& owned_native_reference, const nlohmann::json& owned_workbench_certificate)
{
    using namespace NativeImportTopologyDetail;
    require(!has_native_import_topology_proof(request), "Native import topology proof is already frozen.");
    const auto metadata_path = path8(owned_native_reference.at("metadata_path").get<std::string>());
    const auto contents = bounded_metadata_bytes(metadata_path);
    require(request.matched_colors && request.matched_colors->valid(),
        "Native import has no valid owned material assignment.");
    nlohmann::json proof {{"schema", "orca.native-import-topology-proof/v1"},
        {"native_reference", owned_native_reference}, {"workbench_certificate", owned_workbench_certificate},
        {"matched_colors_sha256", AI::beauty_leaf_digest(
            AI::encode_model_matched_colors(*request.matched_colors).dump())},
        {"metadata_file_sha256", AI::beauty_leaf_digest(contents)}};
    proof["proof_sha256"] = AI::beauty_leaf_digest(proof.dump());
    const auto& expected_cache = owned_native_reference.at("cache_reference");
    auto candidate = request;
    auto found = candidate.native_portrait_partition_references.end();
    for (auto it = candidate.native_portrait_partition_references.begin();
         it != candidate.native_portrait_partition_references.end(); ++it) {
        if (*it != expected_cache) continue;
        require(found == candidate.native_portrait_partition_references.end(),
            "Duplicate native import cache references.");
        found = it;
    }
    require(found != candidate.native_portrait_partition_references.end(),
        "The owned native import cache reference is missing.");
    (*found)["import_topology_proof"] = proof;
    TriangleMesh exact_mesh;
    load_verified_native_mesh(candidate, proof, exact_mesh, {});
    request = std::move(candidate);
}

inline bool restore_certified_native_import_topology(TriangleMesh& mesh,
    const AI::ModelImportRequest& request, AI::ModelImportResult& result,
    std::string& rejection, const std::function<bool()>& canceled = {})
{
    try {
        using namespace NativeImportTopologyDetail;
        const auto* proof = claimed_proof(request);
        require(proof != nullptr, "Native import topology proof is missing.");
        TriangleMesh exact_mesh;
        load_verified_native_mesh(request, *proof, exact_mesh, canceled);
        require(NativePortrait::same_native_triangle_corners(mesh.its, exact_mesh.its, canceled),
            "Final painted triangles differ from the certified ordered native corners.");
        if (canceled && canceled()) throw std::runtime_error("Native import verification canceled.");
        exact_mesh.set_init_shift(mesh.get_init_shift());
        mesh = std::move(exact_mesh);
        result.topology_risk = result.manual_repair_required = false;
        result.topology_risk_reason.clear();
        // This is an identity restore, not an import-time repair. Whole-face
        // paint is already attached by face ID and remains on the volume.
        rejection.clear();
        return true;
    } catch (const std::exception& failure) {
        rejection = failure.what();
        return false;
    }
}

} // namespace Slic3r::GUI
