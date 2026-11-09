#pragma once

#include <nlohmann/json.hpp>
#include "PostGenerationWorkbenchState.hpp"
#include "slic3r/GUI/AI/Model/BeautyLeafDomain.hpp"
#include "slic3r/AI/Contracts/ModelBaseBinding.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>
#include <algorithm>

namespace Slic3r::GUI::WorkbenchArtifacts {

inline bool same_model_path(const std::string& a, const std::string& b) {
    if (a.empty() || a.size() != b.size()) return false;
    const auto character = [](char c) {
#ifdef _WIN32
        // Asset paths may come from a native path or a generic path join.
        // Those spellings must retain the same workflow ownership on Windows.
        if (c == '\\') return '/';
        if (c >= 'A' && c <= 'Z') return char(c + ('a' - 'A'));
#endif
        return c;
    };
    for (size_t i = 0; i < a.size(); ++i)
        if (character(a[i]) != character(b[i])) return false;
    return true;
}

inline bool valid_sha256(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

inline constexpr uint32_t workbench_check_certificate_version = 1;

inline const char* workbench_check_purpose_id(WorkbenchCheckPurpose purpose) {
    switch (purpose) {
    case WorkbenchCheckPurpose::PortraitSourceRisk: return "portrait_source_risk";
    case WorkbenchCheckPurpose::PortraitPostColorReadOnly: return "portrait_post_color_read_only";
    default: return "general_import";
    }
}

inline const char* workbench_check_status_id(WorkbenchCheckStatus status) {
    switch (status) {
    case WorkbenchCheckStatus::Normal: return "normal";
    case WorkbenchCheckStatus::Attention: return "attention";
    case WorkbenchCheckStatus::Invalid: return "invalid";
    case WorkbenchCheckStatus::Failed: return "failed";
    case WorkbenchCheckStatus::Running: return "running";
    default: return "not_run";
    }
}

inline nlohmann::json workbench_check_metadata_payload(nlohmann::json metadata) {
    if (metadata.is_object()) metadata.erase("workbench_check_certificate");
    return metadata;
}

inline std::string workbench_check_metadata_sha256(const nlohmann::json& metadata) {
    return AI::beauty_leaf_digest(workbench_check_metadata_payload(metadata).dump());
}

inline nlohmann::json encode_workbench_check_certificate(
    const WorkbenchCheckResult& check, WorkbenchCheckPurpose purpose,
    const std::string& geometry_id, const std::string& appearance_id,
    const std::string& metadata_path, const nlohmann::json& metadata,
    const std::string& partition_digest = {}, const std::string& topology_hash = {})
{
    if (purpose == WorkbenchCheckPurpose::GeneralImport || !valid_sha256(check.model_sha256) ||
        geometry_id.empty() || appearance_id.empty() || !metadata.is_object() ||
        (!partition_digest.empty() && !valid_sha256(partition_digest)) ||
        (!topology_hash.empty() && !valid_sha256(topology_hash)))
        throw std::invalid_argument("Invalid portrait workbench check certificate identity.");
    nlohmann::json certificate {
        {"schema", "orca.workbench-check-certificate/v1"},
        {"checker_version", workbench_check_certificate_version},
        {"purpose", workbench_check_purpose_id(purpose)},
        {"status", workbench_check_status_id(check.status)},
        {"model_sha256", check.model_sha256}, {"geometry_id", geometry_id},
        {"appearance_id", appearance_id}, {"metadata_path", metadata_path},
        {"metadata_sha256", workbench_check_metadata_sha256(metadata)},
        {"partition_digest", partition_digest}, {"topology_hash", topology_hash},
        {"summary", check.summary}, {"faces", check.before_faces},
        {"vertices", check.before_vertices}, {"boundary_edges", check.boundary_edges},
        {"boundary_loops", check.boundary_loops},
        {"boundary_loop_sizes", check.boundary_loop_sizes},
        {"nonmanifold_edges", check.nonmanifold_edges},
        {"nonmanifold_vertices", check.nonmanifold_vertices},
        {"degenerate_faces", check.degenerate_faces},
        {"duplicate_faces", check.duplicate_faces},
        {"inconsistent_edges", check.inconsistent_edges},
        {"isolated_components", check.isolated_components}, {"small_faces", check.small_faces},
        {"self_intersections_checked", check.self_intersections_checked},
        {"self_intersections", check.self_intersections}
    };
    certificate["certificate_sha256"] = AI::beauty_leaf_digest(certificate.dump());
    return certificate;
}

inline bool valid_workbench_check_certificate(
    const nlohmann::json& certificate, WorkbenchCheckPurpose purpose,
    const std::string& model_sha256, const std::string& geometry_id,
    const std::string& appearance_id, const std::string& metadata_path,
    const nlohmann::json& metadata, const std::string& partition_digest = {},
    const std::string& topology_hash = {})
{
    try {
        if (purpose == WorkbenchCheckPurpose::GeneralImport || !certificate.is_object() ||
            certificate.at("schema") != "orca.workbench-check-certificate/v1" ||
            certificate.at("checker_version").get<uint32_t>() != workbench_check_certificate_version ||
            certificate.at("purpose") != workbench_check_purpose_id(purpose) ||
            !valid_sha256(model_sha256) || certificate.at("model_sha256") != model_sha256 ||
            geometry_id.empty() || certificate.at("geometry_id") != geometry_id ||
            appearance_id.empty() || certificate.at("appearance_id") != appearance_id ||
            certificate.at("partition_digest") != partition_digest ||
            certificate.at("topology_hash") != topology_hash ||
            (!metadata_path.empty() && !same_model_path(
                certificate.at("metadata_path").get<std::string>(), metadata_path)) ||
            (metadata_path.empty() && !certificate.at("metadata_path").get<std::string>().empty()) ||
            certificate.at("metadata_sha256") != workbench_check_metadata_sha256(metadata)) return false;
        auto unhashed = certificate;
        const auto certificate_sha256 = unhashed.at("certificate_sha256").get<std::string>();
        unhashed.erase("certificate_sha256");
        if (!valid_sha256(certificate_sha256) ||
            AI::beauty_leaf_digest(unhashed.dump()) != certificate_sha256) return false;
        const auto bounded = [&](const char* key, uint64_t maximum) {
            const auto value = certificate.at(key).get<uint64_t>();
            return value <= maximum;
        };
        if (!bounded("faces", 2000000) || certificate.at("faces").get<uint64_t>() == 0 ||
            !bounded("vertices", 6000000) || !bounded("boundary_edges", 6000000) ||
            !bounded("boundary_loops", 2000000) || !bounded("nonmanifold_edges", 6000000) ||
            !bounded("nonmanifold_vertices", 6000000) || !bounded("degenerate_faces", 2000000) ||
            !bounded("duplicate_faces", 2000000) || !bounded("inconsistent_edges", 6000000) ||
            !bounded("isolated_components", 2000000) || !bounded("small_faces", 2000000) ||
            !bounded("self_intersections", 2000000) ||
            !certificate.at("boundary_loop_sizes").is_array() ||
            certificate.at("boundary_loop_sizes").size() > 2000000 ||
            !certificate.at("summary").is_string() ||
            certificate.at("summary").get<std::string>().size() > 4096) return false;
        const auto status = certificate.at("status").get<std::string>();
        if (purpose == WorkbenchCheckPurpose::PortraitSourceRisk)
            return partition_digest.empty() && topology_hash.empty() &&
                (status == "normal" || status == "attention" || status == "invalid");
        return status == "normal" && valid_sha256(partition_digest) && valid_sha256(topology_hash) &&
            certificate.at("boundary_edges").get<uint64_t>() == 0 &&
            certificate.at("nonmanifold_edges").get<uint64_t>() == 0 &&
            certificate.at("nonmanifold_vertices").get<uint64_t>() == 0 &&
            certificate.at("degenerate_faces").get<uint64_t>() == 0 &&
            certificate.at("duplicate_faces").get<uint64_t>() == 0 &&
            certificate.at("inconsistent_edges").get<uint64_t>() == 0 &&
            certificate.at("self_intersections_checked").get<bool>() &&
            certificate.at("self_intersections").get<uint64_t>() == 0;
    } catch (const std::exception&) {
        return false;
    }
}

inline bool decode_workbench_check_certificate(const nlohmann::json& certificate,
                                                WorkbenchCheckResult& result) {
    try {
        auto unhashed = certificate;
        const auto certificate_sha256 = unhashed.at("certificate_sha256").get<std::string>();
        unhashed.erase("certificate_sha256");
        if (!valid_sha256(certificate_sha256) ||
            AI::beauty_leaf_digest(unhashed.dump()) != certificate_sha256) return false;
        WorkbenchCheckResult decoded;
        const auto purpose = certificate.at("purpose").get<std::string>();
        decoded.purpose = purpose == "portrait_source_risk"
            ? WorkbenchCheckPurpose::PortraitSourceRisk
            : purpose == "portrait_post_color_read_only"
                ? WorkbenchCheckPurpose::PortraitPostColorReadOnly
                : WorkbenchCheckPurpose::GeneralImport;
        const auto status = certificate.at("status").get<std::string>();
        decoded.status = status == "normal" ? WorkbenchCheckStatus::Normal :
            status == "attention" ? WorkbenchCheckStatus::Attention :
            status == "invalid" ? WorkbenchCheckStatus::Invalid : WorkbenchCheckStatus::Failed;
        if (decoded.purpose == WorkbenchCheckPurpose::GeneralImport ||
            decoded.status == WorkbenchCheckStatus::Failed) return false;
        decoded.source = WorkbenchCheckSource::Automatic;
        decoded.phase = WorkbenchCheckPhase::Idle;
        decoded.model_sha256 = decoded.source_sha256 = certificate.at("model_sha256").get<std::string>();
        decoded.summary = certificate.at("summary").get<std::string>();
        decoded.before_faces = certificate.at("faces").get<size_t>();
        decoded.before_vertices = certificate.at("vertices").get<size_t>();
        decoded.boundary_edges = certificate.at("boundary_edges").get<size_t>();
        decoded.boundary_loops = certificate.at("boundary_loops").get<size_t>();
        decoded.boundary_loop_sizes = certificate.at("boundary_loop_sizes").get<std::vector<size_t>>();
        decoded.nonmanifold_edges = certificate.at("nonmanifold_edges").get<size_t>();
        decoded.nonmanifold_vertices = certificate.at("nonmanifold_vertices").get<size_t>();
        decoded.degenerate_faces = certificate.at("degenerate_faces").get<size_t>();
        decoded.duplicate_faces = certificate.at("duplicate_faces").get<size_t>();
        decoded.inconsistent_edges = certificate.at("inconsistent_edges").get<size_t>();
        decoded.isolated_components = certificate.at("isolated_components").get<size_t>();
        decoded.small_faces = certificate.at("small_faces").get<size_t>();
        decoded.self_intersections_checked = certificate.at("self_intersections_checked").get<bool>();
        decoded.self_intersections = certificate.at("self_intersections").get<size_t>();
        decoded.candidate_status = RepairCandidateStatus::None;
        decoded.appearance_mapped = decoded.purpose == WorkbenchCheckPurpose::PortraitPostColorReadOnly;
        decoded.appearance_confidence = decoded.appearance_mapped ? 1.0 : 0.0;
        if (decoded.appearance_mapped)
            decoded.appearance_reason = "原始外观引用与分区载体分别保存。";
        result = std::move(decoded);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

inline bool valid_native_partition_bounds(const nlohmann::json& reference)
{
    try {
        if (reference.at("coordinate_units") != "mm") return false;
        const auto decode = [](const nlohmann::json& value,
                               std::array<double, 3>& minimum,
                               std::array<double, 3>& maximum) {
            minimum = value.at("minimum").get<std::array<double, 3>>();
            maximum = value.at("maximum").get<std::array<double, 3>>();
            double diagonal_squared = 0.0;
            for (int axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) ||
                    maximum[axis] < minimum[axis]) return false;
                const double extent = maximum[axis] - minimum[axis];
                diagonal_squared += extent * extent;
            }
            return std::isfinite(diagonal_squared) && diagonal_squared > 0.0;
        };
        std::array<double, 3> source_minimum, source_maximum;
        std::array<double, 3> carrier_minimum, carrier_maximum;
        if (!decode(reference.at("source_bounds_mm"), source_minimum, source_maximum) ||
            !decode(reference.at("carrier_bounds_mm"), carrier_minimum, carrier_maximum)) return false;
        double diagonal_squared = 0.0;
        for (int axis = 0; axis < 3; ++axis) {
            const double extent = source_maximum[axis] - source_minimum[axis];
            diagonal_squared += extent * extent;
        }
        const double tolerance = std::max(1e-5, std::sqrt(diagonal_squared) * 0.0025);
        for (int axis = 0; axis < 3; ++axis)
            if (std::abs(source_minimum[axis] - carrier_minimum[axis]) > tolerance ||
                std::abs(source_maximum[axis] - carrier_maximum[axis]) > tolerance) return false;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

inline bool valid_native_partition_reference(const nlohmann::json& reference,
                                             const std::string& digest = {},
                                             const std::string& topology = {}) {
    try {
        if (!reference.is_object() ||
            reference.at("schema") != "orca.native-workbench-partition-reference/v2" ||
            reference.at("algorithm").get<uint32_t>() != 12 ||
            !valid_native_partition_bounds(reference) ||
            !valid_sha256(reference.at("partition_digest").get<std::string>()) ||
            !valid_sha256(reference.at("topology_hash").get<std::string>()) ||
            !valid_sha256(reference.at("carrier_sha256").get<std::string>()) ||
            !valid_sha256(reference.at("appearance_sha256").get<std::string>()) ||
            !valid_sha256(reference.at("cache_sha256").get<std::string>()) ||
            !valid_sha256(reference.at("source_sha256").get<std::string>()) ||
            reference.at("source_path").get<std::string>().empty() ||
            reference.at("carrier_path").get<std::string>().empty() ||
            reference.at("appearance_path").get<std::string>().empty() ||
            reference.at("cache_path").get<std::string>().empty() ||
            reference.at("metadata_path").get<std::string>().empty() ||
            reference.at("geometry_id").get<std::string>().empty() ||
            reference.at("appearance_id").get<std::string>().empty() ||
            !reference.at("cache_reference").is_object() ||
            reference.at("cache_reference").at("schema") !=
                "orca.native-workbench-partition-cache-reference/v1" ||
            reference.at("cache_reference").at("content_id") != reference.at("cache_sha256") ||
            reference.at("cache_reference").at("partition_digest") !=
                reference.at("partition_digest") ||
            reference.at("cache_reference").at("geometry_id") != reference.at("geometry_id")) return false;
        if (reference.at("cache_reference").at("face_count").get<size_t>() == 0 ||
            reference.at("cache_reference").at("face_count").get<size_t>() > 1000000) return false;
        return (digest.empty() || reference.at("partition_digest") == digest) &&
            (topology.empty() || reference.at("topology_hash") == topology);
    } catch (const std::exception&) {
        return false;
    }
}

// Older v12 caches retain readable identity but cannot restore certified indices.
inline bool native_partition_has_exact_mesh(const nlohmann::json& reference) {
    try {
        return valid_native_partition_reference(reference) && valid_sha256(
            reference.at("cache_reference").at("native_mesh_sha256").get<std::string>());
    } catch (const std::exception&) { return false; }
}

inline bool valid_region_semantics(const nlohmann::json& binding, const std::string& digest) {
    try {
        if (!valid_sha256(digest) || !binding.is_object() ||
            binding.at("schema") != "orca.native-workbench-region-semantics/v12" ||
            binding.at("partition_digest") != digest || !binding.at("regions").is_array() ||
            binding.at("regions").empty()) return false;
        for (size_t region = 0; region < binding.at("regions").size(); ++region) {
            const auto& row = binding.at("regions").at(region);
            const double confidence = row.at("confidence").get<double>();
            if (!row.is_object() || row.at("region_id").get<size_t>() != region ||
                row.at("semantic").get<int>() < 0 || row.at("semantic").get<int>() >= 12 ||
                !std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0 ||
                !row.at("semantic_bound").is_boolean() ||
                !row.at("role_support").is_array() || row.at("role_support").size() != 12) return false;
            const double area = row.at("observed_area").get<double>();
            if (!std::isfinite(area) || area < 0.) return false;
            double sum = 0.;
            for (const auto& value : row.at("role_support")) {
                const double support = value.get<double>();
                if (!std::isfinite(support) || support < 0.) return false;
                sum += support;
            }
            if (sum > area * (1. + 1e-6) + 1e-9) return false;
        }
        const size_t faces = binding.at("face_count").get<size_t>();
        const auto& components = binding.at("protected_components");
        if (!faces || faces > 4000000 || !components.is_array() || components.size() > faces) return false;
        std::vector<uint8_t> seen(faces, 0);
        for (size_t id = 0; id < components.size(); ++id) {
            const auto& row = components[id];
            const int role = row.at("semantic").get<int>();
            const double confidence = row.at("confidence").get<double>();
            if (row.at("id").get<size_t>() != id ||
                !(role == 2 || role == 7 || role == 9 || role == 10 || role == 11) ||
                !std::isfinite(confidence) || confidence < .70 || confidence > 1. ||
                row.at("person_id").get<uint64_t>() > UINT32_MAX ||
                !row.at("faces").is_array() || row.at("faces").empty()) return false;
            size_t previous = 0; bool first = true;
            for (const auto& value : row.at("faces")) {
                const size_t face = value.get<size_t>();
                if (face >= faces || seen[face]++ || (!first && face <= previous)) return false;
                previous = face; first = false;
            }
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

inline bool valid_native_color_reduction(const nlohmann::json& reduction,
                                         const std::string& digest,
                                         size_t region_count) {
    try {
        if (!valid_sha256(digest) || region_count == 0 || !reduction.is_object() ||
            reduction.at("schema") != "orca.native-workbench-color-reduction/v12" ||
            reduction.at("algorithm").get<uint32_t>() != 12 ||
            reduction.at("partition_digest") != digest ||
            reduction.at("requested_color_count").get<size_t>() != 5 ||
            reduction.at("actual_color_count").get<size_t>() == 0 ||
            reduction.at("actual_color_count").get<size_t>() > 5 ||
            !reduction.at("region_color_groups").is_array() ||
            reduction.at("region_color_groups").size() != region_count ||
            !reduction.at("group_cluster_ids").is_array() ||
            reduction.at("group_cluster_ids").size() !=
                reduction.at("actual_color_count").get<size_t>() ||
            !reduction.at("merge_order").is_array() ||
            reduction.at("merge_order").size() > region_count ||
            !valid_sha256(reduction.at("mapping_digest").get<std::string>())) return false;
        std::vector<uint8_t> used(reduction.at("actual_color_count").get<size_t>(), 0);
        for (const auto& group : reduction.at("region_color_groups")) {
            const auto value = group.get<size_t>();
            if (value >= used.size()) return false;
            used[value] = 1;
        }
        if (std::find(used.begin(), used.end(), uint8_t(0)) != used.end()) return false;
        for (const auto& cluster : reduction.at("group_cluster_ids"))
            if (cluster.get<size_t>() >= 32) return false;
        for (const auto& merge : reduction.at("merge_order"))
            if (!merge.is_array() || merge.size() != 2 ||
                merge[0].get<size_t>() >= region_count ||
                merge[1].get<size_t>() >= region_count || merge[0] == merge[1]) return false;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

inline bool valid_final_slot_binding(const nlohmann::json& binding,
                                     const std::string& digest,
                                     const std::string& reduction_digest,
                                     size_t region_count,
                                     size_t model_color_count = 5) {
    try {
        if (!valid_sha256(digest) || !valid_sha256(reduction_digest) || region_count == 0 ||
            model_color_count == 0 || !binding.is_object() ||
            binding.at("schema") != "orca.native-workbench-final-slot-binding/v12" ||
            binding.at("partition_digest") != digest ||
            binding.at("reduction_digest") != reduction_digest ||
            !binding.at("regions").is_array() || binding.at("regions").size() != region_count ||
            !binding.at("subface_slots").is_array() || !binding.at("subface_slots").empty()) return false;
        for (size_t region = 0; region < region_count; ++region) {
            const auto& row = binding.at("regions").at(region);
            if (!row.is_object() || row.at("region_id").get<size_t>() != region ||
                row.at("slot").get<size_t>() >= model_color_count) return false;
        }
        const size_t faces = binding.at("face_count").get<size_t>();
        const size_t components = binding.at("component_count").get<size_t>();
        if (!faces || faces > 4000000 || components > faces || !binding.at("face_overrides").is_array() ||
            binding.at("face_overrides").size() > faces) return false;
        size_t previous = 0; bool first = true;
        for (const auto& row : binding.at("face_overrides")) {
            if (!row.is_array() || row.size() != 3) return false;
            const size_t face = row[0].get<size_t>();
            if (face >= faces || (!first && face <= previous) || row[1].get<size_t>() >= components ||
                row[2].get<size_t>() >= model_color_count) return false;
            previous = face; first = false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

inline bool valid_post_color_check(const nlohmann::json& check,
                                   const nlohmann::json& reference) {
    try {
        return valid_native_partition_reference(reference) && check.is_object() &&
            check.at("schema") == "orca.portrait-post-color-check/v1" &&
            check.at("status") == "normal" &&
            check.at("model_sha256") == reference.at("carrier_sha256") &&
            check.at("geometry_id") == reference.at("geometry_id") &&
            check.at("partition_digest") == reference.at("partition_digest") &&
            check.at("topology_hash") == reference.at("topology_hash") &&
            check.at("faces").get<size_t>() > 0 &&
            check.at("boundary_edges").get<size_t>() == 0 &&
            check.at("nonmanifold_edges").get<size_t>() == 0 &&
            check.at("nonmanifold_vertices").get<size_t>() == 0 &&
            check.at("degenerate_faces").get<size_t>() == 0 &&
            check.at("duplicate_faces").get<size_t>() == 0 &&
            check.at("inconsistent_edges").get<size_t>() == 0 &&
            check.at("self_intersections_checked").get<bool>() &&
            check.at("self_intersections").get<size_t>() == 0;
    } catch (const std::exception&) {
        return false;
    }
}


}
