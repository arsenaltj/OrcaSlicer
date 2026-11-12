#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelLibraryObservations.hpp"
#include "ModelPreview3D.hpp"
#include "ModelImageDisplayCopy.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Redesign/RedesignMessageDialog.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/log/trivial.hpp>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/notebook.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;

bool ModelGenerationPanel::request_open_workbench_artifact(const WorkbenchOpenRequest& request)
{
    if (!can_replace_model_asset()) return false;
    const auto rejected = [this](const wxString& message) {
        show_redesign_confirmation(this, message, _L("无法打开工作台"), {wxOK});
        return false;
    };
    if (request.artifact.local_path.empty() || request.artifact.job_id.empty() ||
        request.artifact.local_path != request.entry.model_path || request.artifact.job_id != request.entry.job_id ||
        (!request.expected_sha256.empty() && AI::model_artifact_sha256(request.artifact.local_path) != request.expected_sha256))
        return rejected(_L("模型文件或任务信息已变化，当前模型已保留。请重新打开对应结果或刷新历史后重试。"));
    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(request.artifact.local_path, ec))
        return rejected(_L("模型文件已不存在或无法读取，当前模型已保留。请恢复文件或刷新历史后重试。"));
    const auto& entry = request.entry;
    return load_library_entry(entry.model_path, entry.reference_image_path, entry.ai_image_path,
        entry.palette, entry.palette_roles, entry.use_printable_colors, entry.color_intent_path,
        entry.color_intent_schema, entry.color_intent_sha256, entry.job_id, entry.title, false, entry.repair_index,
        {}, request.expected_sha256);
}
bool ModelGenerationPanel::request_open_generated_artifact()
{
    if (!m_generation_artifact_ready || m_artifact_path.empty()) return false;
    WorkbenchOpenRequest request;
    auto& entry = request.entry;
    entry.model_path = m_artifact_path; entry.job_id = m_job_id;
    entry.reference_image_path = m_reference_image_path; entry.ai_image_path = m_raw_preview_path;
    entry.palette = m_job_palette; entry.palette_roles = m_job_palette_roles;
    entry.use_printable_colors = m_job_use_printable_colors;
    entry.color_intent_path = m_color_intent_path; entry.color_intent_schema = m_color_intent_schema;
    entry.color_intent_sha256 = m_color_intent_sha256; entry.title = _L("生成模型");
    request.artifact.local_path = entry.model_path; request.artifact.job_id = entry.job_id;
    request.expected_sha256 = m_generation_artifact_sha256;
    return request_open_workbench_artifact(request);
}

