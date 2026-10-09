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
    auto* layout = new wxBoxSizer(wxVERTICAL);
    m_portrait_mode_choice = m_model_preview->build_workbench_mode(wrapper,
        [this](bool portrait) { request_semantic_mode(portrait ? SemanticMode::Portrait : SemanticMode::General); },
        [this] { cancel_portrait_optimization(); });
    layout->Add(m_portrait_mode_choice, 0, wxEXPAND);
    m_model_preview->set_portrait_mode(false);
    m_model_preview->set_portrait_input_changed([this] {
        if (m_portrait_task && m_portrait_task->snapshot().running()) {
            const auto current_colors = m_model_preview->color_trial_state();
            cancel_portrait_optimization();
            m_model_preview->restore_color_trial_without_recognition(current_colors);
            if (m_portrait_status) m_portrait_status->SetLabel(_L("配色输入已变化，优化已取消，请重新优化。"));
        }
    });
    m_portrait_card = new wxPanel(wrapper);
    m_portrait_card->SetBackgroundColour(parent->GetBackgroundColour());
    auto* card = new wxBoxSizer(wxVERTICAL);
    m_portrait_start = workbench_button(m_portrait_card, _L("重新优化人像区域"));
    m_portrait_start->SetMinSize(FromDIP(wxSize(-1, 48)));
    m_portrait_start->SetBackgroundColor(StateColor(
        std::pair<wxColour,int>(wxColour(75,75,80), StateColor::Disabled),
        std::pair<wxColour,int>(wxColour(255,212,90), StateColor::Hovered),
        std::pair<wxColour,int>(wxColour(255,194,39), StateColor::Normal)));
    m_portrait_start->SetTextColor(StateColor(wxColour(22,22,25)));
    m_portrait_start->SetFont(wxGetApp().bold_font());
    card->Add(m_portrait_start, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_portrait_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_portrait_draft_before) {
            if (!post_generation_ui_state().can_edit) return;
            if (m_beauty_transactions && !m_beauty_transactions->begin(
                BeautyWorkbenchTransactionController::OperationKind::SemanticReoptimization)) return;
            m_portrait_task = std::make_shared<PortraitOptimizationTask>(ModelGenerationPresentation::new_request_id(), m_displayed_model_path.string(), m_sequence);
            m_portrait_task->evidence("恢复同源草稿：本次仅重建裁切网格并保存，不重新识别。原有颜色与保护边界保留；耳根、发际、颈侧及眉周的未确认残色仍需检查。",true);
            m_portrait_cancel_requested = false; m_portrait_preview_drawn = false;
            m_beauty_reoptimization_before = m_portrait_draft_before;
            export_semantic_candidate();
        } else {
            wxString reason;
            if (!request_portrait_optimization(reason)) m_portrait_status->SetLabel(reason);
        }
    });
    m_portrait_status = new wxStaticText(m_portrait_card, wxID_ANY, _L("本地人像识别，结果先保存为候选。"));
    m_portrait_status->SetForegroundColour(wxColour(230,230,234));
    m_portrait_status->SetMinSize(FromDIP(wxSize(1,-1)));
    card->Add(m_portrait_status, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    m_portrait_gauge = new wxGauge(m_portrait_card, wxID_ANY, 100);
    card->Add(m_portrait_gauge, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    m_portrait_time = new wxStaticText(m_portrait_card, wxID_ANY, wxEmptyString);
    m_portrait_time->SetForegroundColour(wxColour(180,180,188));
    card->Add(m_portrait_time, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto* commands = new wxBoxSizer(wxHORIZONTAL);
    m_portrait_cancel = workbench_button(m_portrait_card, _L("取消优化"));
    commands->Add(m_portrait_cancel, 1, wxRIGHT, FromDIP(4));
    m_portrait_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancel_portrait_optimization(); });
    auto* details = workbench_button(m_portrait_card, _L("识别依据"));
    commands->Add(details, 1);
    details->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        auto state = m_portrait_task ? m_portrait_task->snapshot() : PortraitOptimizationState{};
        wxString message = wxString::FromUTF8(state.evidence);
        if (message.empty()) message = _L("保存结果会按原始模型和几何身份恢复。重新优化时只复用身份匹配的证据；缺少证据的区域保留原外观。");
        message += _L("\n\n保护边界未变化不代表识别准确；请结合原色对照检查五官位置。");
        if (!state.reason.empty()) message += "\n\n" + wxString::FromUTF8(state.reason);
        if (!state.request_id.empty()) message += "\n\n" + _L("诊断 ID：") + wxString::FromUTF8(state.request_id);
        wxMessageBox(message, _L("识别依据与未处理原因"), wxOK | wxICON_INFORMATION, wxGetTopLevelParent(this));
    });
    card->Add(commands, 0, wxEXPAND);
    m_portrait_card->SetSizer(card); layout->Add(m_portrait_card, 0, wxEXPAND);
    wrapper->SetSizer(layout); m_portrait_card->Hide();
    m_portrait_timer.SetOwner(this, wxWindow::NewControlId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { update_portrait_optimization(); }, m_portrait_timer.GetId());
    m_portrait_timer.Start(250);
    return wrapper;
}

