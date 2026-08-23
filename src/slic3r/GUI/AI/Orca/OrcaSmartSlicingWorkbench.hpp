#pragma once

#include <functional>
#include <memory>
#include <string>

class wxWindow;

namespace Slic3r::AI::SmartSlicing {
class CachingTrialSliceExecutor;
class SmartSlicingCoordinator;
}

namespace Slic3r::GUI {

class OrcaOfficialSliceGateway;
class OrcaSmartSlicingAdapter;
class OrcaTrialSliceExecutor;
class OrcaWorkflowRuntimeStore;
class Plater;
class SmartSlicingPanel;
class SmartSlicingPresenter;

class OrcaSmartSlicingWorkbench final
{
public:
    using StartSliceFn = std::function<bool()>;

    OrcaSmartSlicingWorkbench(Plater& plater, StartSliceFn start_slice);
    ~OrcaSmartSlicingWorkbench();

    OrcaSmartSlicingWorkbench(const OrcaSmartSlicingWorkbench&) = delete;
    OrcaSmartSlicingWorkbench& operator=(const OrcaSmartSlicingWorkbench&) = delete;

    wxWindow* panel() const;
    void notify_slice_completed(bool success, std::string diagnostic = {});

private:
    Plater& m_plater;
    std::unique_ptr<OrcaSmartSlicingAdapter> m_workspace;
    std::unique_ptr<OrcaTrialSliceExecutor> m_trial_executor;
    std::unique_ptr<AI::SmartSlicing::CachingTrialSliceExecutor> m_cached_trial_executor;
    std::unique_ptr<OrcaOfficialSliceGateway> m_official_gateway;
    std::unique_ptr<OrcaWorkflowRuntimeStore> m_runtime_store;
    std::unique_ptr<AI::SmartSlicing::SmartSlicingCoordinator> m_coordinator;
    std::unique_ptr<SmartSlicingPresenter> m_presenter;
    std::unique_ptr<SmartSlicingPanel> m_panel;
};

} // namespace Slic3r::GUI
