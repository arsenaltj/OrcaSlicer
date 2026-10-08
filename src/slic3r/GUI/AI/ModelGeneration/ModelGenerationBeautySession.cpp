#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/notebook.h>
#include <wx/numformatter.h>
#include <wx/msgdlg.h>
#include <wx/menu.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/weakref.h>
#include <wx/wrapsizer.h>
#include <wx/wupdlock.h>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;
namespace {
constexpr int beauty_finishing_tools[] = {4, 1, 3, 5, 1};
// Native wrapping may keep an entire CJK sentence as one word. Measure the
// displayed text so a narrow tool panel never truncates its instructions.
void wrap_workbench_text(wxStaticText* label, int width, bool preserve_lines = false)
{
    wxString source = label->GetLabel(), line, result;
    source.Replace("\r", "");
    if (!preserve_lines) source.Replace("\n", "");
    for (wxUniChar character : source) {
        if (character == '\n') {
            result += line + "\n"; line.clear();
            continue;
        }
        wxString next = line; next += character;
        if (!line.empty() && label->GetTextExtent(next).x > width) {
            result += line + "\n"; line.clear();
        }
        line += character;
    }
    const wxString wrapped = result + line;
    label->SetLabel(wrapped);
    wxClientDC dc(label);
    dc.SetFont(label->GetFont());
    label->SetMinSize(wxSize(1, dc.GetMultiLineTextExtent(wrapped).y + label->FromDIP(2)));
}

void apply_workbench_theme(wxWindow* window, bool enabled,
                           const wxColour& enabled_surface = wxColour(32, 32, 35))
{
    if (!window) return;
    const wxColour surface = enabled ? enabled_surface : *wxWHITE;
    const wxColour text = enabled ? wxColour(238, 240, 244) : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    window->SetBackgroundColour(surface);
    window->SetForegroundColour(text);
    for (wxWindow* child : window->GetChildren())
        apply_workbench_theme(child, enabled, enabled_surface);
    if (auto* toggle = dynamic_cast<WorkbenchSwitch*>(window)) toggle->rescale_workbench();
}

void apply_workbench_parameter_theme(wxWindow* parameters)
{
    apply_workbench_theme(parameters, true, wxColour(40, 40, 43));
}

void repaint_workbench_surface(wxWindow* window)
{
    if (!window || !window->IsShownOnScreen() || window->IsFrozen()) return;
    window->Refresh();
    window->Update();
    for (wxWindow* child : window->GetChildren())
        repaint_workbench_surface(child);
}
}
void ModelGenerationPanel::set_finishing_workbench(bool enabled)
{
    if (m_finishing_workbench == enabled) return;
    // Reparent, theme and lay out the shared viewport before exposing the new mode.
    wxWindowUpdateLocker mode_updates(this);
    m_finishing_workbench = enabled;
    if (enabled) {
        m_model_page->GetSizer()->Detach(m_comparison_panel);
        m_comparison_panel->Reparent(m_workbench_view_host);
        m_workbench_view_host->GetSizer()->Insert(1, m_comparison_panel, 1, wxEXPAND);
    } else {
        m_workbench_view_host->GetSizer()->Detach(m_comparison_panel);
        m_comparison_panel->Reparent(m_model_page);
        m_model_page->GetSizer()->Insert(0, m_comparison_panel, 1, wxEXPAND | wxALL, FromDIP(12));
    }
    GetSizer()->Show(m_generation_content, !enabled, true);
    m_generation_header->Show(!enabled);
    m_workbench_shell->Show(enabled);
    SetBackgroundColour(enabled ? wxColour(49, 49, 54) : *wxWHITE);
    m_model_preview->set_selection_preview_suppressed(enabled &&
        ((!m_finishing_candidate.empty() && !m_finishing_before) || (!m_finishing_accepted_path.empty() &&
         m_displayed_model_path == m_finishing_accepted_path)));
    m_model_preview->set_beauty_view(enabled);
    if (!enabled) {
        m_workbench_model_info->Hide();
        m_workbench_history_toggle->Hide();
    }
    m_model_preview->set_color_controls_visible(!enabled);
    m_model_stats->Show(!enabled);
    m_front_model_view->Show(!enabled);
    m_reset_model_view->Show(!enabled);
    apply_workbench_theme(m_finishing_panel, enabled);
    // The parameter surface can be reparented outside the inspector while hidden.
    apply_workbench_parameter_theme(m_workbench_parameters);
    apply_workbench_theme(m_workbench_footer, enabled);
    apply_workbench_theme(m_workbench_history_panel, enabled);
    if (enabled && m_workbench_history_search) {
        m_workbench_history_search->SetBackgroundColour(wxColour(49, 49, 54));
        m_workbench_history_search->SetForegroundColour(wxColour(235, 235, 235));
    }
    if (m_workbench_history_panel) {
        m_workbench_history_panel->Show(enabled);
    }
    if (m_model_preview && m_model_preview->GetParent()) {
        auto* model_card = m_model_preview->GetParent();
        model_card->SetWindowStyleFlag(enabled ? wxBORDER_NONE : wxBORDER_SIMPLE);
        model_card->SetMinSize(enabled ? wxSize(FromDIP(640), FromDIP(480))
                                       : wxSize(FromDIP(440), FromDIP(560)));
        m_model_preview->SetMinSize(enabled ? wxSize(FromDIP(640), FromDIP(480))
                                            : wxSize(FromDIP(420), FromDIP(280)));
        model_card->SetBackgroundColour(enabled ? wxColour(49, 49, 54) : *wxWHITE);
        if (model_card->GetParent())
            model_card->GetParent()->SetBackgroundColour(enabled ? wxColour(49, 49, 54) : wxColour(241, 244, 245));
    }
    // Switching views is not a model transaction. Keep the current Beauty
    // timeline so returning to the workbench can still undo its edits.
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
    if (enabled) {
        m_local_recolor_toggle->SetValue(false);
        const int operation = m_beauty_controls ? m_beauty_controls->operation_index() : 0;
        if (m_finishing_tool && operation >= 0 && operation < int(std::size(beauty_finishing_tools)))
            m_finishing_tool->SetSelection(beauty_finishing_tools[operation]);
    }
    if (m_finishing_tool) m_finishing_tool->Show(!enabled);
    m_model_decision_panel->Hide();
    refresh_local_recolor_controls();
    if (!enabled) { m_finishing_gray->SetValue(false); m_model_preview->set_gray_view(false); }
    update_finishing_selection();
    refresh_model_finishing();
    Layout();
    m_comparison_panel->Layout();
    // Re-evaluate the three-column breakpoint immediately when switching
    // modes; the panel size itself may not change during the toggle.
    m_comparison_panel->SendSizeEvent();
    m_model_page->Layout(); m_model_page->FitInside(); m_model_page->Scroll(0, 0);
    // Thumbnail targets depend on which history surface is visible after the switch.
    if (enabled) refresh_workbench_history();
    if (enabled && m_library_entries.empty())
        load_library_entries();
}