bool ModelGenerationPanel::load_library_entry(const boost::filesystem::path& model_path,
                                               const boost::filesystem::path& reference_image_path,
                                               const boost::filesystem::path& ai_image_path,
                                               const std::vector<std::string>& palette,
                                               const AIModelGenerationClient::PaletteRoles& palette_roles,
                                               bool use_printable_colors,
                                               const boost::filesystem::path& color_intent_path,
                                               const std::string& color_intent_schema,
                                               const std::string& color_intent_sha256,
                                               const std::string& job_id, const wxString& title, bool new_asset,
                                               std::shared_ptr<const ModelHistoryRepairIndex> repairs,
                                               std::shared_ptr<ModelHistoryOpenRequest> restoration,
                                               const std::string& expected_sha256)
{
    if (m_model_preview == nullptr || m_shutdown || m_preview_loading || (m_history_open && !restoration)) return false;
    if (model_path.empty()) {
        if (can_replace_model_asset()) load_design_library_entry(job_id);
        return false;
    }
    // Clicking a history entry is an explicit context switch.  Detach the
    // current preview below, but do not delete its remote job or source input.
    // This keeps history loading to one action even when a restored image
    // preview is still waiting for its paid-generation decision.
    boost::system::error_code ec;
    if (!restoration && !boost::filesystem::is_regular_file(model_path, ec)) {
        m_workbench_load_error = true;
        m_status->SetLabel(_L("历史模型文件已不存在。"));
        m_result_summary->SetLabel(_L("请刷新历史列表或恢复模型文件后重试。"));
        refresh_controls();
        return false;
    }
    if (!restoration && !can_replace_model_asset()) return false;
    auto request = restoration ? restoration : std::make_shared<ModelHistoryOpenRequest>();
    request->selected_asset = job_id;
    request->root = generated_models_root();
    request->expected_source = model_path; request->expected_source_sha256 = expected_sha256;
    request->entry.model_path = model_path;
    request->entry.reference_image_path = reference_image_path;
    request->entry.ai_image_path = ai_image_path;
    request->entry.palette = palette;
    request->entry.palette_roles = palette_roles;
    request->entry.use_printable_colors = use_printable_colors;
    request->entry.color_intent_path = color_intent_path;
    request->entry.color_intent_schema = color_intent_schema;
    request->entry.color_intent_sha256 = color_intent_sha256;
    request->entry.job_id = job_id;
    request->entry.title = title;
    request->repairs = std::move(repairs);
    if (!request->repairs && !restoration) {
        const auto found = std::find_if(m_library_entries.begin(), m_library_entries.end(),
            [&](const auto& entry) { return entry.model_path == model_path; });
        if (found != m_library_entries.end()) request->repairs = found->repair_index;
    }
    const bool direct_history = !new_asset && bool(m_history_model_open);
    if (direct_history || restoration) {
        request->generation = ++m_history_open_generation;
        if (!restoration) {
            const auto previous = std::find_if(m_library_entries.begin(), m_library_entries.end(),
                [&](const auto& entry) { return entry.model_path == m_workbench_model.path; });
            if (previous != m_library_entries.end()) request->previous = *previous;
            else {
                request->previous.model_path = m_workbench_model.path;
                request->previous.job_id = m_workbench_model.asset_id;
                request->previous.palette = m_workbench_model.palette;
                request->previous.palette_roles = m_workbench_model.palette_roles;
                request->previous.use_printable_colors = m_workbench_model.use_printable_colors;
                request->previous.reference_image_path = m_workbench_model.reference_image_path;
                request->previous.ai_image_path = m_workbench_model.design_image_path;
                request->previous.color_intent_path = m_workbench_model.color_intent_path;
                request->previous.color_intent_schema = m_workbench_model.color_intent_schema;
                request->previous.color_intent_sha256 = m_workbench_model.color_intent_sha256;
            }
            if (m_workbench_check_path == m_workbench_model.path.string() &&
                m_workbench_check_revision == m_workbench_model.revision) request->previous_check = m_workbench_check_result;
            const bool previous_workbench = m_finishing_workbench;
            const bool previous_editing = m_workbench_editing;
            const auto previous_view = m_model_preview->view_state();
            const auto previous_stats = m_model_stats->GetLabel();
            const auto previous_status = m_status->GetLabel();
            const auto previous_summary = m_result_summary->GetLabel();
            const bool previous_load_error = m_workbench_load_error;
            request->restore_presentation = [this, previous_workbench, previous_editing, previous_view,
                                            previous_stats, previous_status, previous_summary, previous_load_error] {
                set_finishing_workbench(previous_workbench);
                m_workbench_editing = previous_editing;
                m_model_stats->SetLabel(previous_stats);
                m_status->SetLabel(previous_status);
                m_result_summary->SetLabel(previous_summary);
                m_workbench_load_error = previous_load_error;
                if (m_workbench_model.ready) m_model_preview->restore_view(previous_view);
                refresh_post_generation_workbench();
            };
            m_history_open = request;
            if (!m_history_model_open({request->generation, request->selected_asset, ModelHistoryOpenEvent::Phase::Begin})) {
                m_history_open.reset();
                return false;
            }
        } else m_history_open = request;
    } else if (m_history_navigation) m_history_navigation(false);
    if (direct_history && !restoration) {
        wxWeakRef<ModelGenerationPanel> weak(this);
        request->progress = [weak, generation = request->generation, asset = request->selected_asset](ModelHistoryOpenEvent::Stage stage) {
            wxGetApp().CallAfter([weak, generation, asset, stage] {
                if (!weak || weak->m_shutdown || !weak->m_history_open || weak->m_history_open->canceled ||
                    weak->m_history_open->generation != generation || weak->m_history_open->selected_asset != asset) return;
                weak->m_history_model_open({generation, asset, ModelHistoryOpenEvent::Phase::Progress, stage});
            });
        };
    }
    auto history_metadata = std::make_shared<ModelHistoryMetadata>();
    const wxString previous_model_stats = m_model_stats->GetLabel();
    m_workbench_load_error = false;
    m_status->SetLabel(_L("正在加载历史模型：") + title);
    m_model_stats->SetLabel(_L("正在解析模型..."));
    load_model_preview_async(model_path, palette,
        [=](size_t triangle_count, Vec3d dimensions, size_t color_count, double load_seconds) {
    const auto& entry = request->entry;
    const auto& model_path = entry.model_path;
    const auto& reference_image_path = entry.reference_image_path;
    const auto& ai_image_path = entry.ai_image_path;
    const auto& palette = entry.palette;
    const auto& palette_roles = entry.palette_roles;
    const auto use_printable_colors = entry.use_printable_colors;
    const auto& color_intent_path = entry.color_intent_path;
    const auto& color_intent_schema = entry.color_intent_schema;
    const auto& color_intent_sha256 = entry.color_intent_sha256;
    const auto& job_id = entry.job_id;
    const auto& title = entry.title;
    request->installed = true;
    // Successful explicit history navigation starts a fresh editing context,
    // including when the user selects the same source file again.
    reset_beauty_asset();
    if (!m_finishing_candidate.empty()) {
        boost::system::error_code ignored;
        if (m_finishing_candidate != model_path) boost::filesystem::remove(m_finishing_candidate, ignored);
        m_finishing_candidate.clear();
    }
    m_finishing_options.selected_faces.clear();
    m_finishing_before = false;
    m_portrait_entered = !new_asset; // Only a newly imported asset may auto-start on explicit Portrait entry.
    m_portrait_mode = false;
    if (m_beauty_controls) m_beauty_controls->set_portrait_enabled(false);
    const wxImage reference_image = reference_image_path.empty() ? wxImage() : wxImage(reference_image_path.wstring());
    const wxImage ai_image = ai_image_path.empty() ? wxImage() : wxImage(ai_image_path.wstring());
    m_history_display_image = load_model_image_display_copy(ai_image_path);
    m_history_display_source = ai_image_path;
    nlohmann::json metadata;
    std::map<std::string, std::string> encoded_fields;
    if (!history_metadata->take_if_current(library_metadata_path(job_id), metadata, encoded_fields)) {
        request->check.reset();
        metadata = read_json(library_metadata_path(job_id));
    }
    if (ModelLibraryObservations::has_check_certificate(metadata)) {
        if (!ModelLibraryObservations::write_observations(library_metadata_path(job_id), metadata,
                {{"load_seconds", load_seconds}}))
            BOOST_LOG_TRIVIAL(warning) << "Unable to update certified model load metrics for " << job_id;
    } else if (metadata.is_object()) {
        metadata["schema_version"] = std::max(4, metadata.value("schema_version", 0));
        metadata["triangle_count"] = triangle_count;
        metadata["color_count"] = color_count;
        metadata["load_seconds"] = load_seconds;
        metadata["dimensions"] = {dimensions.x(), dimensions.y(), dimensions.z()};
        if (!write_json_with_preencoded_fields(library_metadata_path(job_id), metadata, encoded_fields))
            BOOST_LOG_TRIVIAL(warning) << "Unable to update model load metrics for " << job_id;
    }

    ++m_workbench_model.revision;
    m_workbench_model.format = AI::model_artifact_format(model_path);
    m_workbench_model.color_encoding = m_workbench_model.format == "glb" ? "textures_or_vertex_colors" : "vertex_colors";
    m_workbench_model.sha256 = m_model_preview->model_sha256();
    m_workbench_model.reference_image_path = reference_image_path;
    m_workbench_model.design_image_path = ai_image_path;
    m_workbench_model.use_printable_colors = use_printable_colors;
    m_workbench_model.color_intent_path = color_intent_path;
    m_workbench_model.color_intent_schema = color_intent_schema;
    m_workbench_model.color_intent_sha256 = color_intent_sha256;
    m_busy = false;
    m_workbench_model.path = model_path;
    m_workbench_model.asset_id = job_id;
    m_workbench_model.palette = palette;
    m_workbench_model.palette_roles = palette_roles.empty() ? automatic_palette_roles(palette) : palette_roles;
    m_workbench_model.ready = true;
    m_workbench_load_error = false;
    if (m_finishing_status != nullptr)
        m_finishing_status->SetLabel(m_workbench_model.format == "glb"
            ? _L("点选拼图改色，调整范围后保存新版本。原件和未改区域的纹理保留。")
            : _L("当前历史模型已加载，处理结果将另存为新版本。"));
    show_model_comparison();
    m_library_model_loaded = true;
    restore_portrait_draft();
    m_model_stats->SetLabel(wxString::Format(
        _L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm\n%s"),
        static_cast<unsigned long long>(triangle_count), static_cast<unsigned long long>(color_count),
        dimensions.x(), dimensions.y(), dimensions.z(),
        model_load_summary(triangle_count, load_seconds).c_str()));
    m_model_preview_message->SetLabel(_L("历史模型已自动摆正；可同时对照原图和 AI 图后再导入。"));
    m_model_preview->refresh();
    // Restore evidence before controls, presentation or automatic checking can
    // observe the new asset identity. Uncertified originals use the normal check.
    m_workbench_check_result = request->check.value_or(WorkbenchCheckResult{});
    m_workbench_check_path = request->check ? model_path.string() : std::string{};
    m_workbench_check_revision = request->check ? m_workbench_model.revision : 0;
    m_workbench_check_card_visible = false;
    refresh_controls();
    if (request->restoring) {
        m_model_preview->retain_history_rollback(false);
        if (m_history_open == request) m_history_open.reset();
        if (request->restored) request->restored();
    } else if (direct_history && m_history_open == request) {
        m_history_model_open({request->generation, request->selected_asset, ModelHistoryOpenEvent::Phase::Loaded,
            ModelHistoryOpenEvent::Stage::Displaying, job_id});
    }
    }, [this, previous_model_stats, request, direct_history](std::string error) {
        m_workbench_load_error = true;
        m_model_stats->SetLabel(previous_model_stats);
        m_status->SetLabel(_L("历史模型加载失败，保留当前模型与预览。"));
        m_result_summary->SetLabel(from_u8(error));
        refresh_controls();
        if (request->restoring) {
            // Never expose a failed target as if it were the previous asset.
            m_model_preview->retain_history_rollback(false);
            m_model_preview->clear();
            m_workbench_model.ready = false;
            m_workbench_model.path.clear();
            m_workbench_model.asset_id.clear();
            m_workbench_check_result = {};
            if (m_history_open == request) m_history_open.reset();
            if (request->restored) request->restored();
        } else if (direct_history && m_history_open == request) {
            m_history_model_open({request->generation, request->selected_asset, ModelHistoryOpenEvent::Phase::Failed});
        }
    }, library_metadata_path(job_id), history_metadata,
       [this, request, direct_history] {
           return !request->canceled && (!direct_history || m_history_open == request) &&
               (request->expected_source_sha256.empty() ||
                AI::model_artifact_sha256(request->expected_source) == request->expected_source_sha256) &&
               (request->restoring || (AI::model_artifact_sha256(request->entry.model_path) == request->model_hash &&
                AI::model_artifact_sha256(library_metadata_path(request->entry.job_id)) == request->record_hash));
       }, request);
    return true;
}

