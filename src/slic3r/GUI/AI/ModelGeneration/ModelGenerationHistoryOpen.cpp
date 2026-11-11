#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelLibraryObservations.hpp"
#include "ModelPreview3D.hpp"
#include "ModelImageDisplayCopy.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
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
                                               std::shared_ptr<ModelHistoryOpenRequest> restoration)
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
                [&](const auto& entry) { return entry.model_path == m_displayed_model_path; });
            if (previous != m_library_entries.end()) request->previous = *previous;
            else {
                request->previous.model_path = m_displayed_model_path;
                request->previous.job_id = m_displayed_model_job_id;
                request->previous.palette = m_displayed_model_palette;
                request->previous.palette_roles = m_displayed_model_palette_roles;
            }
            if (m_workbench_check_path == m_displayed_model_path.string() &&
                m_workbench_check_revision == m_sequence) request->previous_check = m_workbench_check_result;
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
                if (m_model_preview_ready) m_model_preview->restore_view(previous_view);
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
    m_ui_model_generation_context = true;
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

    m_poll_timer.Stop();
    m_client.cancel_current();
    ++m_sequence;
    ++m_model_generation_session;
    // The old download callbacks are invalidated above. Clear their busy and
    // output state only after the historical model has loaded successfully.
    m_preview_download_in_flight = false;
    m_preview_download_cancelled = false;
    m_preview_output_available = false;
    m_preview_output = "preview";
    m_preview_path.clear();
    m_job_id.clear();
    m_job_palette = palette;
    m_job_palette_roles = palette_roles.empty() ? automatic_palette_roles(palette) : palette_roles;
    m_job_use_printable_colors = use_printable_colors;
    m_custom_palette = palette;
    m_palette_roles = m_job_palette_roles;
    m_palette_roles_source = palette;
    m_palette_source->SetSelection(use_printable_colors && !palette.empty() ? 2 : 1);
    m_palette_recommendation_confirmed = !palette.empty();
    m_awaiting_palette_confirmation = false;
    // Replace the visible input atomically after the new model has loaded.
    // Legacy records may have no input context; never borrow it from the last model.
    const auto input_string = [&metadata](const char* key) {
        const auto value = metadata.find(key);
        return value != metadata.end() && value->is_string() ? value->get<std::string>() : std::string();
    };
    m_job_generation_options = AIModelGenerationClient::restore_generation_options(metadata);
    m_selected_design_provider = m_job_generation_options.design_provider;
    m_selected_design_model = m_job_generation_options.design_model;
    m_selected_geometry_model = m_job_generation_options.geometry_model;
    const std::string& texture_quality = m_job_generation_options.texture_quality;
    const auto saved_face_limit = metadata.find("face_limit");
    const int historical_face_limit = saved_face_limit != metadata.end() && saved_face_limit->is_number_integer()
        ? saved_face_limit->get<int>() : 1000000;
    m_job_face_limit = m_job_generation_options.face_limit;
    m_legacy_generation_defaults = historical_face_limit != m_job_face_limit;
    m_job_generation_profile = m_job_face_limit <= 300000 ? "performance" : "quality";
    m_provider->SetSelection(m_job_generation_options.provider == "yoyoclaw" ? 1 : 0);
    refresh_provider_options();
    m_quality->SetSelection(m_job_face_limit <= 300000 ? 0 : m_job_face_limit == 2000000 && m_quality->GetCount() == 3 ? 2 : 1);
    m_geometry_quality->SetSelection(m_geometry_quality->GetCount() > 1 && m_job_generation_options.geometry_quality == "detailed" ? 1 : 0);
    m_texture_quality->SetSelection(m_texture_quality->GetCount() == 1 ? 0 : texture_quality == "extreme" ? 2 : texture_quality == "detailed" ? 1 : 0);
    m_output_format->SetSelection(m_job_generation_options.output_format == "obj" ? 1 : 0);
    m_job_provider_name = m_job_generation_options.provider;
    m_job_provider_task_id = input_string("provider_task_id");
    m_job_provider_conversion_task_id = input_string("provider_conversion_task_id");
    std::string saved_prompt = input_string("prompt");
    if (saved_prompt == INTERNAL_DEFAULT_IMAGE_INSTRUCTION || input_string("source") == "local_finishing" || input_string("source")=="local_import")
        saved_prompt.clear();
    m_job_prompt = from_u8(saved_prompt);
    m_job_style = input_string("style");
    m_job_custom_style = input_string("custom_style");
    m_prompt->ChangeValue(m_job_prompt);
    m_prepared_prompt->ChangeValue(wxString());
    m_custom_style->ChangeValue(from_u8(m_job_custom_style));
    m_style->SetSelection(m_job_style.empty() ? wxNOT_FOUND : style_selection(m_job_style));
    m_stylized_style->SetSelection(m_job_style.empty() ? wxNOT_FOUND : stylized_style_selection(m_job_style));
    m_style->SetToolTip(m_job_style.empty() ? _L("该历史记录未保存风格；再次生成前请选择风格。") : wxString());
    m_style_user_selected = !m_job_style.empty();
    ++m_style_recommendation_sequence;
    m_style_recommendation_loading = false;
    m_style_recommendation_available = false;
    m_style_recommendation = {};
    m_restoring_input = false;
    m_job_image_path = reference_image.IsOk() ? reference_image_path : boost::filesystem::path();
    m_selected_image_path = m_job_image_path;
    m_selected_image->SetLabel(m_selected_image_path.empty()
        ? _L("该历史记录没有可用参考图") : wxString(m_selected_image_path.filename().wstring()));
    m_job_preview_expected = reference_image.IsOk() || ai_image.IsOk();
    m_reference_image_path = reference_image.IsOk() ? reference_image_path : boost::filesystem::path();
    m_raw_preview_path = ai_image.IsOk() ? ai_image_path : boost::filesystem::path();
    m_reference_image = reference_image.IsOk() ? reference_image.Copy() : wxImage();
    m_raw_preview_image = ai_image.IsOk() ? ai_image.Copy() : wxImage();
    m_model_reference_image = wxImage();
    m_strict_preview_image = wxImage();
    m_model_views_image = wxImage();
    m_clean_preview_image = ai_image.IsOk() ? ai_image.Copy() : wxImage();
    m_heatmap_image = wxImage();
    m_raw_preview_available = ai_image.IsOk();
    m_model_reference_available = false;
    m_strict_preview_available = false;
    m_model_views_available = false;
    m_heatmap_available = false;
    m_style_preview_ready = ai_image.IsOk();
    m_local_image_history = false;
    m_history_model_design_reusable = ai_image.IsOk() && input_string("source") == "image";
    m_style_preview_placeholder = ai_image.IsOk()
        ? wxString()
        : _L("该历史记录未保存 AI 生成图");
    if (m_preview_stage != nullptr)
        m_preview_stage->SetSelection(0);
    m_artifact_path = model_path;
    m_artifact_format = AI::model_artifact_format(model_path);
    m_artifact_color_encoding = m_artifact_format == "glb" ? "textures_or_vertex_colors" : "vertex_colors";
    m_color_intent_path = color_intent_path;
    m_color_intent_schema = color_intent_schema;
    m_color_intent_sha256 = color_intent_sha256;
    m_busy = false;
    m_awaiting_confirmation = false;
    m_ready = true;
    m_artifact_download_started = true;
    if (m_use_printable_colors != nullptr)
        m_use_printable_colors->SetValue(use_printable_colors);
    m_displayed_model_path = model_path;
    m_displayed_model_job_id = job_id;
    m_displayed_model_palette = palette;
    m_displayed_model_palette_roles = m_job_palette_roles;
    m_model_preview_ready = true;
    m_workbench_load_error = false;
    if (m_finishing_status != nullptr)
        m_finishing_status->SetLabel(m_artifact_format == "glb"
            ? _L("点选拼图改色，调整范围后保存新版本。原件和未改区域的纹理保留。")
            : _L("当前历史模型已加载，处理结果将另存为新版本。"));
    show_model_comparison();
    m_library_model_loaded = true;
    // Historical records do not persist every print-control value. Compare
    // later edits against the restored controls, as active-job restore does.
    m_job_print_settings = current_print_settings();
    m_job_palette_color_count = current_palette_color_count();
    restore_portrait_draft();
    clear_model_quality();
    update_progress(100, 4, _L("检查并导入"));
    m_model_stats->SetLabel(wxString::Format(
        _L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm\n%s"),
        static_cast<unsigned long long>(triangle_count), static_cast<unsigned long long>(color_count),
        dimensions.x(), dimensions.y(), dimensions.z(),
        model_load_summary(triangle_count, load_seconds).c_str()));
    m_model_preview_message->SetLabel(_L("历史模型已自动摆正；可同时对照原图和 AI 图后再导入。"));
    m_status->SetLabel(_L("已加载历史模型：") + title);
    m_result_summary->SetLabel(_L("历史模型已加载到结果对照，可继续检查图片并导入准备页。"));
    m_preview_message->SetLabel(reference_image.IsOk() && ai_image.IsOk()
        ? _L("历史素材已恢复：原图与 AI 生成图可同屏对照。")
        : reference_image.IsOk()
            ? _L("已恢复原图；该历史记录未保存 AI 生成图。")
            : ai_image.IsOk()
                ? _L("已恢复 AI 生成图；该任务没有可用原图。")
                : _L("旧历史记录未保存关联图片，3D 模型仍可正常预览。"));
    if (m_preview_book != nullptr)
        m_preview_book->SetSelection(0);
    apply_preview_stage(true);
    m_model_preview->refresh();
    // Restore evidence before controls, presentation or automatic checking can
    // observe the new asset identity. Uncertified originals use the normal check.
    m_workbench_check_result = request->check.value_or(WorkbenchCheckResult{});
    m_workbench_check_path = request->check ? model_path.string() : std::string{};
    m_workbench_check_revision = request->check ? m_sequence : 0;
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
    const uint64_t sequence = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.get_status(job_id,
        [weak, sequence, job_id](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                if (!status.palette_roles.empty() && status.palette == weak->m_displayed_model_palette) {
                    weak->m_job_palette_roles = status.palette_roles;
                    weak->m_displayed_model_palette_roles = status.palette_roles;
                }
                weak->update_library_provider_tasks(job_id, status);
                weak->apply_model_quality(status.model_quality);
                weak->apply_visual_quality(status.visual_quality);
                weak->apply_model_refinement(status.refinement);
                weak->refresh_controls();
            });
        },
        [weak, sequence, job_id](std::string) {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                weak->refresh_model_quality_card();
                weak->refresh_controls();
            });
        });
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
            m_model_preview_ready = false;
            m_ready = false;
            m_displayed_model_path.clear();
            m_displayed_model_job_id.clear();
            m_workbench_check_result = {};
            if (m_history_open == request) m_history_open.reset();
            if (request->restored) request->restored();
        } else if (direct_history && m_history_open == request) {
            m_history_model_open({request->generation, request->selected_asset, ModelHistoryOpenEvent::Phase::Failed});
        }
    }, library_metadata_path(job_id), history_metadata,
       [this, request, direct_history] {
           return !request->canceled && (!direct_history || m_history_open == request) &&
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
        m_displayed_model_path.clear();
        m_displayed_model_job_id.clear();
        m_model_preview_ready = false;
        m_ready = false;
        m_ui_model_generation_context = false;
        ++m_sequence;
        ++m_model_generation_session;
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
