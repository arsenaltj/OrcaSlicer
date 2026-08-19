#include "SmartSlicingPresenter.hpp"

#include <utility>

namespace Slic3r::GUI {

SmartSlicingPresenter::SmartSlicingPresenter(AI::SmartSlicing::SmartSlicingCoordinator& coordinator) : m_coordinator(coordinator)
{
    m_coordinator.set_observer([this](const AI::SmartSlicing::WorkflowSnapshot& snapshot) {
        m_view_model = SmartSlicingViewModel::from_snapshot(snapshot);
        if (m_view_changed)
            m_view_changed(m_view_model);
    });
}

SmartSlicingPresenter::~SmartSlicingPresenter() { m_coordinator.set_observer({}); }

void SmartSlicingPresenter::set_view_changed(ViewChangedFn view_changed)
{
    m_view_changed = std::move(view_changed);
    if (m_view_changed)
        m_view_changed(m_view_model);
}

} // namespace Slic3r::GUI
