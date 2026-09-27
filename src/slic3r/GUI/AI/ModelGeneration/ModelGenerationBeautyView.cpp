#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <wx/button.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/tglbtn.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;

void ModelGenerationPanel::on_discard(wxCommandEvent&)
{
    if (m_busy || m_finishing_running || !m_finishing_candidate.empty()) return;
    if (m_ready || m_model_preview_ready || m_style_preview_ready) {
        wxMessageDialog choice(this,
            _L("要先看看重新设计的建议吗？当前历史模型会保留，图片和描述可继续使用。"),
            _L("重新开始"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
        choice.SetYesNoCancelLabels(_L("直接重新开始"), _L("先看建议"), _L("留在当前作品"));
        const int answer = choice.ShowModal();
        if (answer == wxID_CANCEL) return;
        if (answer == wxID_NO) {
            wxString advice;
            if (m_model_refinement.available && !m_model_refinement.summary.empty())
                advice = _L("已有检查建议：\n") + wxString::FromUTF8(m_model_refinement.summary.c_str());
            else
                advice = _L("尚无这件作品的检查结论。可先按下面的方向对照原图：\n\n"
                    "• 人物不像：使用清晰正面参考，描述中明确五官、发型和姿态。\n"
                    "• 形体不完整：确认图片主体完整、遮挡较少，明确底座和连接部位。\n"
                    "• 颜色杂乱：先在3D美颜里试六色、保留嘴唇和服装等关键色。\n"
                    "• 表面小凹凸：先试局部美颜，通常不需要重新生成。\n\n"
                    "这些是设计建议，未运行新的AI分析。");
            wxMessageDialog guidance(this, advice, _L("重新设计建议"), wxOK | wxCANCEL | wxICON_INFORMATION);
            guidance.SetOKCancelLabels(_L("继续重新开始"), _L("返回调整作品"));
            if (guidance.ShowModal() != wxID_OK) return;
        }
    }
    const bool reuse_palette = m_palette_source->GetSelection() == 2 && !current_palette().empty();
    boost::system::error_code reference_error;
    if (!m_reference_image_path.empty() && boost::filesystem::is_regular_file(m_reference_image_path, reference_error)) {
        m_selected_image_path = m_reference_image_path;
        m_selected_image->SetLabel(wxString(m_selected_image_path.filename().wstring()));
    } else if (m_library_model_loaded) {
        m_selected_image_path.clear();
        m_selected_image->SetLabel(_L("未选择图片"));
    }
    if (m_finishing_workbench) set_finishing_workbench(false);
    reset(!m_ready);
    m_palette_recommendation_confirmed = reuse_palette;
    refresh_controls();
    m_prompt->SetFocus();
}

wxWindow* ModelGenerationPanel::build_model_finishing(wxWindow* parent)
{
    auto* scroll = new wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition,
        wxSize(FromDIP(320), FromDIP(480)), wxVSCROLL | wxBORDER_SIMPLE);
    scroll->SetMinSize(wxSize(FromDIP(320), FromDIP(400)));
    scroll->SetScrollRate(0, FromDIP(12));
    m_finishing_panel = scroll;
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(scroll, wxID_ANY, _L("3D 美颜工作台"));
    title->SetFont(wxGetApp().bold_font());
    sizer->Add(title, 0, wxALL, FromDIP(10));

    m_beauty_controls = new BeautyWorkbenchControls(scroll, m_model_preview, m_palette_provider, [this] {
        wxWeakRef<ModelGenerationPanel> weak(this);
        wxGetApp().CallAfter([weak] {
            if (weak && !weak->m_shutdown) weak->refresh_model_finishing();
        });
    });
    sizer->Add(m_beauty_controls, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    auto button = [this, sizer, scroll](wxButton*& target, const wxString& label) {
        target = new wxButton(scroll, wxID_ANY, label);
        sizer->Add(target, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    };
    button(m_finishing_preview, _L("预览新版本"));
    button(m_finishing_compare, _L("查看修改前"));
    button(m_finishing_accept, _L("保存到历史资产"));
    button(m_finishing_discard, _L("放弃本次预览"));
    button(m_finishing_cancel, _L("取消保存预览"));
    button(m_finishing_color_match, _L("下一步：匹配打印颜色"));
    m_finishing_color_match->SetToolTip(_L("保存美颜修改后，在当前模型上匹配打印耗材。"));
    m_finishing_status = new wxStaticText(scroll, wxID_ANY, wxEmptyString);
    m_finishing_status->Wrap(FromDIP(280));
    sizer->Add(m_finishing_status, 0, wxEXPAND | wxALL, FromDIP(10));
    scroll->SetSizer(sizer);

    m_finishing_preview->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { preview_model_finishing(); });
    m_finishing_accept->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { accept_model_finishing(); });
    m_finishing_discard->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { discard_model_finishing(); });
    m_finishing_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_finishing_canceled) m_finishing_canceled->store(true);
        m_finishing_status->SetLabel(_L("正在取消，本次预览不会替换当前模型。"));
        m_finishing_cancel->Disable();
    });
    m_finishing_color_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_color_matching || m_busy || !m_beauty_controls || m_beauty_controls->has_changes()) return;
        if (!is_nonempty_model(m_displayed_model_path)) return;
        AI::GeneratedModelArtifact artifact;
        artifact.local_path = m_displayed_model_path;
        artifact.job_id = m_displayed_model_job_id;
        artifact.format = AI::model_artifact_format(m_displayed_model_path);
        artifact.color_encoding = m_artifact_color_encoding;
        artifact.generation_palette = m_displayed_model_palette;
        artifact.used_printable_colors = m_job_use_printable_colors;
        m_color_matching(artifact);
    });
    auto compare = [this] {
        if (m_busy || m_finishing_candidate.empty()) return;
        const auto& path = m_finishing_before ? m_finishing_candidate : m_finishing_source;
        if (!show_finishing_version(path)) return;
        m_finishing_before = !m_finishing_before;
        m_finishing_compare->SetLabel(m_finishing_before ? _L("查看修改后") : _L("查看修改前"));
        m_finishing_compare_model->SetLabel(m_finishing_compare->GetLabel());
        m_model_preview_message->SetLabel(m_finishing_before ? _L("修改前 · 当前版本") : _L("修改后 · 尚未保存"));
        refresh_model_finishing();
    };
    m_finishing_compare->Bind(wxEVT_BUTTON, [compare](wxCommandEvent&) { compare(); });
    m_finishing_compare_model->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
        if (m_busy || m_finishing_candidate.empty() || m_finishing_before) return;
        if (!show_finishing_version(m_finishing_source)) return;
        m_finishing_compare_held = true;
        m_finishing_compare_model->CaptureMouse();
        m_model_preview_message->SetLabel(_L("修改前 · 松开恢复修改后"));
    });
    auto release_compare = [this] {
        if (!m_finishing_compare_held) return;
        m_finishing_compare_held = false;
        if (m_finishing_compare_model->HasCapture()) m_finishing_compare_model->ReleaseMouse();
        show_finishing_version(m_finishing_candidate);
        m_model_preview_message->SetLabel(_L("修改后 · 尚未保存"));
    };
    m_finishing_compare_model->Bind(wxEVT_LEFT_UP, [release_compare](wxMouseEvent&) { release_compare(); });
    m_finishing_compare_model->Bind(wxEVT_MOUSE_CAPTURE_LOST,
        [release_compare](wxMouseCaptureLostEvent&) { release_compare(); });
    scroll->Hide();
    return scroll;
}

