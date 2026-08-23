#include "OrcaSmartSlicingWorkbench.hpp"

#include "OrcaOfficialSliceGateway.hpp"
#include "OrcaSmartSlicingAdapter.hpp"
#include "OrcaTrialSliceExecutor.hpp"
#include "OrcaWorkflowRuntimeStore.hpp"
#include "slic3r/AI/SmartSlicing/Application/CachingTrialSliceExecutor.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingPanel.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingPresenter.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <boost/filesystem/operations.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace Slic3r::GUI {
namespace {

Sidebar::AIWorkflowStatus to_sidebar_status(LegacyAIWorkflowStatus status)
{
    switch (status) {
    case LegacyAIWorkflowStatus::Running: return Sidebar::AIWorkflowStatus::Running;
    case LegacyAIWorkflowStatus::Success: return Sidebar::AIWorkflowStatus::Success;
    case LegacyAIWorkflowStatus::Warning: return Sidebar::AIWorkflowStatus::Warning;
    case LegacyAIWorkflowStatus::Failed: return Sidebar::AIWorkflowStatus::Failed;
    case LegacyAIWorkflowStatus::Waiting: return Sidebar::AIWorkflowStatus::Waiting;
    }
    return Sidebar::AIWorkflowStatus::Waiting;
}

wxString sidebar_summary(const SmartSlicingViewModel& view)
{
    if (view.is_stale)
        return _L("工程已变化，需要重新检查");
    if (view.summary_key == "preflight_complete")
        return _L("可打印性检查完成");
    if (view.summary_key == "preflight_complete_with_warnings")
        return _L("可打印性检查完成，仍有提示");
    if (view.summary_key == "printability_action_required")
        return _L("发现需要处理的问题");
    if (view.summary_key == "preflight_failed")
        return _L("可打印性检查失败");
    if (view.summary_key == "canceled")
        return _L("可打印性检查已取消");
    return _L("正在执行智能切片预检");
}

bool should_clear_trial_input(const std::string& summary_key)
{
    return summary_key == "official_slice_complete" || summary_key == "canceled" ||
           summary_key == "workspace_changed" || summary_key == "preflight_failed";
}

} // namespace

OrcaSmartSlicingWorkbench::OrcaSmartSlicingWorkbench(Plater& plater, StartSliceFn start_slice)
    : m_plater(plater)
    , m_workspace(std::make_unique<OrcaSmartSlicingAdapter>(&plater))
    , m_trial_executor(std::make_unique<OrcaTrialSliceExecutor>([this] {
        return m_workspace->capture_trial_slice_input();
    }))
    , m_cached_trial_executor(
          std::make_unique<AI::SmartSlicing::CachingTrialSliceExecutor>(*m_trial_executor))
    , m_official_gateway(std::make_unique<OrcaOfficialSliceGateway>(
          plater, [this] { return m_workspace->current_revision(); }, std::move(start_slice)))
    , m_runtime_store(std::make_unique<OrcaWorkflowRuntimeStore>(
          boost::filesystem::temp_directory_path() / "OrcaSlicer-smart-slicing-runtime-v1.json"))
    , m_coordinator(std::make_unique<AI::SmartSlicing::SmartSlicingCoordinator>(
          *m_workspace, *m_cached_trial_executor, *m_official_gateway))
{
    AI::SmartSlicing::WorkflowResourceBudget budget;
    m_trial_executor->set_resource_limits(
        budget.maximum_elapsed, budget.maximum_memory_bytes, budget.maximum_temporary_disk_bytes);
    m_coordinator->set_resource_budget(budget);
    m_coordinator->set_runtime_store(*m_runtime_store);

    m_presenter = std::make_unique<SmartSlicingPresenter>(
        *m_coordinator, [](std::function<void()> publish) {
            if (wxIsMainThread())
                publish();
            else
                wxGetApp().CallAfter(std::move(publish));
        });
    m_panel = std::make_unique<SmartSlicingPanel>(
        &plater, *m_coordinator,
        [this]() -> SmartSlicingPanel::CandidatePlanTask {
            const auto& snapshot = m_coordinator->snapshot();
            if (!snapshot.context)
                return {};
            m_trial_executor->prepare_session_input(m_workspace->capture_trial_slice_input());
            std::optional<OrcaCandidateProposalTask> prepared =
                m_workspace->prepare_candidate_proposals(*snapshot.context);
            if (!prepared)
                return {};
            auto task = std::make_shared<OrcaCandidateProposalTask>(std::move(*prepared));
            return [task = std::move(task)](SmartSlicingPanel::CancelPredicate canceled) {
                return task->execute(std::move(canceled));
            };
        },
        [this] { m_cached_trial_executor->cancel_trial_slice(); },
        [this](uint64_t object_id) { m_workspace->focus_object(object_id); });
    m_presenter->set_view_changed([this](const SmartSlicingViewModel& view) {
        m_panel->render(view);
        if (should_clear_trial_input(view.summary_key))
            m_trial_executor->clear_session_input();

        if (view.summary_key == "ready_to_start")
            return;
        Sidebar& sidebar = m_plater.sidebar();
        const wxString summary = sidebar_summary(view);
        sidebar.start_ai_workflow(summary);
        for (size_t index = 0; index < view.legacy_steps.size(); ++index)
            sidebar.update_ai_workflow_step(static_cast<Sidebar::AIWorkflowStep>(index),
                                            to_sidebar_status(view.legacy_steps[index]));
        if (view.can_start && !view.can_cancel)
            sidebar.finish_ai_workflow(false, summary);
    });
}

OrcaSmartSlicingWorkbench::~OrcaSmartSlicingWorkbench()
{
    m_cached_trial_executor->cancel_trial_slice();
    m_panel.reset();
    m_presenter.reset();
}

wxWindow* OrcaSmartSlicingWorkbench::panel() const
{
    return m_panel.get();
}

void OrcaSmartSlicingWorkbench::notify_slice_completed(bool success, std::string diagnostic)
{
    m_official_gateway->notify_slice_completed(success, std::move(diagnostic));
}

} // namespace Slic3r::GUI
