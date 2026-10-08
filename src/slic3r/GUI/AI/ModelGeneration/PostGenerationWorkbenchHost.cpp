#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "ModelPreview3D.hpp"
#include "ModelGenerationPresentation.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
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

namespace Slic3r::GUI {

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
    if (!m_workbench_shell || m_workbench_shell->GetParent() == this) return;
    if (auto* sizer = m_workbench_shell->GetContainingSizer()) sizer->Detach(m_workbench_shell);
    m_workbench_shell->Reparent(this);
    GetSizer()->Add(m_workbench_shell, 1, wxEXPAND);
    m_workbench_shell->Hide();
}

std::vector<std::string> ModelGenerationPanel::local_recolor_palette() const
{
    std::vector<std::string> palette = project_palette();
    if (palette.empty())
        palette = !m_displayed_model_palette.empty() ? m_displayed_model_palette : m_job_palette;
    if (palette.size() > Slic3r::AI::kMaxPhysicalColorChannels)
        palette.resize(Slic3r::AI::kMaxPhysicalColorChannels);
    return palette;
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
    state.candidate_path = m_finishing_candidate.string();
    state.palette = local_recolor_palette();
    state.editing = m_workbench_editing;
    state.portrait_enabled = m_portrait_mode;
    const auto portrait = m_model_preview ? m_model_preview->portrait_shape_details() : nullptr;
    state.portrait_available = portrait && !portrait->locks.empty();
    if (!state.portrait_available) state.portrait_unavailable_reason = portrait
        ? "当前保护边界与运行时不兼容，或没有可保护的细节。请使用匹配的 R6 运行时与保护数据。"
        : "当前模型缺少通过身份校验的人像保护数据。";
    state.dirty = m_beauty_controls && m_beauty_controls->has_changes();
    state.project_palette = m_palette_provider.printable_palette();
    state.project_channels = m_project_channels_provider ? m_project_channels_provider() : state.project_palette.physical_channels;
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
        state.check = {};
        state.check.summary = "存在候选或未保存修改，接受并保存后自动复检。";
    }
    state.can_import_for_slicing = state.actions.can_import && state.check.status != WorkbenchCheckStatus::Running;
    if (m_model_preview) {
        state.faces = m_model_preview->triangle_count();
        state.vertices = m_model_preview->vertex_count();
    }
    return state;
}

bool ModelGenerationPanel::can_replace_model_asset() const
{
    return !m_shutdown && post_generation_asset_switch_allowed(m_busy || m_preview_download_in_flight || m_workbench_check_running || m_workbench_import_running,
        m_finishing_running, (m_beauty_transactions && m_beauty_transactions->processing()) ||
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

bool ModelGenerationPanel::request_open_workbench()
{
    if (m_shutdown || !m_page_initialized || m_busy) return false;
    const bool opening = !m_finishing_workbench;
    if (!m_model_preview_ready) {
        m_workbench_editing = false;
        m_workbench_history_collapsed = false;
    }
    set_finishing_workbench(true);
    if (opening) {
        m_portrait_mode = false;
        if (m_beauty_controls) m_beauty_controls->set_portrait_enabled(false);
    }
    load_library_entries(true);
    refresh_post_generation_workbench();
    synchronize_workbench_project_palette();
    ensure_workbench_check();
    publish_workbench_state();
    return true;
}

bool ModelGenerationPanel::request_enter_beauty()
{
    if (!post_generation_ui_state().can_edit) return false;
    m_workbench_editing = true;
    update_finishing_selection();
    refresh_model_finishing();
    refresh_post_generation_workbench();
    return true;
}

bool ModelGenerationPanel::request_return_overview()
{
    if (m_shutdown) return false;
    m_workbench_editing = false;
    update_finishing_selection();
    refresh_model_finishing();
    refresh_post_generation_workbench();
    return true;
}

bool ModelGenerationPanel::request_workbench_color_matching()
{
    const auto state = workbench_snapshot();
    if (!state.can_import_for_slicing || (!m_color_matching && !m_workbench_import)) return false;
    if (state.check.status == WorkbenchCheckStatus::NotRun || state.check.status == WorkbenchCheckStatus::Invalid ||
        state.check.status == WorkbenchCheckStatus::Failed) {
        const auto summary = state.check.status == WorkbenchCheckStatus::NotRun ? _L("尚未执行模型检查。") :
            wxString::FromUTF8(state.check.summary);
        if (wxMessageBox(summary + "\n" + _L("继续使用 Orca 原生导入校验？"), _L("模型检查"),
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, m_workbench_shell) != wxYES) return false;
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
        const int mode = m_import_color_mode ? m_import_color_mode->GetSelection() : 0;
        request.color_mode = mode == 1 ? AI::ImportColorMode::AutoMap : mode == 2 ? AI::ImportColorMode::SingleColor :
            mode == 3 ? AI::ImportColorMode::ManualMatch : AI::ImportColorMode::NativeMatch;
        const auto trial = m_model_preview->import_color_mapping(true);
        AI::ModelColorTrial choice {trial.mapping_colors, trial.target_colors};
        if (trial.enabled && choice.valid()) request.color_trial = std::move(choice);
        request.face_color_overrides = m_model_preview->import_face_color_overrides(true);
        for (const auto& item : m_model_preview->import_subface_color_overrides(true))
            request.subface_color_overrides.push_back({item.face_id, item.path.depth, item.path.value, item.color});
        request.face_color_geometry_id = m_model_preview->geometry_id();
        try { BeautyWorkbenchControls::prepare_import(m_displayed_model_path, request); }
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
    if (m_shutdown || m_finishing_running) return false;
    set_finishing_workbench(false);
    if (m_workbench_results) m_workbench_results();
    return true;
}

bool ModelGenerationPanel::request_save_and_return()
{
    if (!post_generation_ui_state().can_edit) return false;
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
    else { m_save_and_return = false; request_return_overview(); request_check_workbench(); }
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
        request_return_overview();
        request_check_workbench();
    } else m_save_and_return = false;
}

} // namespace Slic3r::GUI