void ModelGenerationPanel::set_finishing_workbench(bool enabled)
{
    m_finishing_workbench = enabled;
    if (enabled && m_preview_book) { m_preview_book->SetSelection(0); show_model_comparison(); }
    m_workflow_panel->Show(!enabled);
    m_preview_area->Show(!enabled);
    m_expand_images->SetValue(false);
    m_expand_images->Show(!enabled);
    m_zoom_out->Show(!enabled); m_zoom_in->Show(!enabled);
    m_zoom_fit->Show(!enabled); m_preview_zoom->Show(!enabled);
    m_finishing_shortcut->SetLabel(enabled ? _L("返回作品／导入") : _L("3D 美颜工作台"));
    m_preview_stage_hint->Show(!enabled);
    m_preview_message->Show(!enabled);
    m_result_summary->Show(!enabled);
    m_preview_kind->SetLabel(enabled ? _L("美颜工作台") : _L("结果对照"));
    m_model_decision_panel->Hide();
    m_model_preview->set_color_controls_visible(!enabled);
    refresh_model_finishing();
    Layout();
    m_comparison_panel->Layout();
    m_model_page->Layout(); m_model_page->FitInside(); m_model_page->Scroll(0, 0);
}

void ModelGenerationPanel::refresh_model_finishing()
{
    if (!m_finishing_panel || !m_beauty_controls) return;
    if (!m_finishing_running && !m_finishing_candidate.empty() && m_displayed_model_path != m_finishing_source) {
        boost::system::error_code ignored;
        if (m_displayed_model_path != m_finishing_candidate)
            boost::filesystem::remove(m_finishing_candidate, ignored);
        m_finishing_candidate.clear();
        m_finishing_source.clear();
    }
    const bool pending = !m_finishing_candidate.empty();
    const bool ready = m_model_preview_ready && is_nonempty_model(m_displayed_model_path);
    const bool visible = m_finishing_workbench && (ready || m_finishing_running || pending);
    const bool editable = visible && ready && !m_busy && !pending;
    m_finishing_panel->Show(visible);
    m_beauty_controls->synchronize(m_displayed_model_path, editable, visible && !pending && !m_finishing_running);
    m_model_preview->set_selection_enabled(editable);
    m_finishing_preview->Show(!pending && !m_finishing_running);
    m_finishing_preview->Enable(editable && m_beauty_controls->has_changes());
    for (wxButton* action : {m_finishing_compare, m_finishing_accept, m_finishing_discard}) {
        action->Show(pending);
        action->Enable(pending && !m_busy);
    }
    m_finishing_compare_model->Show(pending);
    m_finishing_cancel->Show(m_finishing_running);
    m_finishing_cancel->Enable(m_finishing_running && m_finishing_canceled && !m_finishing_canceled->load());
    m_finishing_color_match->Show(!pending && !m_finishing_running);
    m_finishing_color_match->Enable(editable && m_beauty_controls->ready() &&
        m_color_matching && !m_beauty_controls->has_changes());
    if (visible && (pending || m_finishing_running || m_beauty_controls->has_changes()))
        m_import->Disable();
    if (m_finishing_running) m_stop->Hide();
    if (m_displayed_model_job_id.rfind("finish-", 0) == 0) {
        m_recheck_model->Disable(); m_visual_review_model->Disable();
        m_recheck_model->SetToolTip(_L("本地处理版本请导入准备页，检查实际打印条件。"));
    }
    m_finishing_panel->Layout();
    static_cast<wxScrolledWindow*>(m_finishing_panel)->FitInside();
    if (auto* page = dynamic_cast<wxScrolledWindow*>(m_finishing_panel->GetParent())) {
        page->Layout(); page->FitInside();
    }
}