PostGenerationUiState ModelGenerationPanel::post_generation_ui_state() const
{
    auto state = derive_post_generation_ui_state(
        m_finishing_workbench ? PostGenerationUiState::Mode::Workbench
                              : PostGenerationUiState::Mode::Result,
        m_model_preview_ready,
        !m_displayed_model_path.empty(),
        m_busy || m_preview_loading || m_workbench_import_running,
        m_finishing_running || m_workbench_check_running,
        (m_beauty_transactions && m_beauty_transactions->processing()) ||
            (m_model_preview && m_model_preview->semantic_processing()) ||
            (m_finishing_workbench && m_model_preview && m_model_preview->selection_busy()),
        !m_finishing_candidate.empty(),
        m_finishing_before || m_finishing_compare_held,
        m_finishing_workbench && m_beauty_controls && m_beauty_controls->has_changes(),
        !m_finishing_undo_path.empty() || (m_model_preview && m_model_preview->can_undo_selection()) ||
            (m_beauty_transactions && m_beauty_transactions->undo_count() > 0),
        !m_finishing_redo_path.empty() || (m_model_preview && m_model_preview->can_redo_selection()) ||
            (m_beauty_transactions && m_beauty_transactions->redo_count() > 0),
        m_workbench_load_error);
    const int tool = m_finishing_tool ? m_finishing_tool->GetSelection() : -1;
    if (AI::model_artifact_format(m_displayed_model_path) == "glb" && (tool == 3 || tool == 5))
        state.can_preview = false;
    return state;
}

