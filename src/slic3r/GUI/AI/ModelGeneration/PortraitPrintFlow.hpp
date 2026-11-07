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

namespace Slic3r::GUI::PortraitPrint {

enum class Submission { Submitted, Pending, Cancelled, Rejected };
inline bool accepted(Submission value) { return value == Submission::Submitted || value == Submission::Pending; }

enum class Stage { Idle, Project, Palette, Design, Model, Check, Color, SaveColor,
                   Filaments, Import, Recommend, Slice, PrintReady, Partition, Base };

// Persisted v1-v3 stage numbers are stable. Workbench order is explicit so new
// local phases cannot accidentally authorize any of the old generation phases.
inline int workbench_order(Stage stage) {
    switch (stage) {
    case Stage::Project: return 0; case Stage::Check: return 1;
    case Stage::Partition: return 2; case Stage::Color: return 3;
    case Stage::SaveColor: return 4; case Stage::Base: return 5;
    case Stage::Filaments: return 6; case Stage::Import: return 7;
    case Stage::Recommend: return 8; case Stage::Slice: return 9;
    case Stage::PrintReady: return 10; default: return -1;
    }
}
inline bool model_processing(Stage stage) {
    const int order = workbench_order(stage);
    return order >= 1 && order <= 7;
}
inline bool before_import(Stage stage) {
    const int order = workbench_order(stage);
    return order >= 0 && order < 7;
}
inline bool protects_saved_color(Stage stage) {
    const int order = workbench_order(stage);
    return order >= 4 && order <= 7;
}

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

inline std::string portrait_source_asset_id_from_path(const std::string& path) {
    if (path.empty()) return {};
    const auto valid_id = [](const std::string& value) {
        return !value.empty() && value.size() <= 128 &&
            value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") ==
                std::string::npos;
    };
    const auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return character >= 'A' && character <= 'Z' ? char(character + ('a' - 'A')) : char(character);
        });
        return value;
    };
    const auto slash = path.find_last_of("/\\");
    const std::string filename = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const std::string normalized = lower(filename);
    constexpr const char* prefix = "orcaslicer-ai-";
    constexpr const char* suffix = ".glb";
    if (normalized.rfind(prefix, 0) == 0 && normalized.size() > std::char_traits<char>::length(prefix) +
            std::char_traits<char>::length(suffix) &&
        normalized.compare(normalized.size() - std::char_traits<char>::length(suffix),
            std::char_traits<char>::length(suffix), suffix) == 0) {
        const auto id = filename.substr(std::char_traits<char>::length(prefix),
            filename.size() - std::char_traits<char>::length(prefix) - std::char_traits<char>::length(suffix));
        return valid_id(id) ? lower(id) : std::string{};
    }
    if (normalized != "model.glb" || slash == std::string::npos) return {};
    const auto parent_end = path.find_last_not_of("/\\", slash);
    if (parent_end == std::string::npos) return {};
    const auto parent_start = path.find_last_of("/\\", parent_end);
    const auto id = path.substr(parent_start == std::string::npos ? 0 : parent_start + 1,
        parent_end - (parent_start == std::string::npos ? 0 : parent_start + 1) + 1);
    return valid_id(id) ? lower(id) : std::string{};
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