void ModelGenerationPanel::preview_model_finishing()
{
    if (m_busy || m_shutdown || !m_finishing_workbench || !m_beauty_controls ||
        !m_beauty_controls->ready() || !m_beauty_controls->has_changes() || !m_finishing_candidate.empty()) return;
    if (m_model_preview->selection_busy()) {
        m_finishing_status->SetLabel(_L("正在更新选区，请完成后再预览。"));
        return;
    }
    const auto source = m_displayed_model_path;
    if (!is_nonempty_model(source)) {
        m_finishing_status->SetLabel(_L("模型文件已不存在，请从历史资产重新加载。"));
        return;
    }
    AI::ModelFinishingOptions options {false, false, 0.0};
    try {
        m_beauty_controls->prepare_options(options, m_model_preview->selection_state());
    } catch (const std::exception& error) {
        m_finishing_status->SetLabel(from_u8(error.what()));
        return;
    }
    if (m_finishing_worker.joinable()) m_finishing_worker.join();
    m_finishing_options = std::move(options);
    m_finishing_source = source;
    m_finishing_id = "finish-" + new_request_id();
    const auto destination = source.parent_path() / temp_path(m_finishing_id, "glb").filename();
    m_finishing_canceled = std::make_shared<std::atomic<bool>>(false);
    const auto canceled = m_finishing_canceled;
    m_finishing_running = true;
    m_busy = true;
    m_finishing_status->SetLabel(_L("正在生成美颜预览，当前版本保持不变……"));
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    const uint64_t sequence = m_sequence;
    const auto view = m_model_preview->view_state();
    try {
        m_finishing_worker = std::thread([weak, source, destination, options = m_finishing_options,
                                           canceled, sequence, view] {
            const auto result = AI::finish_model_artifact(source, destination, options,
                [canceled] { return canceled->load(); });
            auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
            std::string preview_error;
            if (result.success && result.changed() && !canceled->load()) {
                try { ModelPreview3D::prepare_model(destination, *prepared, preview_error, {}); }
                catch (const std::exception& error) { preview_error = error.what(); }
            }
            wxGetApp().CallAfter([weak, source, destination, result, sequence, canceled,
                                  prepared, preview_error, view] {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence) {
                    if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
                    return;
                }
                auto* self = weak.get();
                if (self->m_finishing_worker.joinable()) self->m_finishing_worker.join();
                self->m_finishing_running = false;
                self->m_busy = false;
                self->m_finishing_result = result;
                if (result.canceled || canceled->load()) {
                    self->m_finishing_status->SetLabel(_L("已取消，当前版本保持不变。"));
                } else if (!result.success) {
                    self->m_finishing_status->SetLabel(_L("预览未完成，当前版本已保留：") + from_u8(result.error));
                } else if (!result.changed()) {
                    self->m_finishing_status->SetLabel(_L("本次修改没有产生新版本。"));
                } else {
                    size_t triangles = 0, colors = 0;
                    Vec3d dimensions = Vec3d::Zero();
                    std::string error = preview_error;
                    if (!error.empty() || !self->m_model_preview->load_prepared_model(
                            std::move(*prepared), {}, triangles, dimensions, colors, error)) {
                        self->m_finishing_status->SetLabel(_L("预览加载失败，当前版本已保留：") + from_u8(error));
                    } else {
                        self->m_model_preview->restore_view(view);
                        self->m_model_stats->SetLabel(wxString::Format(
                            _L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm"),
                            static_cast<unsigned long long>(triangles),
                            static_cast<unsigned long long>(colors),
                            dimensions.x(), dimensions.y(), dimensions.z()));
                        self->m_finishing_candidate = destination;
                        self->m_finishing_before = false;
                        self->m_finishing_compare->SetLabel(_L("查看修改前"));
                        self->m_model_preview_message->SetLabel(_L("修改后 · 尚未保存"));
                        self->m_finishing_status->SetLabel(_L("预览已就绪。对照修改前后，再保存到历史资产。"));
                    }
                }
                if (self->m_finishing_candidate.empty() && result.success) {
                    boost::system::error_code ignored;
                    boost::filesystem::remove(destination, ignored);
                }
                self->m_status->SetLabel(self->m_finishing_status->GetLabel());
                self->refresh_controls();
            });
        });
    } catch (const std::exception& error) {
        m_finishing_running = false;
        m_busy = false;
        m_finishing_status->SetLabel(_L("暂时无法启动预览：") + from_u8(error.what()));
        refresh_controls();
    }
}

