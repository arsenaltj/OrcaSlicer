#pragma once
#include "ModelHistoryOpen.hpp"
#include "ModelHistoryRepairIndex.hpp"
#include "ModelLibraryRepository.hpp"
#include "ModelPreview3D.hpp"
#include "ModelGenerationPresentation.hpp"
#include "WorkbenchHistoryEvidence.hpp"

namespace Slic3r::GUI {
inline bool prepare_history_preview(std::shared_ptr<ModelPreview3D::PreparedModel>& prepared,
    const std::shared_ptr<ModelHistoryOpenRequest>& request,
    const std::shared_ptr<ModelHistoryMetadata>& history_metadata, std::string& error,
    const std::function<bool()>& canceled)
{
    if (canceled()) return false;
    const auto& root = request->root;
    const auto original = request->entry;
    using Stage = ModelHistoryOpenEvent::Stage;
    const auto progress = [&](Stage stage) {
        if (!canceled() && request->progress) request->progress(stage);
    };
    progress(Stage::Validating);
    // A repair can publish before the sidebar's next scan completes. Refresh
    // only a stale relationship index, on this worker through the same reader.
    if (request->repairs && !request->restoring && !request->repairs->current()) {
        const auto entries = ModelLibraryRepository::read(root, request->canceled);
        if (canceled()) return false;
        request->repairs = entries.empty() ? nullptr : entries.front().repair_index;
    }
    auto original_record = std::make_shared<ModelHistoryMetadata>();
    const auto record_path = [&](const ModelLibraryEntry& entry) {
        return root / "downloads" / ("orcaslicer-ai-" + entry.job_id + ".json");
    };
    const auto prepare = [&](const ModelLibraryEntry& entry,
                             std::shared_ptr<ModelPreview3D::PreparedModel>& model,
                             const std::shared_ptr<ModelHistoryMetadata>& record) {
        model = std::make_shared<ModelPreview3D::PreparedModel>();
        progress(Stage::Reading);
        return ModelPreview3D::prepare_model(entry.model_path, *model, error, {},
            record_path(entry), true, canceled, record.get());
    };
    const auto certified = [&](const ModelLibraryEntry& entry,
                               const ModelPreview3D::PreparedModel& model) {
        WorkbenchCheckResult restored;
        progress(Stage::Validating);
        const auto appearance = model.semantic_source ? model.semantic_source->content_id : model.artifact_sha256;
        if (!read_validated_workbench_history_check(entry.model_path, record_path(entry),
                model.artifact_sha256, model.geometry_id, appearance, restored, canceled, &request->record_hash)) return false;
        request->check = std::move(restored);
        return true;
    };
    const auto candidates = request->repairs && !request->restoring
        ? request->repairs->candidates(original.model_path) : std::vector<ModelHistoryRepairIndex::Node>{};
    bool might_be_certified = false;
    if (!candidates.empty()) {
        boost::filesystem::ifstream input(record_path(original), std::ios::binary);
        const auto summary = nlohmann::json::parse(input, [](int depth, nlohmann::json::parse_event_t event,
                                                            nlohmann::json& value) {
            return event != nlohmann::json::parse_event_t::key || depth != 1 ||
                value.get_ref<const std::string&>() == "workbench_check_certificate";
        }, false);
        might_be_certified = summary.is_object() && summary.contains("workbench_check_certificate");
    }
    const bool prepared_original = candidates.empty() || might_be_certified;
    const bool original_loaded = prepared_original && prepare(original, prepared, original_record);
    const auto original_error = error;
    if (canceled()) return false;
    // A specifically selected checked version always wins, including recolors.
    if (original_loaded && certified(original, *prepared)) {
        *history_metadata = std::move(*original_record);
        return true;
    }
    const auto original_model = prepared;
    if (request->repairs && !request->restoring) {
        for (const auto& candidate : candidates) {
            if (canceled()) return false;
            progress(Stage::Validating);
            if (!request->repairs->current_chain(candidate, original.model_path, canceled)) continue;
            ModelLibraryEntry entry;
            entry.model_path = candidate.model;
            entry.job_id = candidate.job_id;
            auto record = std::make_shared<ModelHistoryMetadata>();
            if (!prepare(entry, prepared, record) || canceled() || !certified(entry, *prepared) ||
                !request->repairs->current_chain(candidate, original.model_path, canceled)) {
                request->check.reset();
                continue;
            }
            const auto& metadata = record->restoration_fields();
            const auto path_field = [&](const char* key) {
                const auto relative = metadata.value(key, std::string{});
                const auto path = root / relative;
                boost::system::error_code ec;
                return !relative.empty() && ModelGenerationPresentation::path_is_inside(root, path) &&
                    boost::filesystem::is_regular_file(path, ec) && !ec ? path : boost::filesystem::path{};
            };
            entry.reference_image_path = path_field("reference_image_path");
            entry.ai_image_path = path_field("ai_image_path");
            entry.color_intent_path = path_field("color_intent_path");
            entry.color_intent_schema = metadata.value("color_intent_schema", std::string{});
            entry.color_intent_sha256 = metadata.value("color_intent_sha256", std::string{});
            entry.palette = metadata.value("palette", std::vector<std::string>{});
            entry.palette_roles = metadata.value("palette_roles", std::map<std::string, std::string>{});
            entry.use_printable_colors = metadata.value("use_printable_colors", false);
            entry.title = wxString::FromUTF8(metadata.value("prompt", candidate.job_id));
            request->entry = std::move(entry);
            *history_metadata = std::move(*record);
            error.clear();
            return true;
        }
    }
    request->check.reset();
    if (!prepared_original) {
        error.clear();
        const bool loaded = prepare(original, prepared, original_record);
        *history_metadata = std::move(*original_record);
        return loaded;
    }
    error = original_error;
    prepared = original_model;
    *history_metadata = std::move(*original_record);
    if (original_loaded) {
        error.clear();
        if (request->restoring && request->previous_check &&
            request->previous_check->model_sha256 == prepared->artifact_sha256)
            request->check = request->previous_check;
    }
    return original_loaded;
}

} // namespace Slic3r::GUI
