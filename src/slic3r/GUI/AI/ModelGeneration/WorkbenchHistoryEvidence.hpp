#pragma once

#include "WorkbenchArtifactValidation.hpp"
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

} // namespace Slic3r::GUI