bool ModelGenerationPanel::request_semantic_mode(SemanticMode mode)
{
    if (m_shutdown || !m_model_preview || (m_portrait_task && m_portrait_task->snapshot().running()) ||
        m_portrait_draft_before || !m_finishing_candidate.empty()) return false;
    if (mode == m_semantic_mode) return true;
    const bool first = !m_portrait_entered;
    m_semantic_mode = mode;
    m_model_preview->set_portrait_mode(mode == SemanticMode::Portrait);
    if (mode == SemanticMode::Portrait) {
        m_portrait_entered = true;
        const bool start = portrait_should_start_on_enter(first, m_model_preview->has_saved_portrait_result(),
            workbench_snapshot().can_reoptimize_regions && !m_workbench_check_running);
        if (start) {
            wxString reason;
            if (!request_portrait_optimization(reason)) m_portrait_status->SetLabel(reason);
        }
    }
    update_portrait_optimization(); publish_workbench_state(); return true;
}

void ModelGenerationPanel::reset_portrait_session()
{
    if (m_portrait_task) m_portrait_task->finish(PortraitOutcome::Cancelled);
    m_portrait_task.reset(); m_portrait_draft_before.reset(); m_portrait_preview_draft.reset();
    m_portrait_entered = m_portrait_preview_drawn = m_portrait_cancel_requested = false;
    m_semantic_mode = SemanticMode::General;
    if (m_model_preview) { m_model_preview->set_portrait_mode(false); m_model_preview->after_next_preview_frame({}); }
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
    if (m_shutdown || !m_portrait_card || !m_model_preview) return;
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
    if (m_portrait_task && m_portrait_task->snapshot().running() && m_portrait_preview_drawn &&
        !m_finishing_candidate.empty() && !m_model_preview->selection_busy() &&
        !m_model_preview->region_selection_preparing() && !(m_beauty_controls && m_beauty_controls->partitioning())) {
        m_portrait_preview_draft.reset();
        clear_portrait_draft();
        m_portrait_draft_before.reset();
        finish_portrait_optimization(PortraitOutcome::Ready); return;
    }
    const auto task = m_portrait_task ? m_portrait_task->snapshot() : PortraitOptimizationState{};
    const bool visible = m_semantic_mode == SemanticMode::Portrait;
    const bool changed = m_portrait_card->IsShown() != visible;
    m_portrait_card->Show(visible);
    const auto state = workbench_snapshot();
    if (m_workbench_project_colors) m_workbench_project_colors->Enable(state.can_edit_project_colors);
    m_portrait_mode_choice->Enable(!task.running() && !m_portrait_draft_before && m_finishing_candidate.empty());
    m_portrait_start->SetLabel(task.running() ? _L("优化中…") : m_portrait_draft_before ? _L("重试保存裁切草稿") : _L("重新优化人像区域"));
    m_portrait_start->Enable(!task.running() && ((bool(m_portrait_draft_before) && state.actions.can_edit) || state.can_reoptimize_regions) && !m_workbench_check_running);
    m_portrait_cancel->Show(task.running() || bool(m_portrait_draft_before));
    m_portrait_cancel->SetLabel(task.running() ? _L("取消优化") : _L("放弃草稿"));
    m_portrait_cancel->Enable(!m_portrait_cancel_requested || bool(m_portrait_draft_before));
    auto* gauge = static_cast<wxGauge*>(m_portrait_gauge);
    gauge->Show(task.running() || (task.percent == 100 && !m_finishing_candidate.empty()));
    wxString text;
    if (task.running()) {
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
    if (m_shutdown || m_semantic_mode != SemanticMode::Portrait || m_portrait_draft_before ||
        m_workbench_check_running || (m_portrait_task && m_portrait_task->snapshot().running())) {
        reason = _L("请在人像模式下等待检查或当前任务结束；未保存草稿须先保存或放弃。");
        return false;
    }

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
            if (!success) {
                const auto error = m_model_preview->semantic_error();
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
