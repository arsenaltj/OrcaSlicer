#pragma once

#include "WorkbenchArtifactValidation.hpp"
#include "PortraitNativeCheckpoint.hpp"
#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"
#include <boost/nowide/convert.hpp>

namespace Slic3r::GUI {

struct WorkbenchHistoryEvidenceLocation {
    boost::filesystem::path model, metadata;
};

// A copied history record still names its original publication. Resolve that
// location only while BOTH records and BOTH models are unchanged. The caller
// must then validate the original certificate, including its complete payload;
// this does not reissue a certificate or alter the copied record.
inline WorkbenchHistoryEvidenceLocation workbench_history_evidence_location(
    const boost::filesystem::path& model, const boost::filesystem::path& record,
    const nlohmann::json& metadata, const std::function<bool()>& canceled = {})
{
    const WorkbenchHistoryEvidenceLocation current {model, record};
    try {
        const auto checkpoint = [&] {
            if (canceled && canceled()) throw std::runtime_error("History evidence resolution canceled.");
        };
        const auto& certificate = metadata.at("workbench_check_certificate");
        const boost::filesystem::path published_record(boost::nowide::widen(
            certificate.at("metadata_path").get<std::string>()));
        checkpoint();
        const auto root = published_record.parent_path().filename() == "downloads"
            ? published_record.parent_path().parent_path() : published_record.parent_path();
        const boost::filesystem::path relative(boost::nowide::widen(
            metadata.at("model_path").get<std::string>()));
        if (relative.empty()) return current;
        const auto published_model = relative.is_absolute() ? relative : root / relative;
        if (WorkbenchArtifacts::same_model_path(boost::nowide::narrow(record.wstring()),
                boost::nowide::narrow(published_record.wstring())) &&
            WorkbenchArtifacts::same_model_path(boost::nowide::narrow(model.wstring()),
                boost::nowide::narrow(published_model.wstring()))) return current;
        const auto safe = [](const boost::filesystem::path& path) {
            boost::system::error_code error;
            auto prefix = path.root_path();
            for (const auto& part : path.relative_path()) {
                if (part == "..") return false;
                prefix /= part;
                if (boost::filesystem::is_symlink(prefix, error) || error) return false;
            }
            return boost::filesystem::is_regular_file(path, error) && !error;
        };
        if (!safe(model) || !safe(record) || !safe(published_model) || !safe(published_record) ||
            !WorkbenchArtifacts::same_model_path(boost::nowide::narrow(
                AI::beauty_metadata_path(published_model, root).wstring()),
                boost::nowide::narrow(published_record.wstring()))) return current;
        const auto record_hash = AI::model_artifact_sha256(record);
        const auto model_hash = metadata.at("model_sha256").get<std::string>();
        if (!WorkbenchArtifacts::valid_sha256(model_hash) || record_hash.empty() ||
            boost::filesystem::file_size(record) > 128ULL * 1024 * 1024 ||
            AI::model_artifact_sha256(published_record) != record_hash ||
            AI::model_artifact_sha256(model) != model_hash ||
            AI::model_artifact_sha256(published_model) != model_hash) return current;
        boost::filesystem::ifstream input(record, std::ios::binary);
        if (nlohmann::json::parse(input) != metadata) return current;
        checkpoint();
        if (AI::model_artifact_sha256(record) != record_hash ||
            AI::model_artifact_sha256(published_record) != record_hash) return current;
        return {published_model, published_record};
    } catch (const std::exception&) {
        return current;
    }
}

// Shared by background history resolution and the live workbench. No repair,
// inspection or certificate rewriting occurs here.
inline bool read_validated_workbench_history_check(
    const boost::filesystem::path& model, const boost::filesystem::path& metadata_path,
    const std::string& model_hash, const std::string& geometry_id,
    const std::string& appearance_id, WorkbenchCheckResult& out,
    const std::function<bool()>& canceled = {}, std::string* record_identity = nullptr)
{
    namespace PP = WorkbenchArtifacts;
    const auto u8 = [](const boost::filesystem::path& path) { return boost::nowide::narrow(path.wstring()); };
    try {
        if ((canceled && canceled()) || !PP::valid_sha256(model_hash)) return false;
        boost::system::error_code error;
        const auto bytes = boost::filesystem::file_size(metadata_path, error);
        if (error || !bytes || bytes > 128ULL * 1024 * 1024 ||
            boost::filesystem::is_symlink(metadata_path, error) || error) return false;
        const auto record_hash = AI::model_artifact_sha256(metadata_path);
        boost::filesystem::ifstream input(metadata_path, std::ios::binary);
        const auto metadata = nlohmann::json::parse(input);
        if (!metadata.is_object() || (canceled && canceled()) || record_hash.empty() ||
            AI::model_artifact_sha256(metadata_path) != record_hash ||
            AI::model_artifact_sha256(model) != model_hash) return false;
    const auto complete = [&](const nlohmann::json& certificate) {
        if ((canceled && canceled()) || AI::model_artifact_sha256(metadata_path) != record_hash ||
            AI::model_artifact_sha256(model) != model_hash ||
            !PP::decode_workbench_check_certificate(certificate, out)) return false;
        if (record_identity) *record_identity = record_hash;
        return true;
    };
    const auto evidence = workbench_history_evidence_location(model, metadata_path, metadata);
    const auto embedded = metadata.value("workbench_check_certificate", nlohmann::json{});
    if (!metadata.contains("native_portrait_partition") &&
        metadata.value("source", std::string{}) == "local_finishing" &&
        metadata.value("schema_version", 0) == 4 &&
        metadata.value("model_sha256", std::string{}) == model_hash &&
        PP::valid_workbench_check_certificate(embedded, WorkbenchCheckPurpose::SavedModelReadOnly,
            model_hash, geometry_id, appearance_id, u8(evidence.metadata), metadata)) {
        WorkbenchCheckResult restored;
        if (PP::decode_workbench_check_certificate(embedded, restored) &&
            workbench_topology_safe(restored) &&
            metadata.value("triangle_count", size_t(0)) == restored.before_faces)
            return complete(embedded);
        return false;
    }
    const bool require_post_color = metadata.contains("post_color_check");
    nlohmann::json reference;
    if (!portrait_partition_metadata_matches(metadata, evidence.metadata, evidence.model,
            model_hash, geometry_id, appearance_id, require_post_color, reference)) return false;

    const auto certificate_valid = [&](const nlohmann::json& certificate) {
        if (!PP::valid_workbench_check_certificate(certificate,
                WorkbenchCheckPurpose::PortraitPostColorReadOnly, model_hash, geometry_id,
                appearance_id, u8(evidence.metadata), metadata,
                reference.at("partition_digest").get<std::string>(),
                reference.at("topology_hash").get<std::string>())) return false;
        WorkbenchCheckResult restored;
        if (!PP::decode_workbench_check_certificate(certificate, restored) ||
            !workbench_topology_safe(restored) ||
            metadata.at("triangle_count").get<size_t>() != restored.before_faces) return false;
        if (require_post_color) {
            const auto& check = metadata.at("post_color_check");
            if (check.at("faces").get<size_t>() != restored.before_faces ||
                check.at("vertices").get<size_t>() != restored.before_vertices ||
                check.at("boundary_edges").get<size_t>() != restored.boundary_edges ||
                check.at("nonmanifold_edges").get<size_t>() != restored.nonmanifold_edges ||
                check.at("nonmanifold_vertices").get<size_t>() != restored.nonmanifold_vertices ||
                check.at("degenerate_faces").get<size_t>() != restored.degenerate_faces ||
                check.at("duplicate_faces").get<size_t>() != restored.duplicate_faces ||
                check.at("inconsistent_edges").get<size_t>() != restored.inconsistent_edges ||
                check.at("self_intersections_checked").get<bool>() !=
                    restored.self_intersections_checked ||
                check.at("self_intersections").get<size_t>() != restored.self_intersections)
                return false;
        }
        return true;
    };

    return certificate_valid(embedded) && complete(embedded);
    } catch (const std::exception&) { return false; }
}

} // namespace Slic3r::GUI