void ModelGenerationPanel::update_finishing_selection()
{
    const bool local = m_finishing_workbench && m_workbench_editing && (m_finishing_tool->GetSelection() == 1 || m_finishing_tool->GetSelection() == 4 || m_finishing_tool->GetSelection() == 5);
    m_model_preview->set_selection_enabled(local && !m_busy && !m_workbench_check_running && !m_model_preview->semantic_processing() &&
        (m_finishing_candidate.empty() || m_finishing_workbench));
    if (!local) return;
    const auto selected = m_model_preview->selected_face_count();
    const auto protected_faces = m_model_preview->protected_face_count();
    m_finishing_selection_status->SetLabel(wxString::Format(_L("已选 %llu 个面 · 保护 %llu 个面"),
        static_cast<unsigned long long>(selected), static_cast<unsigned long long>(protected_faces)));
    update_region_mode();
}

void ModelGenerationPanel::refresh_model_finishing()
{
    if (!m_finishing_panel) return;
    std::unique_ptr<wxWindowUpdateLocker> workbench_updates;
    if (m_finishing_workbench && m_workbench_shell)
        workbench_updates = std::make_unique<wxWindowUpdateLocker>(m_workbench_shell);
    if (!m_finishing_undo_path.empty() && m_displayed_model_path != m_finishing_accepted_path) {
        m_finishing_undo_path.clear(); m_finishing_accepted_path.clear();
    }
    if (!m_finishing_running && !m_finishing_candidate.empty() && m_displayed_model_path != m_finishing_source) {
        // Selecting a different library model invalidates only the unaccepted
        // local preview, never the selected file or its history record.
        boost::system::error_code ignored;
        if (m_displayed_model_path != m_finishing_candidate)
            boost::filesystem::remove(m_finishing_candidate, ignored);
        clear_unaccepted_beauty_candidates();
        m_beauty_session_source.reset();
        m_beauty_reoptimization_before.reset();
        m_finishing_candidate_region_evidence.reset();
        m_finishing_candidate_secondary_evidence.reset();
        m_finishing_candidate_secondary_error.clear();
        if (m_beauty_transactions) m_beauty_transactions->reset();
        m_finishing_candidate.clear(); m_finishing_source.clear(); m_finishing_undo_path.clear();
        m_model_preview->set_selection_preview_suppressed(false);
    }
    const bool pending = !m_finishing_candidate.empty();
    const bool ready = m_model_preview_ready && is_nonempty_model(m_displayed_model_path);
    const bool transaction_busy = m_workbench_check_running || (m_beauty_transactions && m_beauty_transactions->processing()) ||
        m_model_preview->semantic_processing() ||
        (m_finishing_workbench && m_model_preview->selection_busy());
    const bool repaint_layout = m_finishing_workbench &&
        (m_finishing_panel->IsShown() != m_workbench_editing ||
         m_workbench_history_panel->IsShown() != (!m_workbench_editing && !m_workbench_history_collapsed) ||
         m_workbench_footer->IsShown() != (m_workbench_editing || m_finishing_running || pending ||
                                          (m_beauty_transactions && m_beauty_transactions->processing())));
    m_finishing_panel->Show(m_finishing_workbench && m_workbench_editing && (ready || m_finishing_running || pending));
    const auto beauty_source = pending ? m_finishing_candidate : m_displayed_model_path;
    if (m_beauty_controls)
        m_beauty_controls->synchronize(beauty_source, ready && !m_busy && !transaction_busy &&
            !m_finishing_before && !m_finishing_compare_held,
            m_finishing_workbench, m_busy || m_finishing_running || transaction_busy, pending, generated_models_root());
    const bool editable = ready && !m_busy && !transaction_busy && !m_model_preview->selection_busy();
    const int tool = m_finishing_tool->GetSelection();
    const bool cleanup = tool == 5;
    const bool local = tool == 1 || cleanup || tool == 4;
    const bool color = tool == 2 || tool == 4;
    if (!m_finishing_workbench || m_workbench_editing)
        m_model_preview->set_color_controls_visible(!m_finishing_workbench || tool == 2);
    const bool selection_visible = local || tool == 4;
    const bool geometry_smoothing_enabled = m_finishing_workbench && tool == 0;
    m_finishing_selection_section->Show(m_finishing_workbench && selection_visible);
    m_finishing_selection_section->SetLabel(m_finishing_selection_open ? _L("收起选择区域") : _L("选择区域"));
    m_finishing_selection_controls->Show(selection_visible && (!m_finishing_workbench || m_finishing_selection_open));
    m_finishing_overlay_row->Show(selection_visible && (!m_finishing_workbench || m_finishing_selection_open));
    m_finishing_overlay->Enable(editable && !pending);
    const bool beauty_editable = m_finishing_workbench && editable;
    m_finishing_selection_controls->Enable((editable && !pending) || beauty_editable);
    m_workbench_smoothing_iterations->Enable(beauty_editable && !pending && geometry_smoothing_enabled);
    m_workbench_preserve_hard_edges->Enable(beauty_editable && !pending && geometry_smoothing_enabled);
    m_finishing_tool->Enable(editable && !pending);
    m_finishing_preset->Show(!m_finishing_workbench && !color && tool != 3 && !cleanup);
    m_finishing_smooth->Show(!m_finishing_workbench && !color && tool != 3 && !cleanup);
    m_finishing_strength->Show(!color && tool != 3 && !(m_finishing_workbench && m_beauty_controls && m_beauty_controls->geometry_deform_selected()));
    m_finishing_strength_value->Show(!color && tool != 3 && !(m_finishing_workbench && m_beauty_controls && m_beauty_controls->geometry_deform_selected()));
    m_finishing_strength_value->SetLabel(wxString::Format(_L("处理强度：%d%%"), m_finishing_strength->GetValue()));
    m_finishing_cleanup_hint->Show(cleanup);
    m_finishing_gray->Show(!m_finishing_workbench && !color && !cleanup);
    m_finishing_strength->SetToolTip(cleanup
        ? _L("力度越大，可合并的杂色块越大。仅处理选区内部；不会自动识别五官、纽扣或花纹。")
        : _L("强度越高，柔化越明显。保护轮廓与细小结构；可随时调整并重新预览。"));
    m_finishing_repair->Show(!m_finishing_workbench && tool == 3);
    m_finishing_compare_model->Show(pending);
    m_finishing_compare_model->Enable(editable);
    m_finishing_compare_model->SetLabel(m_finishing_workbench
        ? (m_finishing_before ? _L("查看处理后") : _L("查看处理前"))
        : (m_finishing_before ? _L("当前为处理前") : _L("按住查看处理前")));
    m_finishing_compare_model->GetParent()->Layout();
    m_finishing_preview->Enable(editable);
    m_finishing_preview->SetLabel(pending ? _L("按当前强度重新预览") : cleanup ? _L("预览去杂效果") : _L("预览处理效果"));
    m_finishing_preset->Enable(editable);
    m_finishing_smooth->Enable(editable);
    m_finishing_repair->Enable(editable);
    m_finishing_strength->Enable(editable && (cleanup || m_finishing_smooth->GetValue()));
    for (wxButton* button : {m_finishing_compare, m_finishing_accept, m_finishing_discard}) {
        button->Show(pending && !m_finishing_workbench); button->Enable(editable);
    }
    m_finishing_preview->Show(!m_finishing_workbench && !m_finishing_running && (!color || (tool == 4 && pending)));
    m_finishing_cancel->Show(!m_finishing_workbench && (m_finishing_running || transaction_busy));
    m_finishing_undo->Show(!m_finishing_workbench && !m_finishing_undo_path.empty() && !pending && !m_finishing_running);
    m_finishing_undo->Enable(editable);
    if (!m_finishing_redo_path.empty() && m_displayed_model_path != m_finishing_redo_source)
        m_finishing_redo_path.clear();
    m_finishing_redo->Show(!m_finishing_workbench && !m_finishing_redo_path.empty() && !pending && !m_finishing_running);
    m_finishing_redo->Enable(editable);
    if (pending || m_finishing_running || transaction_busy) {
        m_status->SetLabel(m_workbench_check_running ? _L("正在检查与安全修复，可旋转、缩放或取消。")
            : m_finishing_running ? _L("正在本地处理，可切换页面或取消。")
            : transaction_busy ? _L("正在处理人像区域，可旋转、缩放、查看日志或取消。")
            : _L("美颜预览就绪，接受新版本后可导入。"));
        m_import->Disable(); m_recheck_model->Disable(); m_visual_review_model->Disable();
        if (!beauty_editable) m_local_recolor_panel->Hide();
        m_discard->Disable();
        m_preprocess->Disable(); m_generate->Disable();
        // Disabling selection cancels its worker; let the busy guard block new input.
        if (!beauty_editable && !m_model_preview->selection_busy()) m_model_preview->set_selection_enabled(false);
        if (local) m_finishing_selection_status->SetLabel(wxString::Format(_L("本次处理 %llu 个面 · 未选区域受保护"),
            static_cast<unsigned long long>(m_finishing_options.selected_faces.size())));
    }
    if (m_finishing_running) m_stop->Hide();
    if (m_displayed_model_job_id.rfind("finish-", 0) == 0) {
        m_recheck_model->Disable(); m_visual_review_model->Disable();
        m_recheck_model->SetToolTip(_L("本地处理版本请导入准备页，检查实际打印条件。"));
    }
    wrap_workbench_text(m_finishing_status,
        std::max(FromDIP(80), m_finishing_panel->GetClientSize().x - FromDIP(34)));
    wrap_workbench_text(m_finishing_selection_status, FromDIP(240));
    m_finishing_panel->Layout();
    static_cast<wxScrolledWindow*>(m_finishing_panel)->FitInside();
    if (auto* page = dynamic_cast<wxScrolledWindow*>(m_finishing_panel->GetParent())) {
        page->Layout(); page->FitInside();
    }
    if (m_workbench_history_scroller) {
        const bool can_switch_version = post_generation_ui_state().can_switch_version;
        if (m_workbench_history_upload) m_workbench_history_upload->Enable(can_switch_version);
        const bool switch_state_changed = m_workbench_history_scroller->IsEnabled() != can_switch_version;
        m_workbench_history_scroller->Enable(can_switch_version);
        if (switch_state_changed && m_finishing_workbench)
            refresh_workbench_history();
    }
    if (m_finishing_workbench) {
        const auto state = post_generation_ui_state();
        m_beauty_controls->set_history_permissions(state.can_undo, state.can_redo);
        refresh_post_generation_workbench();
    }
    // Paint the final visible surfaces after thawing, before returning from a mode switch.
    workbench_updates.reset();
    if (repaint_layout) repaint_workbench_surface(m_workbench_shell);
    if (m_save_and_return && !m_busy && !m_finishing_running && !m_workbench_check_running) {
        wxWeakRef<ModelGenerationPanel> weak(this);
        CallAfter([weak] { if (weak && !weak->m_shutdown) weak->finish_workbench_save(); });
    }
}

