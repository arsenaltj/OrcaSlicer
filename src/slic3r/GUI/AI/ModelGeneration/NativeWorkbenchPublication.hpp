#pragma once

#include "PortraitColorBlockPostprocess.hpp"
#include "WorkbenchModelInspection.hpp"
#include <boost/nowide/convert.hpp>

namespace Slic3r::GUI {

inline nlohmann::json encode_portrait_post_color_check(
    const WorkbenchCheckResult& check,
    const nlohmann::json& partition_reference)
{
    if (!workbench_topology_safe(check) ||
        !WorkbenchArtifacts::valid_native_partition_reference(partition_reference) ||
        check.model_sha256 != partition_reference.at("carrier_sha256"))
        throw std::runtime_error("着色后只读拓扑检查未通过。");
    return {{"schema", "orca.portrait-post-color-check/v1"}, {"status", "normal"},
        {"model_sha256", check.model_sha256},
        {"geometry_id", partition_reference.at("geometry_id")},
        {"partition_digest", partition_reference.at("partition_digest")},
        {"topology_hash", partition_reference.at("topology_hash")},
        {"faces", check.before_faces}, {"vertices", check.before_vertices},
        {"boundary_edges", check.boundary_edges}, {"boundary_loops", check.boundary_loops},
        {"boundary_loop_sizes", check.boundary_loop_sizes},
        {"nonmanifold_edges", check.nonmanifold_edges},
        {"nonmanifold_vertices", check.nonmanifold_vertices},
        {"degenerate_faces", check.degenerate_faces},
        {"duplicate_faces", check.duplicate_faces},
        {"inconsistent_edges", check.inconsistent_edges},
        {"isolated_components", check.isolated_components}, {"small_faces", check.small_faces},
        {"self_intersections_checked", check.self_intersections_checked},
        {"self_intersections", check.self_intersections}, {"issue_faces", check.issue_faces}};
}

// Native geometry and its color layer have separate identities. Manual saves
// publish the same verified carrier plus a complete, independently checked layer.
inline void prepare_native_workbench_publication(nlohmann::json& metadata,
    const boost::filesystem::path& metadata_path, const boost::filesystem::path& candidate,
    const PortraitColorBlocks::Result& blocks, const WorkbenchNativeTopologyCertificate& proof)
{
    using namespace WorkbenchArtifacts;
    auto reference = proof.native_reference;
    if (!valid_native_partition_reference(reference, proof.partition_digest, proof.topology_hash) ||
        proof.candidate_sha256 != proof.carrier_sha256 ||
        proof.geometry_id != blocks.geometry_id ||
        !blocks.native_partition_identity ||
        blocks.native_partition_identity->partition_digest != proof.partition_digest)
        throw std::runtime_error("Native color publication identity mismatch.");
    reference["carrier_path"] = boost::nowide::narrow(candidate.wstring());
    reference["carrier_sha256"] = proof.candidate_sha256;
    reference["metadata_path"] = boost::nowide::narrow(metadata_path.wstring());
    const auto encoded = PortraitColorBlocks::encode(blocks);
    const auto& semantics = encoded.at("region_semantics");
    const auto& reduction = encoded.at("native_color_reduction");
    const auto& slots = encoded.at("final_slot_binding");
    const size_t regions = semantics.at("regions").size();
    if (!valid_region_semantics(semantics, proof.partition_digest) ||
        !valid_native_color_reduction(reduction, proof.partition_digest, regions) ||
        !valid_final_slot_binding(slots, proof.partition_digest,
            reduction.at("mapping_digest").get<std::string>(), regions))
        throw std::runtime_error("Native regions, reduction or slot binding are invalid.");
    metadata["native_portrait_partition"] = std::move(reference);
    metadata["native_portrait_topology_certificate"] = proof.partition_metadata.at("native_portrait_topology_certificate");
    metadata["partition_build_report"] = proof.partition_metadata.at("partition_build_report");
    metadata["region_semantics"] = semantics;
    metadata["native_color_reduction"] = reduction;
    metadata["final_slot_binding"] = slots;
}

inline void certify_native_workbench_publication(nlohmann::json& metadata,
    const boost::filesystem::path& metadata_path, const WorkbenchCheckResult& checked)
{
    using namespace WorkbenchArtifacts;
    const auto& reference = metadata.at("native_portrait_partition");
    metadata["post_color_check"] = encode_portrait_post_color_check(checked, reference);
    if (!valid_post_color_check(metadata.at("post_color_check"), reference))
        throw std::runtime_error("Native post-color topology evidence is invalid.");
    const auto geometry = reference.at("geometry_id").get<std::string>();
    const auto appearance = reference.at("appearance_id").get<std::string>();
    const auto digest = reference.at("partition_digest").get<std::string>();
    const auto topology = reference.at("topology_hash").get<std::string>();
    const auto path = boost::nowide::narrow(metadata_path.wstring());
    auto certificate = encode_workbench_check_certificate(checked,
        WorkbenchCheckPurpose::PortraitPostColorReadOnly, geometry, appearance, path, metadata, digest, topology);
    if (!valid_workbench_check_certificate(certificate, WorkbenchCheckPurpose::PortraitPostColorReadOnly,
            checked.model_sha256, geometry, appearance, path, metadata, digest, topology))
        throw std::runtime_error("Native publication check certificate is invalid.");
    metadata["workbench_check_certificate"] = std::move(certificate);
}
}