bool ModelGenerationPanel::show_finishing_version(const boost::filesystem::path& path)
{
    const auto view = m_model_preview->view_state();
    size_t triangles = 0, colors = 0;
    Vec3d dimensions = Vec3d::Zero();
    std::string error;
    if (!m_model_preview->load_model(path, {}, triangles, dimensions, colors, error)) {
        m_finishing_status->SetLabel(_L("模型加载失败，当前版本仍保留：") + from_u8(error));
        return false;
    }
    m_model_preview->restore_view(view);
    m_model_preview->set_color_controls_visible(!m_finishing_workbench);
    m_model_preview_ready = true;
    m_model_stats->SetLabel(wxString::Format(_L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm"),
        static_cast<unsigned long long>(triangles), static_cast<unsigned long long>(colors),
        dimensions.x(), dimensions.y(), dimensions.z()));
    return true;
}

void ModelGenerationPanel::select_local_finishing_version(const boost::filesystem::path& path, const std::string& id)
{
    const bool preview_download_was_active = m_preview_download_in_flight;
    ++m_sequence;
    if (preview_download_was_active) m_client.cancel_current();
    m_preview_download_in_flight = false;
    if (preview_download_was_active) {
        m_preview_download_cancelled = true;
        m_preview_path.clear();
        m_style_preview_ready = false;
        m_preview_output_available = false;
    }
    m_poll_timer.Stop();
    m_job_id.clear(); m_job_palette.clear(); m_job_palette_roles.clear();
    m_job_use_printable_colors = false;
    m_artifact_path = m_displayed_model_path = path;
    m_displayed_model_job_id = id;
    m_displayed_model_palette.clear(); m_displayed_model_palette_roles.clear();
    m_color_intent_path.clear(); m_color_intent_schema.clear(); m_color_intent_sha256.clear();
    m_artifact_format = AI::model_artifact_format(path);
    m_artifact_color_encoding = "vertex_colors";
    m_ready = true; m_library_model_loaded = true; m_artifact_download_started = false;
    m_awaiting_confirmation = false; m_awaiting_palette_confirmation = false;
    m_last_imported_model_path.clear();
    clear_model_quality();
    m_visual_quality = {};
    m_model_refinement = {};
}