void ModelGenerationPanel::reset_beauty_asset()
{
    if (m_workbench_check_cancel) m_workbench_check_cancel->store(true);
    if (m_workbench_check_worker.joinable()) m_workbench_check_worker.join();
    m_workbench_check_cancel.reset();
    m_workbench_check_running = m_workbench_auto_repair = false;
    m_workbench_check_path.clear();
    m_workbench_check_result = {};
    clear_unaccepted_beauty_candidates();
    m_finishing_source.clear();
    m_finishing_candidate.clear();
    m_finishing_undo_path.clear();
    m_finishing_accepted_path.clear();
    m_finishing_redo_path.clear();
    m_finishing_redo_source.clear();
    m_finishing_redo_id.clear();
    m_finishing_id.clear();
    m_finishing_source_context = {};
    m_finishing_restore_context = {};
    m_finishing_restore_selection = {};
    m_finishing_redo_preview = {};
    m_finishing_options = {};
    m_finishing_result = {};
    m_finishing_candidate_face_overrides.clear();
    m_finishing_candidate_semantic_faces.clear();
    m_finishing_candidate_semantic_subfaces.clear();
    m_finishing_candidate_semantic_provenance = {};
    m_finishing_candidate_region_evidence.reset();
    m_finishing_candidate_secondary_evidence.reset();
    m_beauty_session_source.reset();
    m_beauty_reoptimization_before.reset();
    m_beauty_accepted_files.clear();
    m_beauty_session_undo_base = m_beauty_session_file_base = 0;
    m_finishing_before = m_finishing_compare_held = m_save_and_return = false;
    m_portrait_mode = false;
    if (m_beauty_controls) m_beauty_controls->reset_asset();
    if (m_beauty_transactions) m_beauty_transactions->reset();
}

