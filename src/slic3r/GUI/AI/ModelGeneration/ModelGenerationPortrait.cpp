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
    if (m_shutdown || !m_model_preview || (m_portrait_task && m_portrait_task->snapshot().running()) ||
        m_portrait_draft_before || !m_finishing_candidate.empty()) return false;
    if (mode == m_semantic_mode) return true;
    m_semantic_mode = mode;
    m_model_preview->set_portrait_mode(mode == SemanticMode::Portrait);
    publish_workbench_state(); return true;
}

void ModelGenerationPanel::reset_portrait_session()
{
    cancel_native_workbench_partition();
    if (m_portrait_task) m_portrait_task->finish(PortraitOutcome::Cancelled);
    m_portrait_task.reset(); m_portrait_draft_before.reset(); m_portrait_preview_draft.reset();
    m_semantic_mode = SemanticMode::General;
    if (m_model_preview) { m_model_preview->set_portrait_mode(false); m_model_preview->after_next_preview_frame({}); }
}

void ModelGenerationPanel::restore_portrait_preview_draft_async(const wxString& reason)
{
    if (!m_portrait_preview_draft || m_beauty_restore_running || m_shutdown) return;
    const auto draft = m_portrait_preview_draft;
    const auto before = m_portrait_draft_before;
    if (m_portrait_task) m_portrait_task->finish(PortraitOutcome::DraftOnly);
    m_model_preview->after_next_preview_frame({});
    wxWeakRef<ModelGenerationPanel> weak(this);
    auto restore = [weak, draft, before, reason](bool discarded) {
        if (!weak || weak->m_shutdown) return;
        if (!discarded) return;
        weak->restore_beauty_candidate_async(*draft, [weak, draft, before, reason](bool restored) {
            if (!weak || weak->m_shutdown) return;
            if (restored) {
                if (weak->m_portrait_preview_draft == draft) weak->m_portrait_preview_draft.reset();
                weak->m_portrait_draft_before = before;
                weak->preserve_portrait_draft();
                weak->finish_portrait_optimization(PortraitOutcome::DraftOnly, reason);
            }
        });
    };
    if (!m_finishing_candidate.empty()) discard_model_finishing_async(std::move(restore));
    else restore(true);
}

void ModelGenerationPanel::cancel_portrait_optimization()
{
    if (m_beauty_restore_running) return;
    if (m_finishing_running) {
        // The existing completion path restores drafts and protects published
        // results. Keep its busy gate until it returns; never wait here.
        if (m_finishing_canceled) m_finishing_canceled->store(true);
        if (m_beauty_transactions) m_beauty_transactions->request_cancel();
    } else if (m_portrait_preview_draft) {
        restore_portrait_preview_draft_async(_L("已取消预览准备，裁切草稿与原正式版本均已保留。"));
    } else if (m_portrait_draft_before) {
        const auto before = m_portrait_draft_before;
        wxWeakRef<ModelGenerationPanel> weak(this);
        restore_beauty_candidate_async(*before, [weak, before](bool restored) {
            if (!weak || weak->m_shutdown) return;
            if (restored && weak->m_portrait_draft_before == before) {
                weak->clear_portrait_draft(); weak->m_portrait_draft_before.reset();
                if (weak->m_beauty_controls) weak->m_beauty_controls->set_dirty(false);
                if (weak->m_finishing_status) weak->m_finishing_status->SetLabel(_L("已放弃未保存草稿，处理前版本已恢复。"));
                weak->finish_portrait_optimization(PortraitOutcome::Cancelled, _L("已放弃未保存草稿，处理前版本已恢复。"));
            }
        });
    } else if (m_model_preview && m_model_preview->semantic_processing()) {
        m_model_preview->cancel_semantic_request();
        m_model_preview->set_portrait_mode(true);
    } else if (m_portrait_task && m_portrait_task->snapshot().running()) {
        // Saving may have completed while selection preparation still owns the
        // final stage. Cancellation restores the same pre-task snapshot here.
        m_model_preview->after_next_preview_frame({});
        finish_portrait_optimization(PortraitOutcome::Cancelled, _L("优化已取消，处理前正式版本保留。"));
        wxWeakRef<ModelGenerationPanel> weak(this);
        auto finished = [weak](bool) {
            if (!weak || weak->m_shutdown) return;
            weak->m_model_preview->set_portrait_mode(true);
            if (weak->m_portrait_task) weak->m_portrait_task->evidence(weak->m_finishing_candidate.empty()
                ? "已取消预览准备，处理前模型、选区和保护状态已恢复。"
                : "优化已取消，但处理前版本恢复失败；候选仍保留，请重试放弃。", true);
        };
        if (!m_finishing_candidate.empty()) discard_model_finishing_async(std::move(finished));
        else finished(true);
    }
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
    publish_workbench_state();
}

void ModelGenerationPanel::portrait_preview_ready()
{
    if (!m_portrait_task || !m_portrait_task->snapshot().running()) return;
    m_model_preview->prepare_beauty_editor();
    m_portrait_task->report({PortraitStage::Preview, "等待预览绘制与选区准备", 0, 0});
}

std::function<void(AI::ModelImportRequest&)> ModelGenerationPanel::capture_portrait_import_preparation(
    const AI::ModelImportRequest& request) const
{
    Presentation::assert_gui_thread();
    if(m_model_preview && m_model_preview->portrait_color_blocks())return {};
    const auto shapes=m_model_preview ? m_model_preview->portrait_shape_details() : nullptr;
    if (!shapes || !shapes->surface_partition || request.color_mode==AI::ImportColorMode::SingleColor) return {};
    // A saved contour is carried by the verified GLB texture. Texture matching
    // subdivides the source, so old source face/leaf ordinals must not be applied
    // again afterwards. Refuse unsaved or changed appearance instead of dropping it.
    const auto current=AI::baked_portrait_appearance(m_model_preview->model_sha256(),m_model_preview->semantic_result_metadata(),
        m_model_preview->import_cell_color_overrides(),shapes->surface_partition->at("partition_sha256").get<std::string>());
    return [current, record = ModelGenerationPresentation::library_metadata_path(m_displayed_model_job_id)](
            AI::ModelImportRequest& prepared) {
        boost::filesystem::ifstream stream(record,std::ios::binary);
        const auto metadata=stream ? nlohmann::json::parse(stream,nullptr,false) : nlohmann::json();
        if (!metadata.is_object() || !metadata.contains("baked_portrait_appearance") ||
            !AI::same_baked_portrait_appearance(metadata.at("baked_portrait_appearance"),current))
            throw std::runtime_error("当前裁切外观尚未绑定到已保存 GLB，或保存后颜色已变化。请先保存并接受当前版本，再导入切片。");
        prepared.face_color_overrides.clear();
        prepared.subface_color_overrides.clear();
        prepared.face_color_geometry_id.clear();
    };
}

bool ModelGenerationPanel::request_portrait_optimization(wxString& reason)
{
    reason = _L("人像重配色已停用；请在 3D 美颜工作台点击“自动划区”，按需附加语义标签。");
    return false;
}

} // namespace Slic3r::GUI