void ModelGenerationPanel::accept_model_finishing()
{
    if (m_busy || m_finishing_candidate.empty()) return;
    if (!show_finishing_version(m_finishing_candidate)) return;
    const auto root = generated_models_root();
    nlohmann::json metadata {
        {"schema_version", 4}, {"job_id", m_finishing_id},
        {"model_path", m_finishing_candidate.lexically_relative(root).generic_string()},
        {"source", "local_finishing"}, {"prompt", "3D 美颜"},
        {"source_model", m_finishing_source.lexically_relative(root).generic_string()},
        {"source_sha256", m_finishing_result.source_sha256},
        {"model_sha256", m_finishing_result.output_sha256},
        {"palette", nlohmann::json::array()}, {"palette_roles", nlohmann::json::object()},
        {"use_printable_colors", false}, {"generated_at", std::time(nullptr)},
        {"triangle_count", m_finishing_result.faces_after},
        {"dimensions", m_finishing_result.dimensions},
        {"finishing", {{"beauty_puzzle", true}, {"changed_texture_pixels", m_finishing_result.changed_texture_pixels}}}
    };
    metadata["face_color_intent"] = m_model_preview->face_color_metadata();
    metadata["color_trial"] = m_model_preview->color_trial_metadata();
    metadata["semantic_color_state"] = m_model_preview->semantic_color_metadata();
    metadata["beauty_workbench"] = BeautyWorkbenchControls::accepted_document(
        m_finishing_options, m_model_preview->geometry_id());
    if (!m_reference_image_path.empty() && path_is_inside(root, m_reference_image_path))
        metadata["reference_image_path"] = m_reference_image_path.lexically_relative(root).generic_string();
    if (!m_raw_preview_path.empty() && path_is_inside(root, m_raw_preview_path))
        metadata["ai_image_path"] = m_raw_preview_path.lexically_relative(root).generic_string();
    if (!write_json(library_metadata_path(m_finishing_id), metadata)) {
        m_finishing_status->SetLabel(_L("版本记录保存失败，尚未接受；请检查磁盘空间后重试。"));
        return;
    }
    m_beauty_controls->mark_saved();
    select_local_finishing_version(m_finishing_candidate, m_finishing_id);
    m_finishing_candidate.clear();
    m_finishing_source.clear();
    m_finishing_status->SetLabel(_L("新版本已保存到历史资产。可继续编辑，或匹配打印颜色。"));
    m_status->SetLabel(m_finishing_status->GetLabel());
    m_model_preview_message->SetLabel(_L("当前显示：已保存的美颜版本。"));
    load_library_entries();
    refresh_controls();
}

void ModelGenerationPanel::discard_model_finishing()
{
    if (m_busy || m_finishing_candidate.empty()) return;
    if (!show_finishing_version(m_finishing_source)) return;
    boost::system::error_code ignored;
    boost::filesystem::remove(m_finishing_candidate, ignored);
    m_finishing_candidate.clear();
    m_finishing_before = false;
    m_finishing_status->SetLabel(_L("已放弃预览，当前美颜修改仍可继续编辑。"));
    m_status->SetLabel(m_finishing_status->GetLabel());
    m_model_preview_message->SetLabel(_L("当前显示：修改前模型。"));
    refresh_controls();
}

void ModelGenerationPanel::stop_model_finishing()
{
    if (m_finishing_canceled) m_finishing_canceled->store(true);
    if (m_finishing_worker.joinable()) m_finishing_worker.join();
    m_finishing_running = false;
    if (!m_finishing_candidate.empty()) {
        boost::system::error_code ignored;
        boost::filesystem::remove(m_finishing_candidate, ignored);
        m_finishing_candidate.clear();
    }
}
} // namespace Slic3r::GUI