std::function<void()> ModelGenerationPanel::capture_model_context()
{
    return [this, job = m_job_id, displayed = m_displayed_model_job_id,
        artifact = m_artifact_path, source = m_displayed_model_path, palette = m_job_palette, roles = m_job_palette_roles,
        display_palette = m_displayed_model_palette, display_roles = m_displayed_model_palette_roles,
        printable = m_job_use_printable_colors, manifest = m_color_intent_path,
        schema = m_color_intent_schema, hash = m_color_intent_sha256, format = m_artifact_format,
        encoding = m_artifact_color_encoding, quality = m_model_quality, visual = m_visual_quality,
        refinement = m_model_refinement, library = m_library_model_loaded] {
        m_job_id = job; m_displayed_model_job_id = displayed;
        m_artifact_path = artifact; m_displayed_model_path = source;
        m_job_palette = palette; m_job_palette_roles = roles;
        m_displayed_model_palette = display_palette; m_displayed_model_palette_roles = display_roles;
        m_job_use_printable_colors = printable; m_color_intent_path = manifest;
        m_color_intent_schema = schema; m_color_intent_sha256 = hash;
        m_artifact_format = format; m_artifact_color_encoding = encoding;
        m_model_quality = quality; m_visual_quality = visual; m_model_refinement = refinement;
        m_library_model_loaded = library; m_ready = m_artifact_download_started = m_model_preview_ready = true;
        m_last_imported_model_path.clear();
    };
}

