#include "RedesignShell.hpp"
#include "RedesignMessageDialog.hpp"
#include "RedesignSlicingTools.hpp"
#include "OrcaPrinterAdapter.hpp"
#include "../AI/ModelGeneration/ModelGenerationFeatureHost.hpp"
#include "../AI/SmartSlicing/SmartSlicingFeatureHost.hpp"
#include "slic3r/AI/SmartSlicing/Domain/RiskConfirmationPolicy.hpp"
#include "../Plater.hpp"
#include "../PartPlate.hpp"
#include "../I18N.hpp"
#include <wx/weakref.h>
#include <boost/filesystem.hpp>

namespace Slic3r::GUI {
namespace PP = PortraitPrint;
namespace {
std::string revision_key(const AI::SmartSlicing::WorkspaceRevision& revision) {
    if (!revision.valid()) return {};
    return std::to_string(revision.model_revision) + ":" + std::to_string(revision.config_revision) + ":" +
        std::to_string(revision.plate_revision) + ":" + revision.fingerprint;
}
struct AdvanceGuard { bool& flag; explicit AdvanceGuard(bool& f) : flag(f) { flag = true; } ~AdvanceGuard() { flag = false; } };
}

bool RedesignShell::request_portrait_print()
{
    if (!m_model_generation_host || !m_plater || m_submit_in_progress) return false;
    m_submit_in_progress = true;
    const auto submission = m_model_generation_host->request_portrait_print_from_model([this] {
        // The feature has already published a verified project recovery copy.
        return m_plater->new_project(true, true, {}, false) != wxID_CANCEL && m_plater->model().objects.empty();
    });
    m_submit_in_progress = false;
    apply_model_generation_state(m_model_generation_host->snapshot());
    return PP::accepted(submission);
}

void RedesignShell::queue_portrait_print()
{
    if (!m_model_generation_host || m_portrait_print_queued ||
        !m_model_generation_host->snapshot().portrait_print.running()) return;
    m_portrait_print_queued = true;
    wxWeakRef<RedesignShell> weak(this);
    CallAfter([weak] {
        if (!weak) return;
        weak->m_portrait_print_queued = false;
        weak->run_portrait_print();
    });
}

void RedesignShell::run_portrait_print()
{
    if (!m_model_generation_host || !m_plater || m_portrait_print_advancing) return;
    const auto flow = m_model_generation_host->snapshot().portrait_print;
    if (!flow.running() || !flow.from_workbench()) return;
    AdvanceGuard guard(m_portrait_print_advancing);
    if (m_portrait_active_id != flow.id) {
        m_portrait_active_id = flow.id;
        m_portrait_import_requested = m_portrait_analysis_requested = false;
        m_portrait_imported_object_id.clear();
    }
    const auto pause = [this](const std::string& why) { m_model_generation_host->pause_portrait_print(why); };
    if (flow.stage == PP::Stage::Check || flow.stage == PP::Stage::Partition || flow.stage == PP::Stage::Color ||
        flow.stage == PP::Stage::SaveColor || flow.stage == PP::Stage::Base || flow.stage == PP::Stage::Filaments) {
        if (m_model_view != ModelView::Workbench && !m_import_in_progress) open_model_workbench();
        return;
    }
    if (flow.stage == PP::Stage::Import) {
        if (m_import_in_progress) return;
        if (!m_plater->model().objects.empty()) {
            if (!flow.native_repair_completed) { pause("原生修复结果身份缺失，未启动切片。"); return; }
            if (!m_portrait_import_requested || m_plater->model().objects.size() != 1 ||
                m_model_view != ModelView::Slicing ||
                m_portrait_imported_object_id != std::to_string(m_plater->model().objects.front()->id().id)) {
                pause("导入目标不再是本次单个人像工程，请核对后继续。"); return;
            }
            if (!m_slicing_host) { pause("智能切片宿主不可用。"); return; }
            const auto revision = revision_key(m_slicing_host->workbench_snapshot().session.workspace_revision);
            if (revision.empty()) return;
            if (!flow.project_revision.empty() && revision != flow.project_revision) {
                pause("已导入人像的工程修订已变化，请保存后重新校验。"); return;
            }
            m_model_generation_host->advance_portrait_print(flow.id, PP::Stage::Import, PP::Stage::Recommend, revision);
            return;
        }
        if (std::to_string(m_plater->model().id().id) != flow.workspace_id) { pause("空工程身份已变化，请手动继续重新校验。"); return; }
        if (m_portrait_import_requested) return;
        if (!m_model_generation_host->workbench_snapshot().deferred_native_repair &&
            !m_model_generation_host->workbench_snapshot().can_import_for_slicing &&
            !m_model_generation_host->workbench_snapshot().force_import_acknowledged) return;
        m_portrait_import_requested = true;
        if (!m_model_generation_host->request_workbench_color_matching()) {
            m_portrait_import_requested = false; pause("已配色模型未能导入，请在工作台处理提示后继续。");
        }
        return;
    }
    if (flow.stage != PP::Stage::Recommend && flow.stage != PP::Stage::Slice) return;
    if (!m_slicing_host || !m_slice_tools || m_slice_tools->busy() || m_import_in_progress) return;
    const auto state = m_slicing_host->workbench_snapshot();
    const auto current_revision = revision_key(state.session.workspace_revision);
    if (m_plater->model().objects.size() != 1 || current_revision.empty() ||
        m_portrait_imported_object_id != std::to_string(m_plater->model().objects.front()->id().id)) {
        pause("工程不再只包含本次人像，已停止自动切片。"); return;
    }
    if (flow.stage == PP::Stage::Recommend) {
        if (current_revision != flow.project_revision) { pause("导入后的模型、耗材或工程修订已变化，请重新校验。"); return; }
        if (state.recommendation_source != SlicingRecommendationSource::Local) {
            m_slicing_host->set_recommendation_source(SlicingRecommendationSource::Local); return;
        }
        if (state.analyzing || state.lightweight_analyzing) return;
        if (!state.lightweight_analysis || !state.lightweight_analysis->completed || state.lightweight_stale) {
            if (m_portrait_analysis_requested) { pause("本地综合最优建议未完成，请检查切片诊断后继续。"); return; }
            m_portrait_analysis_requested = true;
            if (!m_slicing_host->analyze_workbench()) pause("无法启动本地综合最优分析。");
            return;
        }
        using namespace AI::SmartSlicing;
        if (!state.has_selected_goal || state.selected_goal != RecommendationGoal::Balanced) {
            if (!m_slicing_host->select_goal(RecommendationGoal::Balanced)) pause("综合最优方案当前不可选。");
            return;
        }
        if (!state.can_start) {
            if (state.exact_estimate.status != ExactEstimateStatus::Running && !state.native_blocked_reason.empty()) pause(state.native_blocked_reason);
            return;
        }
        std::vector<RiskConfirmationKind> confirmations;
        const auto& goal = state.session.recommendation.goal_result(RecommendationGoal::Balanced);
        if (goal.risk_confirmation_contract) confirmations = goal.risk_confirmation_contract->required_confirmations();
        for (auto risk : confirmations)
            m_model_generation_host->add_portrait_notice(flow.id, risk == RiskConfirmationKind::ProtectedRegionSupportContact ?
                "切片提示：保护区域将接触支撑，拆支撑时可能影响表面。" : "切片提示：保护区域将出现接缝。");
        // The unified portrait authorization includes the native risk contract.
        // Passing its exact required set retains the slicing gate without a modal.
        const auto verified = m_model_generation_host->snapshot().portrait_print;
        if (!verified.current(flow.id, PP::Stage::Recommend) ||
            revision_key(m_slicing_host->workbench_snapshot().session.workspace_revision) != current_revision) return;
        if (!m_model_generation_host->advance_portrait_print(flow.id, PP::Stage::Recommend, PP::Stage::Slice, current_revision)) return;
        const auto result = m_slicing_host->start_workbench_slice(state, confirmations);
        if (result.phase != OfficialSlicePhase::Slicing && result.phase != OfficialSlicePhase::Completed) {
            pause("正式切片未启动：" + result.diagnostic_code); return;
        }
        // Applying the reviewed plan may change config revision. Bind the
        // completion to the actual official slice input, including that apply.
        const auto applied = revision_key(m_slicing_host->workbench_snapshot().session.workspace_revision);
        m_model_generation_host->advance_portrait_print(flow.id, PP::Stage::Slice, PP::Stage::Slice, applied);
        queue_portrait_print(); return;
    }
    if (current_revision != flow.slice_revision) { pause("正式切片的工程修订已变化，旧结果不会自动进入打印准备。"); return; }
    using AI::SmartSlicing::OfficialSlicePhase;
    if (state.official.phase == OfficialSlicePhase::Failed || state.official.phase == OfficialSlicePhase::Rejected) {
        pause("正式切片失败：" + state.official.diagnostic_code); return;
    }
    auto* plate = m_plater->get_partplate_list().get_curr_plate();
    if (state.official.phase != OfficialSlicePhase::Completed || m_plater->is_background_process_slicing() ||
        !plate || !plate->is_slice_result_valid() || !OrcaPrinterAdapter(m_plater).snapshot().gcode_ready) return;
    const auto gcode = plate->get_gcode_filename();
    if (gcode.empty() || !boost::filesystem::is_regular_file(boost::filesystem::path(wxString::FromUTF8(gcode).ToStdWstring())) ||
        boost::filesystem::file_size(boost::filesystem::path(wxString::FromUTF8(gcode).ToStdWstring())) == 0) {
        pause("正式切片完成但 G-code 文件不可用，未进入打印准备。"); return;
    }
    open_print_preparation();
    if (m_active_page == Page::Print)
        m_model_generation_host->advance_portrait_print(flow.id, PP::Stage::Slice, PP::Stage::PrintReady, current_revision, gcode);
    else pause("正式切片已保存，但打印准备页尚未打开，请核对提示后继续。");
}
} // namespace Slic3r::GUI
