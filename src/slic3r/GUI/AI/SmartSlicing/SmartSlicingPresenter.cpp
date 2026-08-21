#include "SmartSlicingPresenter.hpp"

#include <utility>

namespace Slic3r::GUI {

SmartSlicingPresenter::SmartSlicingPresenter(AI::SmartSlicing::SmartSlicingCoordinator& coordinator, DispatchFn dispatch)
    : m_coordinator(coordinator), m_dispatch(std::move(dispatch)), m_alive(std::make_shared<std::atomic<bool>>(true))
{
    m_coordinator.set_observer([this](const AI::SmartSlicing::WorkflowSnapshot& snapshot) {
        SmartSlicingViewModel view = SmartSlicingViewModel::from_snapshot(snapshot);
        const std::weak_ptr<std::atomic<bool>> alive = m_alive;
        auto publish = [this, alive, view = std::move(view)]() mutable {
            const std::shared_ptr<std::atomic<bool>> guard = alive.lock();
            if (!guard || !guard->load(std::memory_order_acquire))
                return;
            m_view_model = std::move(view);
            if (m_view_changed)
                m_view_changed(m_view_model);
        };
        if (m_dispatch)
            m_dispatch(std::move(publish));
        else
            publish();
    });
}

SmartSlicingPresenter::~SmartSlicingPresenter()
{
    m_alive->store(false, std::memory_order_release);
    m_coordinator.set_observer({});
}

void SmartSlicingPresenter::set_view_changed(ViewChangedFn view_changed)
{
    m_view_changed = std::move(view_changed);
    if (m_view_changed)
        m_view_changed(m_view_model);
}

} // namespace Slic3r::GUI