ModelGenerationPanel::BeautyCandidateSnapshot ModelGenerationPanel::capture_beauty_candidate() const
{
    BeautyCandidateSnapshot snapshot;
    snapshot.source = m_finishing_source.empty() ? m_displayed_model_path : m_finishing_source;
    snapshot.candidate = m_finishing_candidate;
    snapshot.model_sha256 = snapshot.candidate.empty()
        ? (m_beauty_session_source && m_beauty_session_source->source == snapshot.source
            ? m_beauty_session_source->model_sha256 : AI::model_artifact_sha256(snapshot.source))
        : (!m_finishing_result.output_sha256.empty() ? m_finishing_result.output_sha256
            : AI::model_artifact_sha256(snapshot.candidate));
    snapshot.id = m_finishing_id;
    snapshot.geometry_id = m_model_preview->geometry_id();
    snapshot.result = m_finishing_result;
    snapshot.options = m_finishing_options;
    snapshot.selection = m_model_preview->selection_state();
    snapshot.color_trial = m_model_preview->color_trial_state();
    snapshot.face_overrides = m_finishing_candidate.empty()
        ? m_model_preview->face_color_overrides() : m_finishing_candidate_face_overrides;
    snapshot.semantic_faces = m_finishing_candidate.empty() && m_model_preview->semantic_result_active()
        ? m_model_preview->import_face_color_overrides(true) : m_finishing_candidate_semantic_faces;
    snapshot.semantic_subfaces = m_finishing_candidate.empty() && m_model_preview->semantic_result_active()
        ? m_model_preview->import_subface_color_overrides(true) : m_finishing_candidate_semantic_subfaces;
    snapshot.semantic_provenance = m_finishing_candidate_semantic_provenance;
    snapshot.region_evidence = m_model_preview->semantic_region_evidence();
    snapshot.region_evidence_error = m_model_preview->semantic_region_evidence_error();
    snapshot.secondary_evidence = m_model_preview->secondary_region_evidence();
    snapshot.secondary_evidence_error = m_model_preview->secondary_region_evidence_error();
    snapshot.shape_details = m_model_preview->portrait_shape_details();
    snapshot.shapes_unlocked = m_model_preview->portrait_shapes_unlocked();
    snapshot.leaf_edits=m_model_preview->leaf_edit_metadata();
    if (m_beauty_controls) snapshot.partition = m_beauty_controls->capture_partition();
    return snapshot;
}