void ModelGenerationPanel::finish_history_model_open(bool accepted, std::function<void()> complete)
{
    if (!accepted && m_history_open) {
        wxWeakRef<ModelGenerationPanel> weak(this);
        if (m_history_open->cancel_after_drain([weak, complete] {
                if (weak && !weak->m_shutdown) weak->finish_history_model_open(false, complete);
            })) return;
    }
    auto request = std::move(m_history_open);
    m_history_open.reset();
    if (!request) { if (complete) complete(); return; }
    request->canceled = !accepted;
    if (accepted || !request->installed) {
        m_model_preview->retain_history_rollback(false);
        if (!accepted && request->restore_presentation) request->restore_presentation();
        refresh_controls();
        if (complete) complete();
        return;
    }
    const auto& previous = request->previous;
    if (previous.model_path.empty()) {
        m_model_preview->retain_history_rollback(false);
        m_model_preview->clear();
        m_workbench_model.path.clear();
        m_workbench_model.asset_id.clear();
        m_workbench_model.ready = false;
        ++m_workbench_model.revision;
        if (request->restore_presentation) request->restore_presentation();
        refresh_controls();
        if (complete) complete();
        return;
    }
    auto restoration = std::make_shared<ModelHistoryOpenRequest>();
    restoration->restoring = true;
    restoration->previous_check = request->previous_check;
    restoration->restored = [request, complete = std::move(complete)] {
        if (request->restore_presentation) request->restore_presentation();
        if (complete) complete();
    };
    if (!load_library_entry(previous.model_path, previous.reference_image_path, previous.ai_image_path,
            previous.palette, previous.palette_roles, previous.use_printable_colors, previous.color_intent_path,
            previous.color_intent_schema, previous.color_intent_sha256, previous.job_id, previous.title,
            false, {}, restoration)) {
        if (restoration->restored) restoration->restored();
    }
}


} // namespace Slic3r::GUI
