#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "ModelGenerationPresentation.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/AI/Model/BakedPortraitAppearance.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include <wx/gauge.h>
#include <wx/dcclient.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/weakref.h>
#include <boost/filesystem.hpp>
#include <fstream>
#include <boost/filesystem/fstream.hpp>

namespace Slic3r::GUI {
namespace {
wxString stage_name(PortraitStage stage) {
    switch (stage) {
    case PortraitStage::Preparing: return _L("准备与校验");
    case PortraitStage::Recognizing: return _L("多视角识别");
    case PortraitStage::Ownership: return _L("区域归属");
    case PortraitStage::Coloring: return _L("裁切与配色");
    case PortraitStage::Saving: return _L("保存候选");
    case PortraitStage::Preview: return _L("加载预览与选区");
    }
    return wxEmptyString;
}
wxString duration_text(double seconds) {
    const auto value = unsigned(std::max(0.0, seconds));
    return wxString::Format(_L("%u 分 %02u 秒"), value / 60, value % 60);
}
void save_portrait_timing(const PortraitOptimizationTask& task) {
    try {
        const auto key = task.history_key();
        if (key.empty()) return;
        const auto path = boost::filesystem::path(Slic3r::data_dir()) / "portrait-timing.json";
        nlohmann::json history = nlohmann::json::object();
        if (boost::filesystem::is_regular_file(path) && boost::filesystem::file_size(path) < 128*1024) {
            boost::filesystem::ifstream input(path); input >> history;
        }
        if (!history.is_object() || history.size() > 64) history = nlohmann::json::object();
        auto& samples = history[key];
        if (!samples.is_array()) samples = nlohmann::json::array();
        samples.push_back(task.durations());
        while (samples.size() > 5) samples.erase(samples.begin());
        const auto pending = boost::filesystem::path(path.string() + ".tmp");
        { boost::filesystem::ofstream output(pending); output << history.dump(); }
        boost::system::error_code error;
        boost::filesystem::rename(pending, path, error);
    } catch (...) { /* Timing history cannot invalidate a verified candidate. */ }
}
}

wxWindow* ModelGenerationPanel::build_portrait_optimization(wxWindow* parent)
{
    auto* wrapper = new wxPanel(parent);
    wrapper->SetBackgroundColour(parent->GetBackgroundColour());
    wrapper->SetSizer(new wxBoxSizer(wxVERTICAL));
    // Portrait recoloring and the ten-color/four-to-six-color fallback are
    // frozen.  The only user entry is Beauty Workbench -> Automatic Partition,
    // which requests semantic labels without changing native colors.
    m_model_preview->set_portrait_mode(false);
    m_model_preview->set_portrait_input_changed({});
    wrapper->Hide();
    return wrapper;
}

bool ModelGenerationPanel::request_semantic_mode(SemanticMode mode)
{
    if (mode == SemanticMode::Portrait) {
        if (m_model_preview) m_model_preview->set_portrait_mode(false);
        m_semantic_mode = SemanticMode::General;
        return false;
    }
    if (mode == SemanticMode::Portrait && !portrait_recognition_enabled()) return false;
    if (m_shutdown || !m_model_preview || (m_portrait_task && m_portrait_task->snapshot().running()) ||
        m_portrait_draft_before || !m_finishing_candidate.empty()) return false;
    if (mode == m_semantic_mode) return true;
    m_semantic_mode = mode;
    m_model_preview->set_portrait_mode(mode == SemanticMode::Portrait);
    if (mode == SemanticMode::Portrait) {
        m_portrait_entered = true;
        // Recognition is an explicit user action. Entering portrait mode only
        // prepares the lightweight controls; it must not start a local worker
        // or block generation/import on its own.
    }
    update_portrait_optimization(); publish_workbench_state(); return true;
}

void ModelGenerationPanel::reset_portrait_session()
{
    if (m_portrait_task) m_portrait_task->finish(PortraitOutcome::Cancelled);
    m_portrait_task.reset(); m_portrait_draft_before.reset(); m_portrait_preview_draft.reset();
    m_portrait_entered = m_portrait_preview_drawn = m_portrait_cancel_requested = false;
    m_auto_portrait_active = false;
    m_auto_portrait_geometry.clear();
    m_portrait_reoptimization_pending = false;
    m_portrait_reoptimization_count = 0;
    m_portrait_observed_automatic_count = 0;
    m_semantic_mode = SemanticMode::General;
    if (m_model_preview) { m_model_preview->set_portrait_mode(false); m_model_preview->after_next_preview_frame({}); }
}

void ModelGenerationPanel::queue_portrait_reoptimization()
{
    m_portrait_reoptimization_pending = false;
    m_portrait_reoptimization_count = 0;
    m_auto_portrait_active = false;
    return;
    if (m_shutdown || !m_model_preview) return;
    const auto current_colors = m_model_preview->color_trial_state();
    const bool count_changed = current_colors.automatic_count != m_portrait_observed_automatic_count;
    m_portrait_observed_automatic_count = current_colors.automatic_count;
    const bool automatic_source = current_colors.source == 0;
    const bool supported = automatic_source && m_model_preview->portrait_color_count_supported();
    const bool running = m_portrait_task && m_portrait_task->snapshot().running();
    const bool known_portrait = m_model_preview->semantic_person_detected() ||
        m_model_preview->has_modern_portrait_color_blocks() || m_auto_portrait_active;

    // A palette change invalidates an in-flight candidate. Preserve the new
    // slider state across the rollback performed by cancellation, then queue
    // one replacement request below.
    if (running) {
        cancel_portrait_optimization();
        m_model_preview->restore_color_trial_without_recognition(current_colors);
    }

    if (!supported) {
        m_portrait_reoptimization_pending = false;
        m_portrait_reoptimization_count = 0;
        m_auto_portrait_active = false;
        if (!m_model_preview->portrait_color_count_supported()) {
            // Returning to four-to-six colors on the same model must be able
            // to run a fresh detection after the ordinary palette is restored.
            m_auto_portrait_geometry.clear();
            m_semantic_mode = SemanticMode::General;
            m_model_preview->set_portrait_mode(false);
            if (m_portrait_card) m_portrait_card->Hide();
        }
        return;
    }
    // Track switches and manual color-block edits use the same invalidation
    // callback but do not change the automatic suggestion count. They must
    // not recursively launch a new optimizer task.
    if (!known_portrait || !count_changed) return;

    m_portrait_reoptimization_pending = true;
    m_portrait_reoptimization_count = current_colors.automatic_count;
    m_portrait_reoptimization_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(450);
    if (m_portrait_status)
        m_portrait_status->SetLabel(wxString::Format(
            _L("AI配色已调整至 %d 色，等待自动重新优化……"), m_portrait_reoptimization_count));
    BOOST_LOG_TRIVIAL(info) << "Portrait reoptimization queued: geometry="
        << m_model_preview->geometry_id() << ", colors=" << m_portrait_reoptimization_count;
}

void ModelGenerationPanel::cancel_portrait_optimization()
{
    m_portrait_cancel_requested = true;
    if (m_finishing_running) {
        stop_model_finishing();
    } else if (m_portrait_preview_draft) {
        const auto draft=std::move(m_portrait_preview_draft);
        const auto before=m_portrait_draft_before;
        if (m_portrait_task) m_portrait_task->finish(PortraitOutcome::Cancelled);
        m_model_preview->after_next_preview_frame({});
        if (!m_finishing_candidate.empty()) discard_model_finishing();
        restore_beauty_candidate(*draft);
        m_portrait_draft_before=before;
        preserve_portrait_draft();
        finish_portrait_optimization(PortraitOutcome::DraftOnly,_L("已取消预览准备，裁切草稿与原正式版本均已保留。"));
    } else if (m_portrait_draft_before) {
        restore_beauty_candidate(*m_portrait_draft_before); clear_portrait_draft(); m_portrait_draft_before.reset();
        if (m_beauty_controls) m_beauty_controls->set_dirty(false);
        if (m_finishing_status) m_finishing_status->SetLabel(_L("已放弃未保存草稿，处理前版本已恢复。"));
        finish_portrait_optimization(PortraitOutcome::Cancelled, _L("已放弃未保存草稿，处理前版本已恢复。"));
    } else if (m_model_preview && m_model_preview->semantic_processing()) {
        m_model_preview->cancel_semantic_request();
        m_model_preview->set_portrait_mode(true);
    } else if (m_portrait_task && m_portrait_task->snapshot().running()) {
        // Saving may have completed while selection preparation still owns the
        // final stage. Cancellation restores the same pre-task snapshot here.
        m_model_preview->after_next_preview_frame({});
        finish_portrait_optimization(PortraitOutcome::Cancelled, _L("优化已取消，处理前正式版本保留。"));
        if (!m_finishing_candidate.empty()) discard_model_finishing();
        m_model_preview->set_portrait_mode(true);
        if (m_portrait_task) m_portrait_task->evidence(m_finishing_candidate.empty()
            ? "已取消预览准备，处理前模型、选区和保护状态已恢复。"
            : "优化已取消，但处理前版本恢复失败；候选仍保留，请重试放弃。", true);
    }
    update_portrait_optimization();
}

void ModelGenerationPanel::finish_portrait_optimization(PortraitOutcome outcome, const wxString& reason)
{
    if (!m_portrait_task) return;
    m_portrait_task->finish(outcome, reason.ToUTF8().data());
    if (outcome == PortraitOutcome::Ready) save_portrait_timing(*m_portrait_task);
    BOOST_LOG_TRIVIAL(info) << "Portrait optimization completed request=" << m_portrait_task->snapshot().request_id
        << " outcome=" << int(m_portrait_task->snapshot().outcome)
        << " phase_seconds=" << nlohmann::json(m_portrait_task->durations()).dump();
    refresh_model_finishing();
    update_portrait_optimization(); publish_workbench_state();
}

void ModelGenerationPanel::portrait_preview_ready()
{
    if (!m_portrait_task || !m_portrait_task->snapshot().running()) return;
    m_model_preview->prepare_beauty_editor();
    m_portrait_task->report({PortraitStage::Preview, "等待预览绘制与选区准备", 0, 0});
    const auto task = m_portrait_task; const auto sequence = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_model_preview->after_next_preview_frame([weak, task, sequence] {
        if (weak && !weak->m_shutdown && weak->m_portrait_task == task && weak->m_sequence == sequence)
            weak->m_portrait_preview_drawn = true;
    });
}

void ModelGenerationPanel::update_portrait_optimization()
{
    m_portrait_reoptimization_pending = false;
    m_portrait_reoptimization_count = 0;
    m_auto_portrait_active = false;
    if (m_portrait_card) m_portrait_card->Hide();
    return;
    if (m_shutdown || !m_portrait_card || !m_model_preview) return;
    if (!m_model_preview->portrait_color_count_supported()) {
        m_portrait_reoptimization_pending = false;
        m_portrait_reoptimization_count = 0;
        m_auto_portrait_active = false;
        m_auto_portrait_geometry.clear();
        m_portrait_card->Hide();
        return;
    }
    if (m_portrait_reoptimization_pending &&
        std::chrono::steady_clock::now() >= m_portrait_reoptimization_deadline) {
        const auto trial = m_model_preview->color_trial_state();
        if (trial.source != 0 || trial.automatic_count != m_portrait_reoptimization_count) {
            m_portrait_reoptimization_pending = false;
            m_portrait_reoptimization_count = 0;
        } else if ((!m_portrait_task || !m_portrait_task->snapshot().running()) &&
            !m_portrait_draft_before && m_finishing_candidate.empty() &&
            !m_workbench_check_running && !m_workbench_import_running &&
            post_generation_ui_state().can_edit && m_model_preview->semantic_reoptimization_available()) {
            const int requested_count = m_portrait_reoptimization_count;
            m_portrait_reoptimization_pending = false;
            m_portrait_reoptimization_count = 0;
            m_auto_portrait_active = true;
            m_auto_portrait_geometry = m_model_preview->geometry_id();
            m_model_preview->activate_automatic_portrait_probe();
            request_semantic_mode(SemanticMode::Portrait);
            wxString reason;
            if (!request_portrait_optimization(reason)) {
                m_auto_portrait_active = false;
                m_semantic_mode = SemanticMode::General;
                m_model_preview->set_portrait_mode(false);
                if (m_portrait_status) m_portrait_status->SetLabel(reason);
                BOOST_LOG_TRIVIAL(warning) << "Queued portrait reoptimization failed to start: colors="
                    << requested_count << ", reason=" << reason.ToUTF8().data();
            } else {
                BOOST_LOG_TRIVIAL(info) << "Queued portrait reoptimization started: geometry="
                    << m_auto_portrait_geometry << ", colors=" << requested_count;
            }
            return;
        }
    }
    // Run one lightweight portrait probe after the ordinary color preview is
    // available. The semantic worker owns the actual person detection; a
    // negative result is handled by the completion callback and returns to
    // ordinary coloring without showing a blocking portrait workflow.
    if (m_semantic_mode == SemanticMode::General &&
        (!m_portrait_task || !m_portrait_task->snapshot().running()) &&
        !m_portrait_draft_before && m_finishing_candidate.empty() &&
        !m_workbench_check_running && !m_workbench_import_running &&
        (!m_model_preview->has_saved_portrait_result() || !m_model_preview->has_modern_portrait_color_blocks()) &&
        m_model_preview->semantic_reoptimization_available() &&
        // Historical assets become editable only after their preview and
        // metadata have been published. Do not consume the geometry key while
        // that transition is still in progress; doing so would suppress the
        // one automatic migration attempt for an old portrait result.
        post_generation_ui_state().can_edit) {
        const auto geometry = m_model_preview->geometry_id();
        if (!geometry.empty() && geometry != m_auto_portrait_geometry) {
            m_portrait_observed_automatic_count = m_model_preview->color_trial_state().automatic_count;
            m_auto_portrait_geometry = geometry;
            m_auto_portrait_active = true;
            m_model_preview->activate_automatic_portrait_probe();
            request_semantic_mode(SemanticMode::Portrait);
            wxString reason;
            if (!request_portrait_optimization(reason)) {
                m_auto_portrait_active = false;
                m_semantic_mode = SemanticMode::General;
                m_model_preview->set_portrait_mode(false);
                BOOST_LOG_TRIVIAL(info) << "Automatic portrait probe skipped: " << reason.ToUTF8().data();
            }
            return;
        }
    }
    // A saved V2 result must still reach the visibility refresh after the
    // hidden legacy selector returns to General; otherwise the old early
    // return leaves the re-optimization card permanently hidden.
    const bool portrait_ready = m_model_preview->semantic_person_detected() ||
        m_model_preview->has_modern_portrait_color_blocks();
    if (m_semantic_mode != SemanticMode::Portrait && !portrait_ready &&
        !m_portrait_card->IsShown() && (!m_portrait_task || !m_portrait_task->snapshot().running())) return;
    if (m_portrait_task && m_portrait_task->snapshot().running() && m_portrait_preview_draft &&
        m_model_preview->beauty_editor_failed()) {
        const auto draft=std::move(m_portrait_preview_draft);
        const auto before=m_portrait_draft_before;
        m_portrait_task->finish(PortraitOutcome::DraftOnly);
        m_save_and_return=false;
        m_model_preview->after_next_preview_frame({});
        if (!m_finishing_candidate.empty()) discard_model_finishing();
        restore_beauty_candidate(*draft);
        m_portrait_draft_before=before;
        preserve_portrait_draft();
        finish_portrait_optimization(PortraitOutcome::DraftOnly,_L("预览选区或裁切编辑数据恢复失败；原草稿已保留，请重试保存。"));
        return;
    }
    // A preview frame callback is an optional rendering acknowledgement.  On
    // some layouts SwapBuffers() can render successfully without reporting a
    // submitted frame, leaving m_portrait_preview_drawn false forever even
    // though the candidate is loaded and region preparation has completed.
    // The candidate is safe to finish once the editor is ready and no
    // selection/partition worker is active; keep the callback as the fast
    // path, with the editor state as the reliable fallback.
    const bool preview_ready_fallback = m_model_preview->region_editing_ready() &&
        !m_model_preview->selection_busy() && !m_model_preview->region_selection_preparing();
    if (m_portrait_task && m_portrait_task->snapshot().running() &&
        (m_portrait_preview_drawn || preview_ready_fallback) &&
        !m_finishing_candidate.empty() && !m_model_preview->selection_busy() &&
        !m_model_preview->region_selection_preparing() && !(m_beauty_controls && m_beauty_controls->partitioning())) {
        if (!m_portrait_preview_drawn)
            BOOST_LOG_TRIVIAL(info) << "Portrait preview frame acknowledgement missing; completing from prepared editor state";
        m_portrait_preview_drawn = true;
        m_portrait_preview_draft.reset();
        clear_portrait_draft();
        m_portrait_draft_before.reset();
        finish_portrait_optimization(PortraitOutcome::Ready);
        if (m_auto_portrait_active && !m_finishing_candidate.empty()) {
            // Automatic migration uses the same publication and undo/history
            // transaction as the explicit Accept action, but does not leave a
            // user-facing candidate confirmation step behind.
            m_auto_portrait_active = false;
            accept_model_finishing();
        }
        return;
    }
    const auto task = m_portrait_task ? m_portrait_task->snapshot() : PortraitOptimizationState{};
    // A completed V2 result is itself a validated portrait signal.  Keep the
    // card visible after accepting a candidate even though the hidden legacy
    // mode selector is returned to General.
    const bool has_saved_portrait = m_model_preview->has_modern_portrait_color_blocks();
    const bool visible = m_model_preview->semantic_person_detected() || has_saved_portrait;
    const bool changed = m_portrait_card->IsShown() != visible;
    m_portrait_card->Show(visible);
    const auto state = workbench_snapshot();
    if (m_workbench_project_colors) m_workbench_project_colors->Enable(state.can_edit_project_colors);
    m_portrait_mode_choice->Enable(!task.running() && !m_portrait_draft_before && m_finishing_candidate.empty());
    m_portrait_start->SetLabel(task.running() ? _L("优化中…") : m_portrait_draft_before ? _L("重试保存裁切草稿") : has_saved_portrait ? _L("重新优化") : _L("人像颜色块后处理"));
    m_portrait_start->Enable(!m_portrait_reoptimization_pending && !task.running() &&
        ((bool(m_portrait_draft_before) && state.actions.can_edit) || state.can_reoptimize_regions) && !m_workbench_check_running);
    m_portrait_cancel->Show(task.running() || bool(m_portrait_draft_before));
    m_portrait_cancel->SetLabel(task.running() ? _L("取消优化") : _L("放弃草稿"));
    m_portrait_cancel->Enable(!m_portrait_cancel_requested || bool(m_portrait_draft_before));
    auto* gauge = static_cast<wxGauge*>(m_portrait_gauge);
    gauge->Show(task.running() || (task.percent == 100 && !m_finishing_candidate.empty()));
    wxString text;
    if (m_portrait_reoptimization_pending && !task.running()) {
        text = wxString::Format(_L("AI配色已调整至 %d 色，等待自动重新优化……"),
            m_portrait_reoptimization_count);
        m_portrait_time->SetLabel(wxEmptyString);
    } else if (task.running()) {
        text = wxString::Format(_L("优化中 · 步骤 %d/6\n"), int(task.progress.stage) + 1) + stage_name(task.progress.stage);
        if (!task.progress.detail.empty()) text += "\n" + wxString::FromUTF8(task.progress.detail);
        if (task.progress.total) text += wxString::Format(_L(" · %llu/%llu"), (unsigned long long)task.progress.completed, (unsigned long long)task.progress.total);
        if (task.percent >= 0) { gauge->SetValue(task.percent); text += wxString::Format(_L(" · %d%%"), task.percent); }
        else gauge->Pulse();
        m_portrait_time->SetLabel(_L("已用 ") + duration_text(task.elapsed_seconds) + "\n" +
            (task.remaining_seconds < 0 ? _L("预计剩余：正在估算") : _L("预计剩余约 ") + duration_text(task.remaining_seconds)));
    } else {
        if (task.percent == 100) gauge->SetValue(100);
        switch (task.outcome) {
        case PortraitOutcome::Ready: text = m_finishing_candidate.empty() ? _L("当前显示已保存版本；可继续编辑或重新优化。") : _L("候选已就绪 · 100%\n请对比后接受或放弃。"); break;
        case PortraitOutcome::Partial: text = m_finishing_candidate.empty() ? _L("当前显示已保存版本；部分区域保留，请检查识别依据。") : _L("候选已就绪 · 100%\n部分区域保留，请检查识别依据。"); break;
        case PortraitOutcome::DraftOnly: text = _L("仅保留未保存草稿，尚不能导入。"); break;
        case PortraitOutcome::Failed: text = _L("优化失败，已保留处理前版本。"); break;
        case PortraitOutcome::Cancelled: text = _L("优化已取消，可重新开始。"); break;
        default: text = state.can_reoptimize_regions ? _L("本地识别并生成候选；保存结果不会自动重算。") : wxString::FromUTF8(state.reoptimization_reason); break;
        }
        if (!task.reason.empty()) text += "\n" + wxString::FromUTF8(task.reason);
        m_portrait_time->SetLabel(task.elapsed_seconds > 0 ? _L("已用 ") + duration_text(task.elapsed_seconds) : wxEmptyString);
    }
    bool label_changed = false;
    {
        const int width = std::max(FromDIP(160),m_portrait_card->GetClientSize().x);
        wxString line, wrapped;
        for (wxUniChar character : text) {
            if (character == '\n') { wrapped += line + "\n"; line.clear(); continue; }
            wxString next = line; next += character;
            if (!line.empty() && m_portrait_status->GetTextExtent(next).x > width) { wrapped += line + "\n"; line.clear(); }
            line += character;
        }
        wrapped += line;
        // Compare the displayed wrapped text, avoiding a clear/repaint cycle on
        // every timer tick when the raw sentence needs more than one line.
        if (m_portrait_status->GetLabel() != wrapped) {
            m_portrait_status->SetLabel(wrapped);
            wxClientDC dc(m_portrait_status); dc.SetFont(m_portrait_status->GetFont());
            m_portrait_status->SetMinSize(wxSize(1,dc.GetMultiLineTextExtent(wrapped).y+FromDIP(2)));
            m_portrait_card->Layout();
            label_changed = true;
        }
    }
    if (changed || label_changed || task.running()) {
        m_portrait_card->GetParent()->Layout();
        m_portrait_card->GetParent()->GetParent()->Layout();
        m_workbench_settings->GetParent()->Layout();
    }
    if (task.running()) publish_workbench_state();
}

void ModelGenerationPanel::prepare_portrait_import(AI::ModelImportRequest& request)
{
    if(m_model_preview && m_model_preview->portrait_color_blocks())return;
    const auto shapes=m_model_preview ? m_model_preview->portrait_shape_details() : nullptr;
    if (!shapes || !shapes->surface_partition || request.color_mode==AI::ImportColorMode::SingleColor) return;
    // A saved contour is carried by the verified GLB texture. Texture matching
    // subdivides the source, so old source face/leaf ordinals must not be applied
    // again afterwards. Refuse unsaved or changed appearance instead of dropping it.
    boost::filesystem::ifstream stream(ModelGenerationPresentation::library_metadata_path(m_displayed_model_job_id),std::ios::binary);
    const auto metadata=stream ? nlohmann::json::parse(stream,nullptr,false) : nlohmann::json();
    const auto hash=AI::model_artifact_sha256(boost::filesystem::path(request.artifact.local_path));
    const auto current=AI::baked_portrait_appearance(hash,m_model_preview->semantic_result_metadata(),
        m_model_preview->import_cell_color_overrides(),shapes->surface_partition->at("partition_sha256").get<std::string>());
    if (!metadata.is_object() || !metadata.contains("baked_portrait_appearance") ||
        !AI::same_baked_portrait_appearance(metadata.at("baked_portrait_appearance"),current)) {
        if (m_beauty_controls) m_beauty_controls->set_dirty(true);
        throw std::runtime_error("当前裁切外观尚未绑定到已保存 GLB，或保存后颜色已变化。请先保存并接受当前版本，再导入切片。");
    }
    request.face_color_overrides.clear();
    request.subface_color_overrides.clear();
    request.face_color_geometry_id.clear();
}

bool ModelGenerationPanel::request_portrait_optimization(wxString& reason)
{
    reason = _L("人像重配色已停用；请在 3D 美颜工作台点击“自动划区”，按需附加语义标签。");
    return false;
    const bool detected_v2_portrait = m_model_preview &&
        (m_model_preview->semantic_person_detected() || m_model_preview->has_modern_portrait_color_blocks());
    if (m_shutdown || ((m_semantic_mode != SemanticMode::Portrait) && !detected_v2_portrait) || m_portrait_draft_before ||
        m_workbench_check_running || (m_portrait_task && m_portrait_task->snapshot().running())) {
        reason = _L("请在人像模式下等待检查或当前任务结束；未保存草稿须先保存或放弃。");
        return false;
    }

    // A cached V2 result is already proof that the background detector found
    // a person. Re-running from the Beauty workbench must not require the
    // hidden legacy mode selector to be toggled first.
    if (detected_v2_portrait) m_semantic_mode = SemanticMode::Portrait;

        if (!m_model_preview || !post_generation_ui_state().can_edit || !m_finishing_candidate.empty()) {
            reason = _L("当前模型不可编辑，请先完成任务或接受、放弃候选版本。");
            return false;
        }
        if (!m_model_preview->semantic_reoptimization_available()) {
            reason = m_model_preview->semantic_reoptimization_reason();
            return false;
        }
        if (m_beauty_transactions && !m_beauty_transactions->begin(
                BeautyWorkbenchTransactionController::OperationKind::SemanticReoptimization)) {
            reason = _L("当前仍有 Beauty 处理正在进行，请先完成或取消。");
            m_finishing_status->SetLabel(reason);
            refresh_model_finishing();
            return false;
        }
        m_portrait_task = std::make_shared<PortraitOptimizationTask>(ModelGenerationPresentation::new_request_id(), m_displayed_model_path.string(), m_sequence);
        m_portrait_cancel_requested = false;
        m_portrait_preview_drawn = false;
        if (m_finishing_status) m_finishing_status->SetLabel(_L("正在优化人像区域，请查看左侧处理步骤。"));
        m_model_preview->set_portrait_progress(m_portrait_task);
        const auto task = m_portrait_task;
        const auto sequence = m_sequence;
        update_portrait_optimization();
        if (m_portrait_card) {
            m_portrait_card->GetParent()->Layout();
            m_portrait_card->Refresh();
            m_portrait_card->Update();
        }
        BOOST_LOG_TRIVIAL(info) << "Portrait optimization feedback request=" << task->snapshot().request_id
            << " milliseconds=" << task->snapshot().elapsed_seconds * 1000;
        m_beauty_reoptimization_before = std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
        const auto edit_revision = m_model_preview->leaf_edit_revision();
        const auto manual_colors = m_model_preview->face_color_overrides();
        const auto model_path = m_displayed_model_path;
        const bool unlocked = m_model_preview->portrait_shapes_unlocked();
        m_model_preview->set_semantic_completion_callback([this, task, sequence, edit_revision, manual_colors, model_path, unlocked](bool success) {
            if (m_shutdown || m_sequence != sequence || m_portrait_task != task) return;
            const bool automatic = m_auto_portrait_active;
            if (success && !m_model_preview->semantic_person_detected()) {
                success = false;
                if (automatic) {
                    if (auto before = std::move(m_beauty_reoptimization_before))
                        restore_beauty_candidate(*before);
                    finish_portrait_optimization(PortraitOutcome::Failed, _L("未检测到人像，已沿用普通配色。"));
                    m_auto_portrait_active = false;
                    m_semantic_mode = SemanticMode::General;
                    m_model_preview->set_portrait_mode(false);
                    refresh_model_finishing();
                    return;
                }
            }
            if (!success) {
                const auto error = m_model_preview->semantic_error();
                if (automatic) {
                    if (auto before = std::move(m_beauty_reoptimization_before))
                        restore_beauty_candidate(*before);
                    if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "automatic portrait probe unavailable");
                    finish_portrait_optimization(PortraitOutcome::Failed, error);
                    m_auto_portrait_active = false;
                    m_semantic_mode = SemanticMode::General;
                    m_model_preview->set_portrait_mode(false);
                    refresh_model_finishing();
                    return;
                }
                const bool edited = edit_revision != m_model_preview->leaf_edit_revision() ||
                    manual_colors != m_model_preview->face_color_overrides() ||
                    model_path != m_displayed_model_path || unlocked != m_model_preview->portrait_shapes_unlocked();
                if (auto before = std::move(m_beauty_reoptimization_before); before && !edited)
                    restore_beauty_candidate(*before);
                if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "semantic optimization failed");
                if (m_finishing_status) m_finishing_status->SetLabel(
                    _L("人像区域优化未完成，当前模型和选区保持不变：") + error);
                finish_portrait_optimization(m_portrait_cancel_requested ? PortraitOutcome::Cancelled : PortraitOutcome::Failed, error);
                refresh_model_finishing();
                return;
            }
            export_semantic_candidate();
        });
        if (!m_model_preview->request_semantic_reoptimization()) {
            if (!m_beauty_reoptimization_before) {
                reason = m_finishing_status->GetLabel();
                return false;
            }
            reason = _L("未重新识别人像区域：") + m_model_preview->semantic_reoptimization_reason();
            m_model_preview->set_semantic_completion_callback({});
            if (auto before = std::move(m_beauty_reoptimization_before)) restore_beauty_candidate(*before);
            if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "semantic request unavailable");
            m_finishing_status->SetLabel(reason);
            finish_portrait_optimization(PortraitOutcome::Failed, reason);
            refresh_model_finishing();
            return false;
        }
        refresh_model_finishing();
        return true;
}

} // namespace Slic3r::GUI
