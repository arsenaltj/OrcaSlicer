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
    if (!request->expected_model_sha256.empty() &&
        AI::model_artifact_sha256(original.model_path) != request->expected_model_sha256) {
        error = "模型内容已变化，请重新打开模型后重试。";
        return false;
    }
    using Stage = ModelHistoryOpenEvent::Stage;
    const auto progress = [&](Stage stage) {
        if (!canceled() && request->progress) request->progress(stage);
    };
    progress(Stage::Validating);
    // A creator may hand off a downloaded mirror while the sidebar registers
    // the task-directory copy. Such requests have no sidebar index yet. Build
    // it here, or refresh a stale one, through the same background reader.
    if (!request->restoring && (!request->repairs || !request->repairs->current())) {
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

// The resolver may replace its shared_ptr while selecting a repaired artifact.
// Return that final pointer across the worker/GUI boundary, never capture the
// initially empty allocation in a separately constructed completion handler.
struct ModelPreviewLoadResult {
    std::shared_ptr<ModelPreview3D::PreparedModel> prepared;
    std::string error;
    const char* stage {"reading"};
};

inline ModelPreviewLoadResult prepare_model_preview_load(const boost::filesystem::path& path,
    const boost::filesystem::path& metadata_path,
    const std::shared_ptr<ModelHistoryMetadata>& history_metadata,
    const std::shared_ptr<ModelHistoryOpenRequest>& history_open,
    const std::function<bool()>& stopped, bool share_exact_vertices)
{
    ModelPreviewLoadResult result;
    result.prepared = std::make_shared<ModelPreview3D::PreparedModel>();
    try {
        const bool ready = history_open
            ? prepare_history_preview(result.prepared, history_open, history_metadata, result.error, stopped)
            : ModelPreview3D::prepare_model(path, *result.prepared, result.error, {}, metadata_path,
                true, stopped, history_metadata.get());
        if (!ready) {
            if (result.error.empty()) result.error = "模型预览准备已取消。";
            return result;
        }
        result.stage = "render_geometry";
        if (history_open && history_open->progress && !stopped())
            history_open->progress(ModelHistoryOpenEvent::Stage::Preview);
        result.prepared->prepare_render_geometry(stopped, share_exact_vertices);
        if (stopped()) { result.error = "模型预览准备已取消。"; return result; }
        if (history_open) {
            result.stage = "file_validation";
            history_open->model_hash = result.prepared->artifact_sha256;
            const auto record = history_open->root / "downloads" /
                ("orcaslicer-ai-" + history_open->entry.job_id + ".json");
            if (!history_open->check) history_open->record_hash = AI::model_artifact_sha256(record);
            auto proof = std::make_shared<VerifiedModelFiles>();
            if (!proof->verify(history_open->entry.model_path, history_open->model_hash) ||
                !proof->verify(record, history_open->record_hash, true))
                result.error = "历史模型或检查凭证在加载期间发生变化，请重新打开。";
            else history_open->verified_files = std::move(proof);
        }
        if (result.error.empty()) result.stage = "prepared";
    } catch (const std::exception& failure) { result.error = failure.what(); }
    return result;
}

// A failed presentation does not undo an already published check. Resolve it
// through the same read-only index and full evidence validation as history,
// before the native producer is allowed to create another carrier.
inline std::optional<ModelPreviewLoadResult> prepare_published_workbench_retry(
    const std::shared_ptr<ModelHistoryOpenRequest>& request,
    const std::function<bool()>& stopped, bool share_exact_vertices)
{
    if (stopped()) return {};
    const auto entries = ModelLibraryRepository::read(request->root, request->canceled);
    if (entries.empty() || stopped()) return {};
    request->repairs = entries.front().repair_index;
    if (!request->repairs || request->repairs->candidates(request->entry.model_path).empty()) return {};
    auto metadata = std::make_shared<ModelHistoryMetadata>();
    auto result = prepare_model_preview_load(request->entry.model_path, {}, metadata, request,
        stopped, share_exact_vertices);
    if (stopped() || !request->check) return {};
    if (result.error.empty() && (!result.prepared->native_workbench_checkpoint ||
        !result.prepared->native_portrait_partition || !result.prepared->native_portrait_partition->valid()))
        result.error = "已发布检查成果的预览数据未能完整恢复，请重新打开；原成果已保留。";
    // A valid publication with a failed CPU presentation remains a reuse
    // result. Its error must not fall through into another expensive repair.
    return result;
}

} // namespace Slic3r::GUI
