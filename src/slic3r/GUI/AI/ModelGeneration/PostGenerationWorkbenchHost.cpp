#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "slic3r/GUI/Redesign/RedesignMessageDialog.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "ModelPreview3D.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchPrinterFilaments.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPrintPaletteSnapshot.hpp"
#include "ModelGenerationPresentation.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include "WorkbenchModelInspection.hpp"
#include "WorkbenchImportEligibility.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchNativeImportTopology.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <wx/weakref.h>
#include <wx/notebook.h>
#include <wx/stattext.h>
#include <wx/choice.h>
#include <wx/slider.h>
#include <wx/tglbtn.h>
#include <wx/sizer.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>

namespace Slic3r::GUI {

namespace {

void style_workbench_color_confirmation(RedesignConfirmationOptions& options)
{
    options.message_point_size = 11;
    options.action_point_size = 11;
    options.fit_message_to_content = true;
    options.maximum_message_height = 240;
}

}

std::vector<FilamentColorPack> ModelGenerationPanel::project_filament_color_sources() const
{
    return m_model_preview ? m_model_preview->configuration_color_sources() : std::vector<FilamentColorPack>{};
}

void ModelGenerationPanel::mount_workbench(wxWindow* parent)
{
    if (!m_workbench_shell || !parent || m_workbench_shell->GetParent() == parent) return;
    if (auto* sizer = m_workbench_shell->GetContainingSizer()) sizer->Detach(m_workbench_shell);
    m_workbench_shell->Reparent(parent);
    parent->GetSizer()->Add(m_workbench_shell, 1, wxEXPAND);
    m_workbench_shell->Show(m_finishing_workbench);
    parent->Layout();
}

void ModelGenerationPanel::unmount_workbench()
{
    set_workbench_presented(false);
    restore_workbench_history_sidebar();
    restore_workbench_import_history();
    if (!m_workbench_shell || m_workbench_shell->GetParent() == this) return;
    if (auto* sizer = m_workbench_shell->GetContainingSizer()) sizer->Detach(m_workbench_shell);
    m_workbench_shell->Reparent(this);
    GetSizer()->Add(m_workbench_shell, 1, wxEXPAND);
    m_workbench_shell->Hide();
}

wxWindow* ModelGenerationPanel::workbench_import_viewport() const
{
    return m_model_preview ? m_model_preview->workbench_viewport_window() : m_workbench_view_host;
}
wxWindow* ModelGenerationPanel::workbench_import_history() const { return m_workbench_history_panel; }

void ModelGenerationPanel::toggle_workbench_history()
{
    if (m_workbench_import_feedback || m_workbench_import_running) return;
    m_workbench_history_state.toggle(m_workbench_history_sidebar_mounted, m_workbench_editing);
    if (m_workbench_history_sidebar_mounted) {
        layout_mounted_workbench_history_sidebar();
        return;
    }
    if (!m_workbench_editing) {
        update_workbench_history_visibility();
        if (m_workbench_history_panel->IsShown()) reveal_workbench_history();
        else m_workbench_shell->Layout();
        queue_workbench_surface_refresh();
        return;
    }
    m_workbench_editing = false;
    update_finishing_selection();
    refresh_model_finishing(); refresh_post_generation_workbench();
    reveal_workbench_history();
}

bool ModelGenerationPanel::update_workbench_history_visibility()
{
    if (!m_workbench_history_panel || !m_workbench_history_toggle ||
        (m_workbench_history_home && !m_workbench_history_sidebar_mounted)) return false;
    const bool mounted = m_workbench_history_sidebar_mounted;
    auto* toggle = mounted ? m_creation_history_toggle.get() : m_workbench_history_toggle;
    if (!toggle) return false;
    const bool open = m_workbench_history_state.panel_visible(mounted, m_finishing_workbench, m_workbench_editing);
    bool changed = false;
    if (!m_workbench_import_feedback && m_workbench_history_panel->IsShown() != open)
        changed = m_workbench_history_panel->Show(open);
    const bool show_toggle = mounted || m_finishing_workbench;
    if (toggle->IsShown() != show_toggle)
        changed = toggle->Show(show_toggle) || changed;
    const bool return_history = !mounted && m_workbench_editing;
    const wxString action = return_history ? _L("返回历史模型") :
        open ? _L("收起历史模型") : _L("展开历史模型");
    if (toggle->GetName() != action) toggle->SetName(action);
    if (!toggle->GetToolTip() || toggle->GetToolTipText() != action)
        toggle->SetToolTip(action);
    static_cast<WorkbenchButton*>(toggle)->set_drawer_open(return_history || open);
    toggle->Enable(!m_workbench_import_feedback && !m_workbench_import_running);
    return changed;
}

void ModelGenerationPanel::reveal_workbench_history()
{
    if (!m_finishing_workbench || m_workbench_history_sidebar_mounted || m_workbench_history_home ||
        !m_workbench_history_panel || !m_workbench_history_panel->IsShown() || !m_workbench_shell) return;
    auto* shell = static_cast<wxScrolledWindow*>(m_workbench_shell);
    shell->Layout();
    const auto client = shell->GetClientSize();
    if (client.x <= 0 || client.y <= 0) return;
    const auto left = shell->ScreenToClient(m_workbench_history_panel->ClientToScreen(wxPoint())).x;
    int x, y, unit_x, unit_y;
    shell->GetViewStart(&x, &y);
    shell->GetScrollPixelsPerUnit(&unit_x, &unit_y);
    const int target = workbench_history_scroll_target(client.x, left,
        m_workbench_history_panel->GetSize().x, x, unit_x,
        shell->FromDIP(workbench_history_handle_size()).x);
    if (target != x) shell->Scroll(target, -1);
    queue_workbench_surface_refresh();
}

void ModelGenerationPanel::mount_workbench_import_history(wxWindow* parent)
{
    if (!parent || !m_workbench_history_panel || m_workbench_history_home) return;
    m_workbench_import_history_was_shown = m_workbench_history_panel->IsShown();
    m_workbench_history_home = m_workbench_history_panel->GetParent();
    m_workbench_history_toggle->Hide();
    if (auto* sizer = m_workbench_history_panel->GetContainingSizer()) sizer->Detach(m_workbench_history_panel);
    m_workbench_history_panel->Reparent(parent);
    parent->GetSizer()->Add(m_workbench_history_panel, 1, wxEXPAND);
    m_workbench_history_panel->Show(m_workbench_import_history_was_shown);
    parent->Layout();
}

void ModelGenerationPanel::restore_workbench_import_history()
{
    if (m_workbench_history_sidebar_mounted) {
        restore_workbench_history_sidebar();
        return;
    }
    if (!m_workbench_history_home) return;
    if (auto* sizer = m_workbench_history_panel->GetContainingSizer()) sizer->Detach(m_workbench_history_panel);
    m_workbench_history_panel->Reparent(m_workbench_history_home);
    m_workbench_history_home->GetSizer()->Add(m_workbench_history_panel, 0,
        wxEXPAND | wxTOP | wxBOTTOM | wxLEFT, FromDIP(12));
    auto* home = m_workbench_history_home;
    m_workbench_history_home = nullptr;
    update_workbench_history_visibility();
    home->Layout();
    layout_workbench_history_toggle(true);
}

void ModelGenerationPanel::mount_workbench_history_sidebar(wxWindow* parent)
{
    initialize_for_shell_host();
    if (!parent || !parent->GetSizer() || !m_workbench_history_panel || !m_workbench_history_toggle ||
        m_workbench_history_home || m_workbench_history_sidebar_mounted)
        return;

    m_workbench_history_sidebar_mounted = true;
    m_workbench_history_sidebar_host = parent;
    m_workbench_history_home = m_workbench_history_panel->GetParent();
    m_workbench_import_history_was_shown = m_workbench_history_panel->IsShown();
    auto* search_home = m_workbench_history_search->GetParent();
    m_workbench_history_search_home_min_size = search_home->ToDIP(search_home->GetMinSize());
    m_workbench_history_search_home_max_size = search_home->ToDIP(search_home->GetMaxSize());

    if (auto* sizer = m_workbench_history_panel->GetContainingSizer())
        sizer->Detach(m_workbench_history_panel);
    // Keep each page's handle in its permanent native parent. Reparenting a
    // painted handle through a hidden host can leave its visible surface blank.
    if (!m_creation_history_toggle) {
        auto* toggle = new WorkbenchButton(parent, wxEmptyString);
        toggle->SetCornerRadius(0);
        toggle->SetPaddingSize(FromDIP(wxSize(0, 0)));
        toggle->SetBackgroundColour(wxColour(49, 49, 54));
        toggle->set_drawer_assets();
        wxWeakRef<ModelGenerationPanel> weak(this);
        toggle->Bind(wxEVT_BUTTON, [weak](wxCommandEvent&) {
            if (weak && !weak->m_shutdown) weak->toggle_workbench_history();
        });
        parent->GetSizer()->Add(toggle, 0, wxALIGN_CENTER_VERTICAL);
        m_creation_history_toggle = toggle;
    }
    m_workbench_history_panel->Reparent(parent);
    parent->GetSizer()->Add(m_workbench_history_panel, 1, wxEXPAND);

    layout_mounted_workbench_history_sidebar();
    load_library_entries(true);
    refresh_workbench_history();
}

void ModelGenerationPanel::layout_mounted_workbench_history_sidebar()
{
    if (!m_workbench_history_sidebar_mounted || !m_workbench_history_sidebar_host ||
        !m_workbench_history_panel || !m_creation_history_toggle || !m_workbench_history_search)
        return;

    update_workbench_history_visibility();
    const wxSize toggle_size = m_workbench_history_sidebar_host->FromDIP(workbench_history_handle_size());
    auto* toggle = m_creation_history_toggle.get();
    if (toggle->GetMinSize() != toggle_size)
        static_cast<WorkbenchButton*>(toggle)->rescale_workbench();
    toggle->SetMinSize(toggle_size);
    toggle->SetMaxSize(toggle_size);
    toggle->SetSize(toggle_size);

    auto* search = m_workbench_history_search->GetParent();
    search->SetMinSize(FromDIP(wxSize(212, 32)));
    search->SetMaxSize(wxSize(-1, FromDIP(32)));
    search->SetBackgroundColour(wxColour(49, 49, 54));
    m_workbench_history_search->SetBackgroundColour(wxColour(49, 49, 54));
    m_workbench_history_search->SetForegroundColour(wxColour(235, 235, 235));
    search->Refresh(false);
    m_workbench_history_search->Refresh(false);

    const bool open = m_workbench_history_state.creation_open;
    const int panel_width = std::max(0, m_workbench_history_panel->GetMinSize().x);
    m_workbench_history_sidebar_host->SetMinSize(wxSize(open ? toggle_size.x + panel_width : toggle_size.x, -1));
    m_workbench_history_sidebar_host->InvalidateBestSize();
    m_workbench_history_sidebar_host->Layout();
    if (auto* parent = m_workbench_history_sidebar_host->GetParent()) {
        parent->Layout();
        parent->Refresh(false);
    }
}

void ModelGenerationPanel::restore_workbench_history_sidebar()
{
    if (!m_workbench_history_sidebar_mounted)
        return;

    auto* sidebar_host = m_workbench_history_sidebar_host;
    if (m_creation_history_toggle) m_creation_history_toggle->Hide();
    if (m_workbench_history_search) {
        auto* search = m_workbench_history_search->GetParent();
        search->SetMinSize(search->FromDIP(m_workbench_history_search_home_min_size));
        search->SetMaxSize(search->FromDIP(m_workbench_history_search_home_max_size));
    }
    m_workbench_history_sidebar_host = nullptr;
    m_workbench_history_sidebar_mounted = false;
    restore_workbench_import_history();
    if (sidebar_host) {
        sidebar_host->SetMinSize(wxDefaultSize);
        sidebar_host->InvalidateBestSize();
        sidebar_host->Layout();
        if (sidebar_host->GetParent()) sidebar_host->GetParent()->Layout();
    }
    layout_workbench_history_toggle(true);
}

void ModelGenerationPanel::set_workbench_import_feedback(bool active)
{
    if (active == m_workbench_import_feedback) return;
    if (active) m_workbench_import_history_was_shown = m_workbench_history_panel->IsShown();
    m_workbench_import_feedback = active;
    m_workbench_settings->Enable(!active);
    if (m_model_preview) m_model_preview->set_workbench_input_locked(active);
    m_workbench_history_toggle->Enable(!active);
    refresh_workbench_history();
}

std::vector<BeautyColorItem> ModelGenerationPanel::local_recolor_palette() const
{
    std::vector<BeautyColorItem> palette;
    if (wxGetApp().preset_bundle) {
        const auto filaments = PrinterFilamentConfig::capture(*wxGetApp().preset_bundle);
        for (const auto& slot : filaments.slots) {
            if (!slot.exists || !slot.configured || !slot.compatible || !AI::is_rgb_hex_color(slot.color)) continue;
            palette.push_back({slot.color, u8"耗材 " + std::to_string(slot.slot + 1),
                               slot.slot, BeautyColorSource::Physical});
        }
        for (const auto& slot : filaments.mixed) {
            if (!slot.valid || !AI::is_rgb_hex_color(slot.color)) continue;
            palette.push_back({slot.color, u8"叠色 " + std::to_string(slot.slot + 1),
                               slot.slot, BeautyColorSource::Mixed});
        }
    }
    // Beauty editing is print-bound: an unavailable live native palette must
    // stay unavailable instead of silently exposing arbitrary project/model
    // RGB values. The entry gate will explain how to restore compatibility.
    if (!palette.empty() || (m_finishing_workbench && m_workbench_editing)) return palette;

    auto fallback = project_palette();
    if (fallback.empty())
        fallback = !m_displayed_model_palette.empty() ? m_displayed_model_palette : m_job_palette;
    if (fallback.size() > PreviewPalette::max_preview_colors)
        fallback.resize(PreviewPalette::max_preview_colors);
    for (size_t slot = 0; slot < fallback.size(); ++slot) {
        if (!AI::is_rgb_hex_color(fallback[slot])) continue;
        palette.push_back({fallback[slot], u8"耗材 " + std::to_string(slot + 1),
                           slot, BeautyColorSource::Physical});
    }
    return palette;
}

bool ModelGenerationPanel::beauty_color_match_current() const
{
    if (!m_model_preview || !m_beauty_matched_colors ||
        !m_beauty_matched_colors->valid() ||
        m_beauty_color_gate_state != BeautyColorGateState::Compatible)
        return false;
    const auto* bundle = wxGetApp().preset_bundle;
    if (!bundle) return false;
    const auto configuration = PrinterFilamentConfig::capture(*bundle);
    const auto native = OrcaPrintPaletteSnapshot::capture(*bundle);
    const auto& matched = *m_beauty_matched_colors;
    const std::string source_sha256 = m_beauty_session.source()
        ? m_beauty_session.source()->model_sha256
        : AI::model_artifact_sha256(m_displayed_model_path);
    const std::string geometry_id = m_beauty_session.source()
        ? m_beauty_session.source()->geometry_id : m_model_preview->geometry_id();
    return matched.source_sha256 == source_sha256 && matched.geometry_id == geometry_id &&
        configuration.signature == m_beauty_color_gate_configuration_signature &&
        matched.material_fingerprint == native.material_fingerprint &&
        matched.process_fingerprint == native.process_fingerprint;
}

bool ModelGenerationPanel::beauty_session_has_changes() const
{
    if (!m_workbench_editing) return false;
    return beauty_workbench_has_unsaved_changes(
        !m_finishing_candidate.empty(), m_beauty_manual_color_dirty,
        m_beauty_controls && m_beauty_controls->has_changes(), bool(m_beauty_compare_draft));
}

void ModelGenerationPanel::update_region_mode()
{
    if (!m_model_preview || !m_finishing_selection_operation) return;
    const auto gesture = static_cast<ModelPreview3D::SelectionGesture>(m_finishing_selection_operation->GetSelection());
    m_model_preview->set_selection_gesture(gesture, FromDIP(m_finishing_radius->GetValue() * 4));
    m_model_preview->set_selection_operation(AI::RegionSelectionOperation::AddSimilar);
    m_model_preview->set_selection_settings(AI::RegionSelectionSettings {});
    m_model_preview->set_selection_preview_color(ColorRGBA(1.0f, 0.55f, 0.0f, 1.0f));
}


PostGenerationWorkbenchState ModelGenerationPanel::workbench_snapshot() const
{
    PostGenerationWorkbenchState state;
    state.actions = post_generation_ui_state();
    state.revision = m_sequence;
    state.asset_id = m_displayed_model_job_id;
    state.model_path = m_displayed_model_path.string();
    if (native_workbench_context_matches(m_native_workbench_context)) {
        state.source_sha256 = m_native_workbench_context.source_sha256;
        state.geometry_id = m_native_workbench_context.geometry_id;
        state.appearance_id = m_native_workbench_context.appearance_id;
    }
    state.candidate_path = m_finishing_candidate.string();
    for (const auto& color : local_recolor_palette()) state.palette.push_back(color.color);
    if (m_model_preview) {
        const auto trial = m_model_preview->color_trial_state();
        state.automatic_palette_requested_count = trial.automatic_count > 0
            ? size_t(trial.automatic_count) : 0;
        state.automatic_palette_active = workbench_default_ai_palette_ready(
            trial.enabled, trial.source, trial.automatic_count, !trial.suite_id.empty(),
            !trial.automatic_basis_signature.empty());
    }
    state.editing = m_workbench_editing;
    state.workbench_presented = m_workbench_presented;
    state.modal_blocking = workbench_modal_navigation_blocked(m_finishing_workbench,
        m_workbench_presented, m_workbench_honor_semantic_active, m_workbench_check_card_visible,
        m_workbench_check_running, m_workbench_auto_repair && m_finishing_running,
        m_native_workbench_partition_running, m_workbench_check_result);
    state.semantic_mode = m_semantic_mode;
    if (m_portrait_task) state.portrait_optimization = m_portrait_task->snapshot();
    state.portrait_enabled = m_portrait_mode;
    const auto portrait = m_model_preview ? m_model_preview->portrait_shape_details() : nullptr;
    state.portrait_available = m_model_preview && m_model_preview->active_shape_locks();
    if (!state.portrait_available) state.portrait_unavailable_reason = portrait
        ? "当前保护边界与运行时不兼容，或没有可保护的细节。请使用匹配的 R6 运行时与保护数据。"
        : "当前模型缺少通过身份校验的人像保护数据。";
    state.dirty = m_beauty_controls && m_beauty_controls->has_changes();
    state.project_palette = m_palette_provider.printable_palette();
    state.project_channels = m_project_channels_provider ? m_project_channels_provider() : state.project_palette.physical_channels;
    if (wxGetApp().preset_bundle) state.printer_filaments = PrinterFilamentConfig::capture(*wxGetApp().preset_bundle);
    state.can_edit_project_colors = state.actions.can_edit && m_finishing_candidate.empty() && bool(m_project_color_edit);
    state.can_reoptimize_regions = workbench_region_optimization_allowed(state.actions,
        !m_finishing_candidate.empty(), m_model_preview && m_model_preview->semantic_reoptimization_available());
    if (!state.can_reoptimize_regions) state.reoptimization_reason = m_model_preview
        ? m_model_preview->semantic_reoptimization_reason().ToUTF8().data() : "当前没有已加载模型。";
    state.actions.can_switch_version = state.actions.can_switch_version && !state.dirty;
    state.actions.can_import = state.actions.can_import && !state.dirty;
    if (m_workbench_check_path == state.model_path && m_workbench_check_revision == state.revision)
        state.check = m_workbench_check_result;
    if (!state.candidate_path.empty() || state.dirty) {
        // Keep the original and candidate topology reports visible while a
        // repair is awaiting confirmation. Import remains blocked until the
        // candidate is accepted and checked again.
        if (state.candidate_path.empty()) {
            state.check = {};
            state.check.summary = "存在未保存修改，接受并保存后自动复检。";
        }
    }
    const bool portrait_blocked = state.portrait_optimization.running() || state.portrait_optimization.outcome == PortraitOutcome::DraftOnly;
    state.can_reoptimize_regions = state.can_reoptimize_regions && !portrait_blocked;
    state.actions.can_switch_version = state.actions.can_switch_version && !portrait_blocked;
    state.actions.can_import = state.actions.can_import && !portrait_blocked;
    state.can_edit_project_colors = state.can_edit_project_colors && !portrait_blocked;
    state.can_apply_printer_filaments = state.can_edit_project_colors && state.printer_filaments.ready &&
        !(m_model_preview && m_model_preview->selection_busy());
    state.apply_filaments_reason = !state.printer_filaments.ready ? state.printer_filaments.reason :
        !state.can_apply_printer_filaments ? "模型处理中或存在未接受候选，请先完成当前操作" : "";
    const bool candidate_pending = !state.candidate_path.empty() ||
        state.check.candidate_status == RepairCandidateStatus::Generating ||
        state.check.candidate_status == RepairCandidateStatus::Ready;
    const bool topology_safe = workbench_topology_safe(state.check);
    state.can_import_for_slicing = state.actions.can_import && !candidate_pending && !state.dirty && topology_safe;
    // Portrait v9 has already completed native repair before clustering.
    // Legacy deferred repair is intentionally never advertised to import.
    state.deferred_native_repair = false;
    state.native_repair_source_sha256.clear();
    // Failed checks never authorize a risky import. Detailed topology remains
    // available for diagnosis while the user can retry or select another asset.
    state.force_import_acknowledged = false;
    state.can_force_import = false;
    state.import_has_topology_risk = false;
    if (!m_workbench_check_running && !m_native_workbench_partition_running &&
        (state.check.status == WorkbenchCheckStatus::Attention || state.check.status == WorkbenchCheckStatus::Invalid))
        state.check.status = WorkbenchCheckStatus::Failed;
    if (m_model_preview && !m_model_preview->project_matching_valid() &&
        state.printer_filaments.ready && state.can_edit_project_colors) {
        // The old slot identity is only a preference. Import rebuilds an exact
        // RGB8 binding from the current physical and mixed slots, so a changed
        // layout must reach that coverage check instead of being disabled here.
        state.apply_filaments_reason = "耗材配置已变化；导入时将按当前颜色重新校验，也可重新映射模型";
    }
    if (m_model_preview) {
        state.faces = m_model_preview->triangle_count();
        state.vertices = m_model_preview->vertex_count();
    }
    return state;
}

bool ModelGenerationPanel::can_replace_model_asset() const
{
    return !m_shutdown && !m_workbench_import_feedback && !m_portrait_draft_before &&
        !(m_portrait_task && m_portrait_task->snapshot().running()) && post_generation_asset_switch_allowed(
        m_busy || m_preview_download_in_flight || m_workbench_check_running || m_workbench_import_running,
        m_finishing_running,
        (m_beauty_transactions && m_beauty_transactions->processing()) ||
            (m_model_preview && m_model_preview->semantic_processing()),
        !m_finishing_candidate.empty(), m_finishing_before, m_beauty_controls && m_beauty_controls->has_changes());
}


void ModelGenerationPanel::set_workbench_import_running(bool running)
{
    m_workbench_import_running = running;
    refresh_post_generation_workbench();
    publish_workbench_state();
}

void ModelGenerationPanel::set_workbench_listener(PostGenerationWorkbenchListener listener)
{
    m_workbench_listener = std::move(listener);
    publish_workbench_state();
}

void ModelGenerationPanel::publish_workbench_state()
{
    if (!m_shutdown && m_workbench_listener) m_workbench_listener(workbench_snapshot());
}

void ModelGenerationPanel::set_workbench_presented(bool presented)
{
    const bool active = presented && !m_shutdown && m_finishing_workbench &&
        m_workbench_shell && m_workbench_shell->IsShownOnScreen();
    if (m_workbench_presented == active) {
        if (active) {
            queue_workbench_surface_refresh();
            // Resize/restore is a presentation request, not a new model
            // entry. Schedule missing checks without rehashing the asset.
            queue_workbench_check();
        }
        return;
    }

    m_workbench_presented = active;
    ++m_workbench_presentation_sequence;
    if (!active) {
        if (m_workbench_check_running && m_workbench_check_cancel)
            m_workbench_check_cancel->store(true);
        m_workbench_check_card_visible = false;
        m_workbench_honor_semantic_active = false;
        m_workbench_honor_semantic_progress = 0;
        m_workbench_honor_semantic_context.clear();
        m_workbench_honor_semantic_geometry.clear();
        m_workbench_honor_semantic_message.clear();
        hide_workbench_check_card();
    } else {
        reveal_workbench_history();
        queue_workbench_surface_refresh();
        ensure_workbench_check();
    }
    publish_workbench_state();
}

bool ModelGenerationPanel::request_open_workbench()
{
    if (m_shutdown) return false;
    initialize_for_shell_host();
    if (!m_page_initialized || m_busy || !finishing_workbench_layout_ready(true)) {
        BOOST_LOG_TRIVIAL(error) << "Workbench open rejected before a complete model layout was available: page_initialized="
            << m_page_initialized << ", busy=" << m_busy
            << ", model_page=" << bool(m_model_page)
            << ", comparison=" << bool(m_comparison_panel)
            << ", workbench_host=" << bool(m_workbench_view_host)
            << ", workbench_shell=" << bool(m_workbench_shell);
        return false;
    }
    const bool opening = !m_finishing_workbench;
    if (!m_model_preview_ready) {
        m_workbench_editing = false;
    }
    // Publish exact ownership before presenting the workbench can start an
    // automatic check or any partition consumer.
    publish_native_workbench_context();
    set_finishing_workbench(true);
    if (opening) {
        m_portrait_mode = false;
        if (m_beauty_controls) m_beauty_controls->set_portrait_enabled(false);
    }
    load_library_entries(true);
    refresh_post_generation_workbench();
    synchronize_workbench_project_palette();
    publish_workbench_state();
    return true;
}

bool ModelGenerationPanel::request_enter_beauty()
{
    if (!post_generation_ui_state().can_edit) return false;
    return request_beauty_color_gate(true);
}

bool ModelGenerationPanel::request_return_overview()
{
    if (m_shutdown) return false;
    const auto ui = post_generation_ui_state();
    const bool processing = ui.status == PostGenerationUiState::Status::Loading ||
        ui.status == PostGenerationUiState::Status::Processing ||
        m_beauty_color_gate_state == BeautyColorGateState::Checking ||
        (m_beauty_controls && m_beauty_controls->partitioning());
    const auto leave = derive_beauty_workbench_leave_state(processing, beauty_session_has_changes());
    if (m_workbench_editing && leave == BeautyWorkbenchLeaveState::Processing) {
        m_workbench_operation_notice = "当前处理尚未结束，请先等待完成或取消处理。";
        if (m_finishing_status) m_finishing_status->SetLabel(_L("当前处理尚未结束，请先等待完成或取消处理。"));
        refresh_model_finishing();
        return false;
    }

    // Comparison is a display-only state. Restore the draft before asking what
    // to do with it so Save always operates on the edited version.
    if (m_workbench_editing && m_finishing_before) {
        bool restored = false;
        if (m_beauty_compare_draft)
            restored = restore_beauty_candidate(*m_beauty_compare_draft);
        else if (!m_finishing_candidate.empty())
            restored = show_finishing_version(m_finishing_candidate);
        if (!restored) {
            m_workbench_operation_notice = "无法恢复修改后预览，已留在美颜工作台。";
            refresh_model_finishing();
            return false;
        }
        m_beauty_compare_draft.reset();
        m_finishing_before = false;
    }

    if (m_workbench_editing && leave == BeautyWorkbenchLeaveState::ConfirmChanges) {
        const bool can_save = beauty_color_match_current();
        RedesignConfirmationOptions options;
        options.style = wxYES_NO | wxCANCEL | wxCANCEL_DEFAULT;
        options.dismiss_result = wxID_CANCEL;
        style_workbench_color_confirmation(options);
        if (can_save)
            options.actions = {{wxID_YES, _L("保存并返回"), true},
                               {wxID_NO, _L("放弃修改并返回"), false},
                               {wxID_CANCEL, _L("留在此处"), false}};
        else
            options.actions = {{wxID_NO, _L("放弃修改并返回"), false},
                               {wxID_CANCEL, _L("留在此处"), false}};
        const int answer = show_redesign_confirmation(this,
            can_save
                ? _L("当前修改尚未保存。保存会生成新版本，原模型不会被覆盖。")
                : _L("当前耗材配置与模型配色不兼容，无法保存。可以放弃修改并返回，或留在此处恢复兼容配置。"),
            _L("返回模型总览"), options);
        if (can_save && answer == wxID_YES) return request_save_and_return();
        if (answer != wxID_NO) return false;
        discard_model_finishing();
        if (beauty_session_has_changes()) return false;
    }
    complete_return_overview();
    return true;
}

void ModelGenerationPanel::complete_return_overview()
{
    m_beauty_entry_pending = false;
    m_beauty_compare_draft.reset();
    m_beauty_session.clear_draft();
    m_finishing_before = false;
    m_workbench_editing = false;
    update_finishing_selection();
    refresh_model_finishing();
    refresh_post_generation_workbench();
}

bool ModelGenerationPanel::request_workbench_color_matching()
{
    const auto state = workbench_snapshot();
    if ((!state.can_import_for_slicing && !state.can_force_import && !state.deferred_native_repair) || (!m_color_matching && !m_workbench_import)) return false;
    if (!state.check.model_sha256.empty() &&
        AI::model_artifact_sha256(m_displayed_model_path) != state.check.model_sha256) {
        m_workbench_check_result = {};
        m_workbench_check_result.status = WorkbenchCheckStatus::Failed;
        m_workbench_check_result.summary = "模型文件已变化，请重新执行检查后再导入。";
        refresh_post_generation_workbench();
        return false;
    }
    AI::GeneratedModelArtifact artifact;
    artifact.local_path = m_displayed_model_path.string();
    artifact.job_id = m_displayed_model_job_id;
    artifact.format = m_artifact_format;
    artifact.color_encoding = m_artifact_color_encoding;
    artifact.generation_palette = m_displayed_model_palette;
    if (m_workbench_import) {
        AI::ModelImportRequest request;
        request.artifact = artifact;
        request.color_mode = AI::ImportColorMode::NativeMatch;
        request.use_saved_printer_match = false;
        auto* bundle = wxGetApp().preset_bundle;
        if (const auto* honor_suite = m_model_preview->active_honor_filament_suite()) {
            const auto compatibility = bundle
                ? PrinterFilamentConfig::honor_suite_compatibility(*bundle, *honor_suite)
                : PrinterFilamentConfig::HonorSuiteCompatibility{};
            if (!compatibility.compatible) {
                const wxString reason = wxString::FromUTF8(compatibility.reason.empty()
                    ? "当前打印机不支持六实体色荣耀耗材套装" : compatibility.reason);
                const wxString message = _L("当前模型使用荣耀六实体色耗材套装，但打印机配置不兼容。\n\n") + reason +
                    _L("\n\n推荐将当前真实耗材色映射到模型，此操作不会改变打印机耗材配置。完成后请确认预览并再次点击“去切片”。");
                const int map_to_model = wxWindow::NewControlId();
                const int return_to_workbench = wxWindow::NewControlId();
                RedesignConfirmationOptions options;
                options.style = 0;
                options.dismiss_result = return_to_workbench;
                style_workbench_color_confirmation(options);
                options.actions = {{map_to_model, _L("将耗材色映射到模型"), true},
                                   {return_to_workbench, _L("返回工作台"), false}};
                const int answer = show_redesign_confirmation(this, message, _L("套装与打印机不兼容"), options);
                if (answer == map_to_model) apply_workbench_printer_filaments(true);
                return false;
            }
        }
        const auto match = bundle ? m_model_preview->printer_filament_match_result(
            *bundle, AI::model_artifact_sha256(m_displayed_model_path)) : ModelPreview3D::PrinterFilamentMatchResult{};
        if (!match.matched) {
            if (!match.error.empty() || match.missing_colors.empty()) {
                m_workbench_operation_notice = match.error.empty()
                    ? "当前没有可用的打印机耗材配置。" : match.error;
                refresh_post_generation_workbench();
                return false;
            }

            wxString missing;
            const size_t visible = std::min<size_t>(match.missing_colors.size(), 8);
            for (size_t index = 0; index < visible; ++index) {
                if (index) missing += _L("、");
                missing += wxString::FromUTF8(match.missing_colors[index]);
            }
            if (match.missing_colors.size() > visible)
                missing += wxString::Format(_L(" 等 %llu 色"),
                    static_cast<unsigned long long>(match.missing_colors.size()));
            const wxString message = _L("当前模型配色和打印机耗材不匹配。\n\n缺少：") + missing +
                _L("\n\n推荐“将耗材色映射到模型”，此操作不会改变当前打印机耗材配置。\n") +
                _L("如需保留模型当前配色，请选择“将模型配色导入到耗材”。\n") +
                _L("处理完成后请再次点击“去切片”。");
            const int map_to_model = wxWindow::NewControlId();
            const int import_to_filaments = wxWindow::NewControlId();
            RedesignConfirmationOptions options;
            options.style = wxCANCEL | wxCANCEL_DEFAULT;
            options.dismiss_result = wxID_CANCEL;
            style_workbench_color_confirmation(options);
            options.actions = {{map_to_model, _L("将耗材色映射到模型"), true},
                               {import_to_filaments, _L("将模型配色导入到耗材"), false}};
            const int answer = show_redesign_confirmation(this, message, _L("配色不匹配"), options);
            if (answer == map_to_model) apply_workbench_printer_filaments();
            else if (answer == import_to_filaments) toggle_printer_filament_trial();
            return false;
        }
        request.matched_colors = *match.matched;
        if (m_beauty_manual_color_dirty || (m_beauty_controls && m_beauty_controls->has_changes())) {
            m_workbench_operation_notice = "模型存在未保存的局部改色，请先保存后再导入。";
            refresh_post_generation_workbench();
            return false;
        }
        request.face_color_geometry_id = m_model_preview->geometry_id();
        request.topology_risk_confirmed = state.force_import_acknowledged && !state.can_import_for_slicing;
        request.topology_risk_reason = state.import_topology_reason;
        request.topology_check_sha256 = state.check.model_sha256;
        request.native_repair_before_slice = state.deferred_native_repair;
        request.native_repair_source_sha256 = state.native_repair_source_sha256;
        request.apply_round_base_preparation = m_model_preview->round_base_preview_enabled();
        request.base_binding = m_model_preview->color_trial_state().base_binding;
        if (!workbench_import_request_allowed(state, request)) {
            m_workbench_operation_notice = state.check.status == WorkbenchCheckStatus::Attention
                ? "请先在检查提示中确认继续导入；当前检查结果只绑定本次模型。"
                : "当前模型未通过可导入检查，无法进入切片。";
            refresh_post_generation_workbench();
            return false;
        }
        try {
            prepare_portrait_import(request);
            BeautyWorkbenchControls::prepare_import(m_displayed_model_path, request);
            nlohmann::json owned_native_reference, owned_native_check;
            if (native_workbench_context_matches(m_native_workbench_context) &&
                WorkbenchArtifacts::native_partition_has_exact_mesh(m_native_workbench_partition_reference)) {
                // Reopened accepted native versions retain their owned checkpoint.
                owned_native_reference = m_native_workbench_partition_reference;
                owned_native_check = m_native_workbench_partition_certificate;
            }
            if (owned_native_reference.is_object()) {
                if (m_model_preview->has_native_portrait_partition_conflict() ||
                    !m_model_preview->import_subface_color_overrides(true).empty() ||
                    owned_native_reference.at("geometry_id") != m_model_preview->geometry_id() ||
                    owned_native_reference.at("appearance_id") != m_model_preview->semantic_source_content_id())
                    throw std::runtime_error("Native portrait import contains uncommitted geometry or subfaces.");
                freeze_native_import_topology_proof(request,
                    owned_native_reference, owned_native_check);
            }
        }
        catch (const std::exception& error) {
            wxMessageBox(wxString::FromUTF8(error.what()), _L("配色确认"), wxOK | wxICON_ERROR, this);
            return false;
        }
        m_workbench_import(request);
    } else m_color_matching(artifact);
    return true;
}

bool ModelGenerationPanel::request_enable_portrait(bool enabled)
{
    if (enabled && !workbench_snapshot().portrait_available) return false;
    if (!post_generation_ui_state().can_edit) return false;
    m_portrait_mode = enabled;
    if (m_beauty_controls) m_beauty_controls->set_portrait_enabled(enabled);
    refresh_model_finishing();
    publish_workbench_state();
    return true;
}

bool ModelGenerationPanel::request_workbench_results()
{
    if (m_shutdown || m_finishing_running || workbench_snapshot().modal_blocking) {
        raise_workbench_modal();
        return false;
    }
    set_finishing_workbench(false);
    if (m_workbench_results) m_workbench_results();
    return true;
}

bool ModelGenerationPanel::request_save_and_return()
{
    if (!post_generation_ui_state().can_edit) return false;
    if (m_workbench_editing) {
        if (!beauty_color_match_current()) {
            m_finishing_status->SetLabel(_L("当前模型配色尚未完整绑定到耗材；已暂停保存，请恢复兼容配置后重试。"));
            m_workbench_operation_notice = m_finishing_status->GetLabel().ToUTF8().data();
            refresh_controls();
            return false;
        }
    }
    m_model_preview->synchronize_project_colors();
    // Applying printer filaments deliberately starts from an invalidated
    // project match: the staged bundle contains the new slot identities and
    // must be committed before the match can become valid again.  The normal
    // save path still requires a valid match, while the application path is
    // protected by commit_printer_filament_application()'s source signature
    // check and rolls back on any failure.
    if (!m_apply_filaments_saving && !m_model_preview->project_matching_valid()) {
        m_finishing_status->SetLabel(_L("打印机或槽位已变化，请重新进入打印机耗材试色后保存。"));
        m_status->SetLabel(m_finishing_status->GetLabel());
        m_workbench_operation_notice = "打印机或槽位已变化，请重新进入打印机耗材试色后保存。";
        refresh_controls();
        publish_workbench_state();
        return false;
    }
    m_save_and_return = true;
    if (m_beauty_manual_color_dirty) {
        if (m_beauty_transactions && !m_beauty_transactions->begin(
                BeautyWorkbenchTransactionController::OperationKind::AppearanceRecolor)) {
            m_save_and_return = false;
            return false;
        }
        export_semantic_candidate();
        if (!m_finishing_running) m_save_and_return = false;
    }
    else if (!m_finishing_candidate.empty()) accept_model_finishing();
    else if (m_beauty_controls && m_beauty_controls->has_changes()) {
        if (m_model_preview->leaf_editing() || !m_model_preview->face_color_overrides().empty()) {
            if (m_beauty_transactions && !m_beauty_transactions->begin(
                    BeautyWorkbenchTransactionController::OperationKind::AppearanceRecolor)) {
                m_save_and_return = false;
                return false;
            }
            export_semantic_candidate();
        } else preview_model_finishing();
        if (!m_finishing_running) m_save_and_return = false;
    }
    else {
        m_save_and_return = false;
        complete_return_overview();
        ensure_workbench_check();
    }
    return true;
}

void ModelGenerationPanel::finish_workbench_save()
{
    if (!m_save_and_return || m_finishing_running || m_workbench_check_running || m_busy) return;
    if (!m_finishing_candidate.empty() && m_finishing_result.success) {
        accept_model_finishing();
        if (!m_finishing_running) m_save_and_return = false;
    } else if (m_finishing_result.success && (!m_beauty_controls || !m_beauty_controls->has_changes())) {
        m_save_and_return = false;
        complete_return_overview();
        ensure_workbench_check();
    } else m_save_and_return = false;
}

} // namespace Slic3r::GUI