bool ModelGenerationPanel::restore_beauty_candidate(const BeautyCandidateSnapshot& snapshot)
{
    const auto path = snapshot.candidate.empty() ? snapshot.source : snapshot.candidate;
    if (!is_nonempty_model(path)) return false;
    if (snapshot.model_sha256.empty() || AI::model_artifact_sha256(path) != snapshot.model_sha256) return false;
    const auto view = m_model_preview->view_state();
    size_t triangles = 0, colors = 0;
    Vec3d dimensions;
    std::string error;
    if (!m_model_preview->load_model(path, {}, triangles, dimensions, colors, error,
            snapshot.face_overrides, AI::beauty_metadata_path(path, generated_models_root()))) {
        m_finishing_status->SetLabel(_L("版本恢复失败，当前模型保持不变：") + from_u8(error));
        return false;
    }
    m_model_preview->restore_view(view);
    m_model_preview_ready = true;
    m_workbench_load_error = false;
    m_model_stats->SetLabel(wxString::Format(_L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm"),
        static_cast<unsigned long long>(triangles), static_cast<unsigned long long>(colors),
        dimensions.x(), dimensions.y(), dimensions.z()));
    m_finishing_source = snapshot.source;
    m_finishing_candidate = snapshot.candidate;
    m_finishing_id = snapshot.id;
    m_finishing_result = snapshot.result;
    m_finishing_options = snapshot.options;
    m_finishing_candidate_face_overrides = snapshot.face_overrides;
    m_finishing_candidate_semantic_faces = snapshot.semantic_faces;
    m_finishing_candidate_semantic_subfaces = snapshot.semantic_subfaces;
    m_finishing_candidate_semantic_provenance = snapshot.semantic_provenance;
    m_finishing_candidate_region_evidence = snapshot.region_evidence;
    m_finishing_candidate_region_error = snapshot.region_evidence_error;
    m_finishing_candidate_secondary_evidence = snapshot.secondary_evidence;
    m_finishing_candidate_secondary_error = snapshot.secondary_evidence_error;
    m_model_preview->restore_semantic_region_evidence(snapshot.region_evidence, snapshot.region_evidence_error);
    m_model_preview->restore_secondary_region_evidence(snapshot.secondary_evidence, snapshot.secondary_evidence_error);
    m_model_preview->restore_portrait_shapes(snapshot.shape_details, snapshot.shapes_unlocked);
    m_model_preview->restore_leaf_edits(snapshot.leaf_edits);
    if (m_beauty_controls) m_beauty_controls->restore_partition_snapshot(snapshot.partition);
    m_model_preview->restore_color_trial_without_recognition(snapshot.color_trial);
    if (!snapshot.semantic_faces.empty() || !snapshot.semantic_subfaces.empty()) {
        m_model_preview->set_saved_semantic_result(snapshot.semantic_faces, snapshot.semantic_subfaces);
        m_model_preview->synchronize_project_bound_semantics(snapshot.color_trial, m_model_preview->color_trial_state());
        m_finishing_candidate_semantic_faces = m_model_preview->import_face_color_overrides(true);
        m_finishing_candidate_semantic_subfaces = m_model_preview->import_subface_color_overrides(true);
    }
    if (snapshot.geometry_id == m_model_preview->geometry_id() &&
        snapshot.selection.selected.size() == m_model_preview->triangle_count())
        m_model_preview->restore_selection_state(snapshot.selection);
    m_finishing_selection_state = snapshot.selection;
    m_finishing_before = false;
    m_model_preview->set_selection_preview_suppressed(!snapshot.candidate.empty());
    m_finishing_compare->SetLabel(_L("查看处理前"));
    if (m_beauty_transactions) {
        if (snapshot.candidate.empty()) m_beauty_transactions->mark_editing();
        else m_beauty_transactions->mark_preview_ready();
    }
    refresh_controls();
    update_finishing_selection();
    return true;
}