inline bool valid_role_binding(const nlohmann::json& semantics, const nlohmann::json& slots,
                               size_t model_color_count) {
    try {
        if (semantics.at("face_count") != slots.at("face_count") ||
            semantics.at("protected_components").size() != slots.at("component_count").get<size_t>()) return false;
        const size_t faces = semantics.at("face_count").get<size_t>();
        if (!faces || faces > 4000000) return false;
        std::vector<size_t> owners(faces, SIZE_MAX);
        size_t count = 0;
        for (const auto& component : semantics.at("protected_components")) {
            if (component.at("slot").get<size_t>() >= model_color_count) return false;
            for (const auto& face : component.at("faces")) {
                if (face.get<size_t>() >= faces || owners[face.get<size_t>()] != SIZE_MAX) return false;
                owners[face.get<size_t>()] = component.at("id").get<size_t>(); ++count;
            }
        }
        if (count != slots.at("face_overrides").size()) return false;
        for (const auto& row : slots.at("face_overrides")) {
            const size_t face = row[0].get<size_t>(), id = row[1].get<size_t>();
            if (face >= faces || owners[face] != id || id >= semantics.at("protected_components").size() ||
                row[2] != semantics.at("protected_components")[id].at("slot")) return false;
            owners[face] = SIZE_MAX;
        }
        return true;
    } catch (const std::exception&) { return false; }
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

struct State;

struct ColorCandidateIdentity {
    std::string flow_id;
    std::string color_asset_id;
    std::string candidate_path;
    std::string source_sha256;
    std::string candidate_sha256;
    std::string geometry_id;
    std::string appearance_id;
    std::string partition_digest;
    std::string topology_hash;

    bool valid() const {
        return !flow_id.empty() && !color_asset_id.empty() && !candidate_path.empty() &&
            valid_sha256(source_sha256) && valid_sha256(candidate_sha256) &&
            !geometry_id.empty() && !appearance_id.empty() && valid_sha256(partition_digest) && valid_sha256(topology_hash);
    }
    bool owns(const State& flow, const std::string& asset_id, const std::string& path,
              const std::string& source_hash, const std::string& candidate_hash,
              const std::string& geometry, const std::string& appearance) const;
};

inline bool valid_frozen(const nlohmann::json& input) {
    if (!input.is_object()) return false;
    for (const auto* key : {"image", "prompt", "style", "custom_style", "provider", "design_provider", "design_model",
                           "geometry_model", "geometry_quality", "texture_quality", "output_format", "profile", "print_mode", "shadow_color", "color_distance"})
        if (!input.contains(key) || !input.at(key).is_string()) return false;
    if (input.at("style") != "realistic" || (input.at("image") == "" && input.at("prompt") == "")) return false;
    if (!input.contains("face_limit") || !input.at("face_limit").is_number_integer()) return false;
    if (input.at("color_distance") != "ciede2000" && input.at("color_distance") != "delta_e76") return false;
    for (const auto* key : {"width_mm", "nozzle_mm", "line_width_mm", "minimum_feature_mm"}) {
        if (!input.contains(key) || !input.at(key).is_number()) return false;
        const double value = input.at(key).get<double>();
        if (!std::isfinite(value) || value < 0 || value > 100000) return false;
    }
    return true;
}

// Durable intent ledger. A reservation is saved BEFORE sending a request. It
// survives crashes even when the response (and therefore the remote ID) is lost.
// Authorization and automatic continuation deliberately never survive restart.
struct State {
    int record_version {7};
    std::string id;
    std::string entry {"generation"};
    bool from_workbench() const { return entry == "completed_model"; }
    std::string color_asset_id() const {
        if (record_version >= 6)
            return "finish-workbench-native-v" + std::to_string(boundary_algorithm_version) + "-" + id;
        if (record_version >= 5)
            return "finish-portrait-native-v" + std::to_string(boundary_algorithm_version) + "-" + id;
        if (record_version >= 4 && boundary_algorithm_version > 8)
            return "finish-portrait-slots-v" + std::to_string(boundary_algorithm_version) + "-" + id;
        if (record_version >= 4 && boundary_algorithm_version == 8)
            return "finish-portrait-slots-" + id;
        return (record_version >= 4 ? "finish-portrait-v4-" : "finish-portrait-") + id;
    }
    bool colored_saved {false}, semantic_applied {false}, person_detected {false};
    std::string evidence_status, application_status;
    std::string geometry_id, appearance_id, region_binding_digest;
    uint64_t edit_revision {0};
    nlohmann::json base_binding;
    Stage stage {Stage::Idle};
    bool paused {false}, failed {false}, authorized {false}, restored {false};
    bool design_reserved {false}, model_reserved {false}, palette_confirmed {false};
    bool fallback_confirmed {false}, near_color_fallback {false};
    bool design_risk_accepted {false}, model_risk_accepted {false};
    bool repair_saved {false}, slice_started {false};
    bool repair_checked {false}, partition_completed {false};
    // v5 separates source risk inspection from the topology certificate
    // produced by the native partition transaction.
    bool source_checked {false}, partition_pending {false}, partition_committed {false};
    std::string source_check_hash, partition_topology_hash, pending_partition_metadata_path;
    nlohmann::json source_check;
    nlohmann::json native_portrait_partition;
    // v5 kept semantics and slots in one legacy binding. v6 persists the
    // immutable region semantics, deterministic reduction and final slots as
    // separate identities so none can be mistaken for partition geometry.
    nlohmann::json semantic_binding;
    nlohmann::json region_semantics;
    nlohmann::json native_color_reduction;
    nlohmann::json final_slot_binding;
    nlohmann::json partition_build_report;
    nlohmann::json post_color_check;
    nlohmann::json workbench_check_certificate;
    uint32_t boundary_algorithm_version {0};
    bool round_base_enabled {true}, native_repair_required {true}, native_repair_completed {false};
    int generation_attempt {0}, palette_retries {0}, design_retries {0}, model_retries {0}, conversion_retries {0};
    std::string semantic_status, recovery_project, native_geometry_id, preview_result_id;
    std::vector<std::string> notices, retired_task_ids;
    int requested_count {6}, final_count {0};
    int model_color_count {5}, physical_slot_count {6}, support_slot_count {1};
    std::string reason, request_id, design_job_id, model_job_id, provider_task_id, conversion_task_id;
    std::string source_image_hash, source_image_copy, design_path;
    std::string authorization_id;
    std::vector<std::string> final_palette;
    std::string original_model_path, original_model_hash, model_path, model_hash;
    std::string repair_source_hash, repair_result_hash;
    std::string basis_signature, suite_revision, workspace_id, project_revision, slice_revision, gcode_path;
    nlohmann::json frozen = nlohmann::json::object();

    bool active() const { return !id.empty() && stage != Stage::Idle && stage != Stage::PrintReady; }
    bool owns_workbench_model(const std::string& path, const std::string& geometry,
                              const std::string& appearance) const {
        // Completion ends automation, not ownership of its source and saved
        // colour carrier. Reopening either must not start the legacy repair.
        return from_workbench() && !id.empty() && stage != Stage::Idle && !path.empty() &&
            !geometry_id.empty() && geometry == geometry_id &&
            !appearance_id.empty() && appearance == appearance_id &&
            (same_model_path(path, original_model_path) || same_model_path(path, model_path));
    }
    bool owns_display_context(const std::string& path, const std::string& hash,
                              const std::string& geometry, const std::string& appearance) const {
        return from_workbench() && !id.empty() && stage != Stage::Idle && !path.empty() &&
            same_model_path(path, model_path) && !model_hash.empty() && hash == model_hash &&
            !geometry_id.empty() && geometry == geometry_id &&
            !appearance_id.empty() && appearance == appearance_id;
    }
    bool can_adopt_source_alias(const std::string& path, const std::string& hash,
                                const std::string& geometry, const std::string& appearance,
                                const std::string& asset_id = {}) const {
        const std::string recorded_asset = frozen.is_object()
            ? frozen.value("asset_id", portrait_source_asset_id_from_path(original_model_path))
            : std::string{};
        const std::string displayed_asset = asset_id.empty()
            ? portrait_source_asset_id_from_path(path) : asset_id;
        const auto normalize_asset = [](std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
                return character >= 'A' && character <= 'Z' ? char(character + ('a' - 'A')) : char(character);
            });
            return value;
        };
        return from_workbench() && record_version >= 5 && active() && paused && !failed &&
            (stage == Stage::Check || stage == Stage::Partition) && !partition_pending &&
            !partition_committed && !partition_completed && !colored_saved && !semantic_applied &&
            frozen.is_object() && !path.empty() && !same_model_path(path, model_path) &&
            same_model_path(model_path, original_model_path) && !hash.empty() &&
            hash == model_hash && hash == original_model_hash &&
            !geometry_id.empty() && geometry == geometry_id &&
            !appearance_id.empty() && appearance == appearance_id &&
            !recorded_asset.empty() && normalize_asset(displayed_asset) == normalize_asset(recorded_asset);
    }
    bool adopt_source_alias(const std::string& path, const std::string& hash,
                            const std::string& geometry, const std::string& appearance,
                            const std::string& asset_id = {}) {
        if (!can_adopt_source_alias(path, hash, geometry, appearance, asset_id)) return false;
        const auto canonical_asset = asset_id.empty()
            ? portrait_source_asset_id_from_path(path) : asset_id;
        original_model_path = model_path = path;
        frozen["model_path"] = path;
        frozen["asset_id"] = canonical_asset;
        return true;
    }
    bool running() const { return active() && !paused && !failed && authorized; }
    void rollback_color_save() {
        stage = Stage::Color; paused = true; authorized = false; failed = false; reason.clear();
        colored_saved = false; region_binding_digest.clear();
        semantic_binding = nullptr; region_semantics = nullptr; native_color_reduction = nullptr;
        final_slot_binding = nullptr; post_color_check = nullptr; base_binding = nullptr;
    }
    bool current(const std::string& token, Stage expected) const {
        return running() && id == token && stage == expected;
    }
    bool partition_needs_rebuild(uint32_t version) const {
        if (!from_workbench() || record_version < 4 ||
            workbench_order(stage) < workbench_order(Stage::Partition)) return false;
        if (record_version < 7) return true;
        return boundary_algorithm_version != version || !partition_committed ||
            !valid_native_partition_reference(native_portrait_partition, basis_signature, partition_topology_hash) ||
            !native_partition_has_exact_mesh(native_portrait_partition);
    }
    bool reset_partition_to_original(uint32_t version, const std::string& original_path,
                                     const std::string& verified_hash) {
        if (!from_workbench() || !paused || original_path.empty() ||
            original_model_hash.empty() || verified_hash != original_model_hash ||
            !same_model_path(original_path, original_model_path)) return false;
        record_version = 7;
        model_path = original_path; model_hash = verified_hash;
        stage = Stage::Check;
        boundary_algorithm_version = version;
        model_color_count = 5;
        physical_slot_count = 6;
        support_slot_count = 1;
        requested_count = final_count = model_color_count;
        partition_completed = colored_saved = semantic_applied = person_detected = false;
        slice_started = native_repair_completed = near_color_fallback = false;
        native_repair_required = true;
        source_checked = partition_pending = partition_committed = false;
        source_check_hash.clear(); source_check = nullptr;
        partition_topology_hash.clear(); pending_partition_metadata_path.clear();
        native_portrait_partition = nullptr; semantic_binding = nullptr;
        region_semantics = nullptr; native_color_reduction = nullptr;
        final_slot_binding = nullptr;
        partition_build_report = nullptr; post_color_check = nullptr;
        workbench_check_certificate = nullptr;
        repair_checked = repair_saved = false;
        repair_source_hash.clear(); repair_result_hash.clear();
        basis_signature.clear(); region_binding_digest.clear();
        geometry_id.clear(); appearance_id.clear(); edit_revision = 0;
        evidence_status.clear(); application_status.clear(); semantic_status.clear();
        native_geometry_id.clear(); project_revision.clear(); slice_revision.clear();
        preview_result_id.clear(); gcode_path.clear();
        return true;
    }
    bool rebuild_partition(uint32_t version, const std::string& original_path, const std::string& verified_hash) {
        return partition_needs_rebuild(version) &&
            reset_partition_to_original(version, original_path, verified_hash);
    }
    bool adopt_partition_commit(const nlohmann::json& document) {
        try {
            if (!from_workbench() || record_version < 7 || !restored || !paused || authorized ||
                colored_saved || workbench_order(stage) < workbench_order(Stage::Partition) ||
                !source_checked || source_check_hash != original_model_hash ||
                document.at("schema") != "orca.native-workbench-partition-commit/v1" ||
                document.at("transaction_status") != "committed" ||
                (!document.value("portrait_print_id", std::string{}).empty() &&
                    document.at("portrait_print_id") != id) ||
                document.at("source_sha256") != original_model_hash ||
                !same_model_path(document.at("source_model").get<std::string>(), original_model_path) ||
                !document.at("partition_build_report").is_object()) return false;
            const auto& reference = document.at("native_portrait_partition");
            const auto digest = reference.at("partition_digest").get<std::string>();
            const auto topology_hash = reference.at("topology_hash").get<std::string>();
            if (!valid_native_partition_reference(reference, digest, topology_hash) ||
                document.at("model_sha256") != reference.at("carrier_sha256") ||
                !same_model_path(document.at("model_path").get<std::string>(),
                    reference.at("carrier_path").get<std::string>()) ||
                (partition_pending && !same_model_path(pending_partition_metadata_path,
                    reference.at("metadata_path").get<std::string>())) ||
                (partition_committed && native_portrait_partition != reference)) return false;
            native_portrait_partition = reference;
            partition_topology_hash = topology_hash;
            basis_signature = digest;
            partition_build_report = document.at("partition_build_report");
            const auto recovered_certificate = document.value(
                "workbench_check_certificate", nlohmann::json{});
            workbench_check_certificate = valid_workbench_check_certificate(
                recovered_certificate, WorkbenchCheckPurpose::PortraitPostColorReadOnly,
                reference.at("carrier_sha256").get<std::string>(),
                reference.at("geometry_id").get<std::string>(),
                reference.at("appearance_id").get<std::string>(),
                reference.at("metadata_path").get<std::string>(), document,
                digest, topology_hash) ? recovered_certificate : nlohmann::json{};
            partition_pending = false;
            partition_committed = true;
            partition_completed = true;
            pending_partition_metadata_path.clear();
            boundary_algorithm_version = reference.at("algorithm").get<uint32_t>();
            model_path = reference.at("carrier_path").get<std::string>();
            model_hash = reference.at("carrier_sha256").get<std::string>();
            geometry_id = reference.at("geometry_id").get<std::string>();
            appearance_id = reference.at("appearance_id").get<std::string>();
            repair_checked = repair_saved = false;
            repair_source_hash.clear();
            repair_result_hash.clear();
            native_repair_required = false;
            if (stage == Stage::Partition) stage = Stage::Color;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    void pause(std::string message, bool hard = false) {
        paused = true; failed = hard; reason = std::move(message);
    }
    bool pause_for_history_switch(bool export_in_flight) {
        if (!from_workbench() || !active() || failed ||
            (export_in_flight && stage != Stage::SaveColor)) return false;
        if (export_in_flight) {
            // A canceled export has no committed color carrier. Resume from
            // read-only region annotation after the exact owner is reopened.
            stage = Stage::Color;
            colored_saved = false;
            semantic_applied = false;
            person_detected = false;
            near_color_fallback = false;
            evidence_status.clear();
            application_status.clear();
            semantic_status.clear();
            region_binding_digest.clear();
            base_binding = nullptr;
            semantic_binding = nullptr;
            region_semantics = nullptr;
            native_color_reduction = nullptr;
            final_slot_binding = nullptr;
            post_color_check = nullptr;
        }
        paused = true;
        authorized = false;
        reason = "已切换到其他历史模型；所属模型的一键流程已保存为可继续状态。";
        return true;
    }
    bool advance(const std::string& token, Stage expected, Stage next) {
        if (!current(token, expected)) return false;
        if (from_workbench()) {
            if (workbench_order(expected) < 0 || workbench_order(next) != workbench_order(expected) + 1 ||
                (expected == Stage::Check && (record_version >= 5 ? !source_checked : !repair_checked)) ||
                (expected == Stage::Partition && (!partition_completed || basis_signature.empty() ||
                    (record_version >= 5 && (!partition_committed || native_portrait_partition.is_null() ||
                        native_portrait_partition.empty() || partition_topology_hash.empty() ||
                        !same_model_path(model_path, native_portrait_partition.value("carrier_path", std::string{})) ||
                        model_hash != native_portrait_partition.value("carrier_sha256", std::string{}))))) ||
                (record_version >= 6 && expected == Stage::SaveColor &&
                    (!colored_saved || !valid_region_semantics(region_semantics, basis_signature) ||
                     !valid_native_color_reduction(native_color_reduction, basis_signature,
                        region_semantics.at("regions").size()) ||
                     !valid_final_slot_binding(final_slot_binding, basis_signature,
                        native_color_reduction.at("mapping_digest").get<std::string>(),
                        region_semantics.at("regions").size(), size_t(model_color_count)) ||
                     !valid_role_binding(region_semantics, final_slot_binding, size_t(model_color_count)) ||
                     !valid_post_color_check(post_color_check, native_portrait_partition)))) return false;
        } else if (int(next) != int(expected) + 1) return false;
        stage = next; reason.clear(); return true;
    }
    bool reserve_design(const std::string& token, const std::string& request) {
        if (from_workbench() || !current(token, Stage::Palette) || design_reserved || request.empty()) return false;
        request_id = request; design_reserved = true; return true;
    }
    bool observe_job(const std::string& token, const std::string& job,
                     const std::string& provider = {}, const std::string& conversion = {}, int attempt = 0,
                     const std::vector<std::string>& terminal_tasks = {}) {
        if (from_workbench() || id != token || !active() || job.empty() || !design_reserved) return false;
        if (attempt < generation_attempt) return false;
        const auto terminal = [&](const std::string& task) {
            return !task.empty() && std::find(terminal_tasks.begin(), terminal_tasks.end(), task) != terminal_tasks.end();
        };
        auto next_provider = provider_task_id;
        auto next_conversion = conversion_task_id;
        auto retired = retired_task_ids;
        if (attempt > generation_attempt && terminal(next_provider)) {
            retired.push_back(next_provider);
            next_provider.clear(); next_conversion.clear();
        }
        if (!conversion.empty() && conversion != next_conversion && terminal(next_conversion)) {
            retired.push_back(next_conversion); next_conversion.clear();
        }
        if ((!design_job_id.empty() && design_job_id != job) ||
            (!provider.empty() && !next_provider.empty() && provider != next_provider) ||
            (!conversion.empty() && !next_conversion.empty() && conversion != next_conversion) ||
            (!provider.empty() && std::find(retired.begin(), retired.end(), provider) != retired.end()) ||
            (!conversion.empty() && std::find(retired.begin(), retired.end(), conversion) != retired.end())) return false;
        // Earlier phases can arrive after a remote submission. Missing fields
        // never erase an identity, and conflicting fields change nothing.
        design_job_id = job;
        provider_task_id = std::move(next_provider);
        conversion_task_id = std::move(next_conversion);
        retired_task_ids = std::move(retired);
        if (!provider.empty()) provider_task_id = provider;
        if (!conversion.empty()) conversion_task_id = conversion;
        generation_attempt = attempt;
        return true;
    }
    bool reserve_model(const std::string& token, const std::string& job) {
        if (from_workbench() || !current(token, Stage::Model) || model_reserved || design_job_id.empty() || job != design_job_id)
            return false;
        model_reserved = true; model_job_id = job; return true;
    }
    bool accept_repair(const std::string& source, const std::string& candidate,
                       bool accepted_and_saved, bool identities_match) {
        // A manual acceptance may finish while automation is paused. Record
        // the published identity, but leave authorization and pause untouched.
        if (!active() || failed || stage != Stage::Check || !accepted_and_saved || !identities_match ||
            source.empty() || candidate.empty() || source != model_hash) return false;
        repair_source_hash = source; repair_result_hash = candidate; repair_saved = true;
        repair_checked = true; native_repair_required = false;
        model_hash = candidate; return true;
    }
    bool accept_source_check(const std::string& hash, nlohmann::json report = nlohmann::json::object()) {
        if (!running() || stage != Stage::Check || hash.empty() || hash != model_hash) return false;
        if (report.contains("workbench_check_certificate"))
            workbench_check_certificate = report.at("workbench_check_certificate");
        else workbench_check_certificate = nullptr;
        source_checked = true; source_check_hash = hash; source_check = std::move(report);
        return true;
    }
    bool begin_partition(std::string metadata_path) {
        if (!current(id, Stage::Partition) || !source_checked || metadata_path.empty() ||
            partition_pending || partition_committed)
            return false;
        partition_pending = true; partition_committed = false;
        pending_partition_metadata_path = std::move(metadata_path);
        native_portrait_partition = nullptr; partition_build_report = nullptr;
        partition_topology_hash.clear();
        return true;
    }
    bool commit_partition(const nlohmann::json& reference, const std::string& topology_hash,
                          const std::string& digest, const nlohmann::json& report,
                          const nlohmann::json& certificate = nlohmann::json{}) {
        if (!current(id, Stage::Partition) || !partition_pending ||
            !valid_native_partition_reference(reference, digest, topology_hash) ||
            !report.is_object()) return false;
        native_portrait_partition = reference;
        partition_topology_hash = topology_hash;
        basis_signature = digest;
        partition_build_report = report;
        workbench_check_certificate = certificate;
        partition_pending = false; partition_committed = true; partition_completed = true;
        pending_partition_metadata_path.clear();
        repair_checked = repair_saved = false;
        repair_source_hash.clear(); repair_result_hash.clear();
        native_repair_required = false;
        return true;
    }
    bool fail_partition(const nlohmann::json& report) {
        if (!current(id, Stage::Partition) || !partition_pending || partition_committed || !report.is_object())
            return false;
        partition_pending = false;
        pending_partition_metadata_path.clear();
        partition_build_report = report;
        return true;
    }
    bool activate_partition(const std::string& path, const std::string& hash,
                            const std::string& geometry, const std::string& appearance) {
        if (!current(id, Stage::Partition) || !partition_committed ||
            !valid_native_partition_reference(native_portrait_partition, basis_signature, partition_topology_hash) ||
            !same_model_path(path, native_portrait_partition.at("carrier_path").get<std::string>()) ||
            hash != native_portrait_partition.at("carrier_sha256").get<std::string>() ||
            geometry != native_portrait_partition.at("geometry_id").get<std::string>() ||
            appearance != native_portrait_partition.at("appearance_id").get<std::string>()) return false;
        model_path = path; model_hash = hash; geometry_id = geometry; appearance_id = appearance;
        return true;
    }
    bool commit_colored_carrier(const nlohmann::json& reference,
                                const nlohmann::json& semantics,
                                const nlohmann::json& reduction,
                                const nlohmann::json& slots,
                                const nlohmann::json& check,
                                const std::string& path, const std::string& hash,
                                const std::string& geometry, const std::string& appearance,
                                const nlohmann::json& certificate = nlohmann::json{}) {
        const size_t region_count = semantics.is_object() && semantics.contains("regions") &&
            semantics.at("regions").is_array() ? semantics.at("regions").size() : 0;
        const std::string reduction_digest = reduction.is_object()
            ? reduction.value("mapping_digest", std::string{}) : std::string{};
        if (!current(id, Stage::SaveColor) || !partition_committed || colored_saved ||
            !valid_native_partition_reference(reference, basis_signature, partition_topology_hash) ||
            !valid_region_semantics(semantics, basis_signature) ||
            !valid_native_color_reduction(reduction, basis_signature, region_count) ||
            !valid_final_slot_binding(slots, basis_signature, reduction_digest,
                region_count, size_t(model_color_count)) ||
            !valid_role_binding(semantics, slots, size_t(model_color_count)) ||
            semantics.at("face_count") != reference.at("cache_reference").at("face_count") ||
            !valid_post_color_check(check, reference) || path.empty() ||
            !same_model_path(path, reference.at("carrier_path").get<std::string>()) ||
            hash != reference.at("carrier_sha256").get<std::string>() ||
            geometry != reference.at("geometry_id").get<std::string>() ||
            appearance != reference.at("appearance_id").get<std::string>()) return false;
        for (const char* key : {"algorithm", "partition_digest", "topology_hash",
                "source_path", "source_sha256", "appearance_path", "appearance_sha256",
                "cache_path", "cache_sha256",
                "geometry_id", "appearance_id", "cache_reference"})
            if (!native_portrait_partition.contains(key) ||
                native_portrait_partition.at(key) != reference.at(key)) return false;
        native_portrait_partition = reference;
        semantic_binding = nullptr;
        region_semantics = semantics;
        native_color_reduction = reduction;
        final_slot_binding = slots;
        post_color_check = check;
        workbench_check_certificate = certificate;
        model_path = path; model_hash = hash; geometry_id = geometry; appearance_id = appearance;
        colored_saved = true;
        return true;
    }
    bool slice_complete(const std::string& token, const std::string& revision,
                        bool completed, bool valid_result, const std::string& gcode) {
        if (!current(token, Stage::Slice) || !slice_started || revision.empty() || revision != slice_revision ||
            !completed || !valid_result || gcode.empty()) return false;
        gcode_path = gcode; stage = Stage::PrintReady; return true;
    }
    bool bind_slice_input(const std::string& token, const std::string& revision) {
        if (!current(token, Stage::Slice) || slice_started || revision.empty()) return false;
        slice_revision = revision; slice_started = true; return true;
    }
};

inline bool ColorCandidateIdentity::owns(const State& flow, const std::string& asset_id,
    const std::string& path, const std::string& source_hash, const std::string& candidate_hash,
    const std::string& geometry, const std::string& appearance) const
{
    return valid() && flow.from_workbench() && flow.current(flow_id, Stage::SaveColor) &&
        color_asset_id == asset_id && color_asset_id == flow.color_asset_id() &&
        same_model_path(candidate_path, path) && source_sha256 == source_hash &&
        source_sha256 == flow.model_hash && candidate_sha256 == candidate_hash &&
        geometry_id == geometry && geometry_id == flow.geometry_id &&
        appearance_id == appearance && appearance_id == flow.appearance_id &&
        partition_digest == flow.basis_signature && flow.partition_committed &&
        topology_hash == flow.partition_topology_hash &&
        valid_native_partition_reference(flow.native_portrait_partition,
            flow.basis_signature, flow.partition_topology_hash);
}

inline nlohmann::json encode(const State& s) {
    return {{"schema", s.from_workbench() ? s.record_version >= 7 ? "orca.portrait-print/v7" :
            s.record_version >= 6 ? "orca.portrait-print/v6" :
            s.record_version >= 5 ? "orca.portrait-print/v5" :
            s.record_version >= 4 ? "orca.portrait-print/v4" : "orca.portrait-print/v3" : "orca.portrait-print/v2"},
        {"id", s.id}, {"stage", int(s.stage)},
        {"repair_checked", s.repair_checked}, {"partition_completed", s.partition_completed},
        {"source_checked", s.source_checked}, {"source_check_hash", s.source_check_hash},
        {"source_check", s.source_check},
        {"partition_pending", s.partition_pending}, {"partition_committed", s.partition_committed},
        {"partition_topology_hash", s.partition_topology_hash},
        {"pending_partition_metadata_path", s.pending_partition_metadata_path},
        {"native_portrait_partition", s.native_portrait_partition},
        {"semantic_binding", s.semantic_binding},
        {"region_semantics", s.region_semantics},
        {"native_color_reduction", s.native_color_reduction},
        {"final_slot_binding", s.final_slot_binding},
        {"partition_build_report", s.partition_build_report},
        {"post_color_check", s.post_color_check},
        {"workbench_check_certificate", s.workbench_check_certificate},
        {"boundary_algorithm_version", s.boundary_algorithm_version},
        {"entry", s.entry}, {"colored_saved", s.colored_saved}, {"semantic_applied", s.semantic_applied},
        {"person_detected", s.person_detected}, {"evidence_status", s.evidence_status}, {"application_status", s.application_status},
        {"geometry_id", s.geometry_id}, {"appearance_id", s.appearance_id}, {"edit_revision", s.edit_revision},
        {"region_binding_digest", s.region_binding_digest}, {"base_binding", s.base_binding},
        {"round_base_enabled", s.round_base_enabled}, {"base_height_mm", 3.0}, {"base_overlap_mm", 0.2},
        {"native_repair_required", s.native_repair_required}, {"native_repair_completed", s.native_repair_completed},
        {"native_geometry_id", s.native_geometry_id}, {"preview_result_id", s.preview_result_id},
        {"semantic_status", s.semantic_status}, {"recovery_project", s.recovery_project},
        {"generation_attempt", s.generation_attempt}, {"palette_retries", s.palette_retries},
        {"design_retries", s.design_retries}, {"model_retries", s.model_retries}, {"conversion_retries", s.conversion_retries},
        {"notices", s.notices}, {"retired_task_ids", s.retired_task_ids},
        {"paused", s.paused}, {"failed", s.failed}, {"reason", s.reason},
        {"design_reserved", s.design_reserved}, {"model_reserved", s.model_reserved},
        {"palette_confirmed", s.palette_confirmed}, {"fallback_confirmed", s.fallback_confirmed},
        {"near_color_fallback", s.near_color_fallback}, {"design_risk_accepted", s.design_risk_accepted},
        {"model_risk_accepted", s.model_risk_accepted}, {"repair_saved", s.repair_saved},
        {"slice_started", s.slice_started},
        {"requested_count", s.requested_count}, {"final_count", s.final_count},
        {"model_color_count", s.model_color_count},
        {"physical_slot_count", s.physical_slot_count},
        {"support_slot_count", s.support_slot_count},
        {"request_id", s.request_id}, {"design_job_id", s.design_job_id}, {"model_job_id", s.model_job_id},
        {"provider_task_id", s.provider_task_id}, {"conversion_task_id", s.conversion_task_id},
        {"source_image_hash", s.source_image_hash}, {"source_image_copy", s.source_image_copy},
        {"authorization_id", s.authorization_id}, {"final_palette", s.final_palette},
        {"design_path", s.design_path}, {"original_model_path", s.original_model_path},
        {"original_model_hash", s.original_model_hash}, {"model_path", s.model_path}, {"model_hash", s.model_hash},
        {"repair_source_hash", s.repair_source_hash}, {"repair_result_hash", s.repair_result_hash},
        {"basis_signature", s.basis_signature}, {"suite_revision", s.suite_revision},
        {"workspace_id", s.workspace_id}, {"project_revision", s.project_revision}, {"slice_revision", s.slice_revision}, {"gcode_path", s.gcode_path},
        {"frozen", s.frozen}};
}

inline bool decode(const nlohmann::json& j, State& result) {
    try {
        const auto schema = j.at("schema").get<std::string>();
        if (schema != "orca.portrait-print/v1" && schema != "orca.portrait-print/v2" &&
            schema != "orca.portrait-print/v3" && schema != "orca.portrait-print/v4" &&
            schema != "orca.portrait-print/v5" && schema != "orca.portrait-print/v6" && schema != "orca.portrait-print/v7") return false;
        const int stage = j.at("stage").get<int>();
        if (stage < int(Stage::Project) || stage > int((schema == "orca.portrait-print/v4" ||
            schema == "orca.portrait-print/v5" || schema == "orca.portrait-print/v6" || schema == "orca.portrait-print/v7")
                ? Stage::Base : Stage::PrintReady)) return false;
        State s;
        s.record_version = schema == "orca.portrait-print/v7" ? 7 : schema == "orca.portrait-print/v6" ? 6 :
            schema == "orca.portrait-print/v5" ? 5 :
            schema == "orca.portrait-print/v4" ? 4 : schema == "orca.portrait-print/v3" ? 3 : 2;
        s.id = j.at("id").get<std::string>(); s.stage = Stage(stage);
        if (s.id.empty() || s.id.size() > 128 || s.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
            return false;
        s.frozen = j.at("frozen");
        if (s.record_version >= 3) {
            s.entry = j.at("entry").get<std::string>();
            s.geometry_id = j.at("geometry_id").get<std::string>();
            s.appearance_id = j.at("appearance_id").get<std::string>();
            s.edit_revision = j.at("edit_revision").get<uint64_t>();
            s.colored_saved = j.at("colored_saved").get<bool>();
            s.semantic_applied = j.at("semantic_applied").get<bool>();
            s.person_detected = j.value("person_detected", false);
            s.evidence_status = j.value("evidence_status", std::string{});
            s.application_status = j.value("application_status", std::string{});
            s.region_binding_digest = j.at("region_binding_digest").get<std::string>();
            s.base_binding = j.at("base_binding");
            if (!s.base_binding.is_null()) {
                AI::ModelBaseBinding binding;
                if (!AI::ModelBaseBinding::decode(s.base_binding, binding)) return false;
            }
            const bool awaiting_original_identity = s.record_version >= 5 && s.stage == Stage::Check &&
                !s.source_checked && s.geometry_id.empty() && s.appearance_id.empty();
            if (!s.from_workbench() || ((!awaiting_original_identity) &&
                (s.geometry_id.empty() || s.appearance_id.empty())) ||
                (s.stage >= Stage::Palette && s.stage <= Stage::Model) || !s.frozen.is_object()) return false;
        } else if (!valid_frozen(s.frozen)) return false;
#define PORTRAIT_READ(field) s.field = j.at(#field).get<decltype(s.field)>()
        PORTRAIT_READ(failed); PORTRAIT_READ(reason);
        PORTRAIT_READ(design_reserved); PORTRAIT_READ(model_reserved); PORTRAIT_READ(palette_confirmed);
        PORTRAIT_READ(fallback_confirmed); PORTRAIT_READ(near_color_fallback);
        PORTRAIT_READ(design_risk_accepted); PORTRAIT_READ(model_risk_accepted); PORTRAIT_READ(repair_saved);
        PORTRAIT_READ(slice_started);
        PORTRAIT_READ(requested_count); PORTRAIT_READ(final_count);
        if (s.record_version >= 6) {
            PORTRAIT_READ(model_color_count);
            PORTRAIT_READ(physical_slot_count);
            PORTRAIT_READ(support_slot_count);
        } else {
            s.model_color_count = s.final_count;
            s.physical_slot_count = s.final_count;
            s.support_slot_count = 0;
        }
        PORTRAIT_READ(request_id); PORTRAIT_READ(design_job_id); PORTRAIT_READ(model_job_id);
        PORTRAIT_READ(provider_task_id); PORTRAIT_READ(conversion_task_id);
        PORTRAIT_READ(source_image_hash); PORTRAIT_READ(source_image_copy); PORTRAIT_READ(design_path);
        PORTRAIT_READ(authorization_id); PORTRAIT_READ(final_palette);
        PORTRAIT_READ(original_model_path); PORTRAIT_READ(original_model_hash);
        PORTRAIT_READ(model_path); PORTRAIT_READ(model_hash);
        PORTRAIT_READ(repair_source_hash); PORTRAIT_READ(repair_result_hash);
        PORTRAIT_READ(basis_signature); PORTRAIT_READ(suite_revision); PORTRAIT_READ(project_revision);
        PORTRAIT_READ(workspace_id);
        PORTRAIT_READ(slice_revision); PORTRAIT_READ(gcode_path);
#undef PORTRAIT_READ
        s.round_base_enabled = j.value("round_base_enabled", true);
        if (j.value("base_height_mm", 3.0) != 3.0 || j.value("base_overlap_mm", 0.2) != 0.2) return false;
        s.native_repair_required = j.value("native_repair_required", true);
        s.native_repair_completed = j.value("native_repair_completed", false);
        s.repair_checked = s.record_version >= 4 && j.value("repair_checked", false);
        s.partition_completed = s.record_version >= 4 && j.value("partition_completed", false);
        s.source_checked = s.record_version >= 5 && j.value("source_checked", false);
        s.source_check_hash = s.record_version >= 5 ? j.value("source_check_hash", std::string{}) : std::string{};
        s.source_check = s.record_version >= 5 ? j.value("source_check", nlohmann::json{}) : nlohmann::json{};
        s.partition_pending = s.record_version >= 5 && j.value("partition_pending", false);
        s.partition_committed = s.record_version >= 5 && j.value("partition_committed", false);
        s.partition_topology_hash = s.record_version >= 5 ?
            j.value("partition_topology_hash", std::string{}) : std::string{};
        s.pending_partition_metadata_path = s.record_version >= 5 ?
            j.value("pending_partition_metadata_path", std::string{}) : std::string{};
        s.native_portrait_partition = s.record_version >= 5 ?
            j.value("native_portrait_partition", nlohmann::json{}) : nlohmann::json{};
        s.semantic_binding = s.record_version >= 5 ?
            j.value("semantic_binding", nlohmann::json{}) : nlohmann::json{};
        s.region_semantics = s.record_version >= 6 ?
            j.value("region_semantics", nlohmann::json{}) : nlohmann::json{};
        s.native_color_reduction = s.record_version >= 6 ?
            j.value("native_color_reduction", nlohmann::json{}) : nlohmann::json{};
        s.final_slot_binding = s.record_version >= 6 ?
            j.value("final_slot_binding", nlohmann::json{}) : nlohmann::json{};
        s.partition_build_report = s.record_version >= 5 ?
            j.value("partition_build_report", nlohmann::json{}) : nlohmann::json{};
        s.post_color_check = s.record_version >= 5 ?
            j.value("post_color_check", nlohmann::json{}) : nlohmann::json{};
        s.workbench_check_certificate = s.record_version >= 5 ?
            j.value("workbench_check_certificate", nlohmann::json{}) : nlohmann::json{};
        s.boundary_algorithm_version = j.value("boundary_algorithm_version", uint32_t(0));
        s.native_geometry_id = j.value("native_geometry_id", std::string{});
        s.preview_result_id = j.value("preview_result_id", std::string{});
        s.semantic_status = j.value("semantic_status", std::string{});
        s.recovery_project = j.value("recovery_project", std::string{});
        s.generation_attempt = j.value("generation_attempt", 0);
        s.palette_retries = j.value("palette_retries", 0); s.design_retries = j.value("design_retries", 0);
        s.model_retries = j.value("model_retries", 0); s.conversion_retries = j.value("conversion_retries", 0);
        s.notices = j.value("notices", std::vector<std::string>{});
        s.retired_task_ids = j.value("retired_task_ids", std::vector<std::string>{});
        if (s.generation_attempt < 0 || s.palette_retries < 0 || s.design_retries < 0 ||
            s.model_retries < 0 || s.conversion_retries < 0) return false;
        if (s.requested_count < 4 || s.requested_count > 6 ||
            (s.final_count != 0 && (s.final_count < 4 || s.final_count > 6)) ||
            (s.record_version >= 6 && (s.model_color_count != 5 ||
                s.physical_slot_count != 6 || s.support_slot_count != 1 ||
                s.model_color_count + s.support_slot_count != s.physical_slot_count)) ||
            (!s.final_palette.empty() && s.final_palette.size() != size_t(s.final_count)) ||
            (s.design_reserved && s.request_id.empty()) ||
            (s.model_reserved && (s.model_job_id.empty() || s.model_job_id != s.design_job_id))) return false;
        if (s.from_workbench() && (s.original_model_hash.empty() || s.model_hash.empty() ||
            s.model_path.empty() || s.design_reserved || s.model_reserved)) return false;
        if (s.from_workbench() && s.record_version == 4 &&
            ((s.partition_completed && (!s.repair_checked || s.basis_signature.empty())) ||
             (s.repair_checked && (!s.repair_saved || s.repair_result_hash.empty())))) return false;
        if (s.from_workbench() && s.record_version >= 7 &&
            ((s.source_checked && (s.source_check_hash.empty() || s.source_check_hash != s.original_model_hash)) ||
             (s.partition_pending && (s.partition_committed || s.pending_partition_metadata_path.empty())) ||
             (s.partition_committed && (!s.partition_completed || s.partition_pending ||
                !valid_native_partition_reference(s.native_portrait_partition,
                    s.basis_signature, s.partition_topology_hash))) ||
             (s.partition_completed && !s.partition_committed) ||
             (workbench_order(s.stage) >= workbench_order(Stage::Color) && !s.partition_committed) ||
             (s.colored_saved && (!valid_region_semantics(s.region_semantics, s.basis_signature) ||
                !valid_native_color_reduction(s.native_color_reduction, s.basis_signature,
                    s.region_semantics.at("regions").size()) ||
                !valid_final_slot_binding(s.final_slot_binding, s.basis_signature,
                    s.native_color_reduction.at("mapping_digest").get<std::string>(),
                    s.region_semantics.at("regions").size(), size_t(s.model_color_count)) ||
                !valid_role_binding(s.region_semantics, s.final_slot_binding, size_t(s.model_color_count)) ||
                s.region_semantics.at("face_count") != s.native_portrait_partition.at("cache_reference").at("face_count") ||
                !valid_post_color_check(s.post_color_check, s.native_portrait_partition))))) return false;
        s.restored = true; s.authorized = false; s.paused = s.active();
        if (s.active() && !s.failed) s.reason = "流程已恢复，请核对当前工程后点击“继续一键流程”。";
        result = std::move(s); return true;
    } catch (const std::exception&) { return false; }
}

} // namespace Slic3r::GUI::PortraitPrint
