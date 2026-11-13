#pragma once
#include "WorkbenchHistoryEvidence.hpp"
#include "VerifiedModelFiles.hpp"
#include <memory>

namespace Slic3r::GUI {
// Immutable CPU result, prepared alongside the model. Refreshing controls
// consumes this proof; it never reopens/hashes the carrier or its source chain.
struct NativeWorkbenchCheckpoint {
    nlohmann::json reference, report, certificate, commit;
};
inline std::shared_ptr<const NativeWorkbenchCheckpoint> prepare_native_workbench_checkpoint(
    const boost::filesystem::path& model, const boost::filesystem::path& record,
    const nlohmann::json& metadata, const std::string& hash, const std::string& geometry,
    const std::string& appearance, const std::string& partition,
    const nlohmann::json& preview_reference, const std::function<bool()>& canceled = {},
    std::shared_ptr<const VerifiedModelFiles>* installation_files = nullptr)
{
    try {
        auto files = std::make_shared<VerifiedModelFiles>();
        const auto record_hash = AI::model_artifact_sha256(record);
        if (!files->verify(model, hash) || !files->verify(record, record_hash)) return {};
        boost::filesystem::ifstream input(record, std::ios::binary);
        if (nlohmann::json::parse(input) != metadata) return {};
        const auto evidence = workbench_history_evidence_location(model, record, metadata, canceled);
        if (!files->verify(evidence.model, hash) || !files->verify(evidence.metadata, record_hash)) return {};
        const auto root = evidence.metadata.parent_path().filename() == "downloads"
            ? evidence.metadata.parent_path().parent_path() : evidence.metadata.parent_path();
        if (metadata.contains("source_model")) {
            const boost::filesystem::path source(boost::nowide::widen(metadata.at("source_model").get<std::string>()));
            if (!files->verify(source.is_absolute() ? source : root / source,
                    metadata.at("source_sha256").get<std::string>())) return {};
        }
        nlohmann::json reference;
        if (!restorable_native_partition_metadata_matches(metadata, evidence.metadata,
                evidence.model, hash, geometry, appearance, reference) ||
            !WorkbenchArtifacts::valid_native_partition_reference(reference, partition) ||
            (!preview_reference.is_null() && preview_reference != reference)) return {};
        auto result = std::make_shared<NativeWorkbenchCheckpoint>();
        result->report = metadata.value("partition_build_report", nlohmann::json{});
        result->certificate = metadata.value("workbench_check_certificate", nlohmann::json{});
        if (result->report.value("schema", std::string{}) != "orca.native-workbench-partition-build-report/v1" ||
            !result->report.value("success", false) ||
            !WorkbenchArtifacts::valid_workbench_check_certificate(result->certificate,
                WorkbenchCheckPurpose::PortraitPostColorReadOnly, hash, geometry, appearance,
                boost::nowide::narrow(evidence.metadata.wstring()), metadata, partition,
                reference.at("topology_hash").get<std::string>())) return {};
        if ((canceled && canceled()) || !files->current()) return {};
        result->reference = std::move(reference); result->commit = metadata;
        if (installation_files) *installation_files = std::move(files);
        return result;
    } catch (...) { return {}; }
}
}