void ModelGenerationPanel::record_beauty_candidate(BeautyWorkbenchTransactionController::OperationKind kind,
                                                   std::shared_ptr<BeautyCandidateSnapshot> before)
{
    if (!m_finishing_workbench || !m_beauty_transactions || !before) return;
    auto after = std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
    m_beauty_candidate_files.push_back(after->candidate);
    const auto valid = [](const BeautyCandidateSnapshot& snapshot) {
        const auto& path = snapshot.candidate.empty() ? snapshot.source : snapshot.candidate;
        return is_nonempty_model(path) && !snapshot.model_sha256.empty() &&
            AI::model_artifact_sha256(path) == snapshot.model_sha256;
    };
    m_beauty_transactions->record({kind, "Beauty candidate",
        {}, {},
        [before, valid] { return valid(*before); },
        [after, valid] { return valid(*after); },
        [this, before] { return restore_beauty_candidate(*before); },
        [this, after] { return restore_beauty_candidate(*after); }});
}

void ModelGenerationPanel::clear_unaccepted_beauty_candidates(size_t start)
{
    if (start > m_beauty_candidate_files.size()) return;
    for (size_t index = start; index < m_beauty_candidate_files.size(); ++index) {
        const auto& path = m_beauty_candidate_files[index];
        if (path.empty() || path == m_finishing_accepted_path ||
            std::find(m_beauty_accepted_files.begin(), m_beauty_accepted_files.end(), path) != m_beauty_accepted_files.end())
            continue;
        boost::system::error_code ignored;
        boost::filesystem::remove(path, ignored);
    }
    m_beauty_candidate_files.resize(start);
}

} // namespace Slic3r::GUI
