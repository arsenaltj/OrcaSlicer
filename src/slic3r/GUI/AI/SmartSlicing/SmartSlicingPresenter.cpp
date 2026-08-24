#include "SmartSlicingPresenter.hpp"

#include <utility>

namespace Slic3r::GUI {

SmartSlicingPresenter::SmartSlicingPresenter(AI::SmartSlicing::SmartSlicingCoordinator& coordinator, DispatchFn dispatch)
    : m_coordinator(coordinator)
    , m_dispatch(std::move(dispatch))
    , m_alive(std::make_shared<std::atomic<bool>>(true))
    , m_publish_sequence(std::make_shared<std::atomic<uint64_t>>(0))
{
    m_coordinator.set_observer([this](const AI::SmartSlicing::WorkflowSnapshot& snapshot) {
        SmartSlicingViewModel view = SmartSlicingViewModel::from_snapshot(snapshot);
        const std::weak_ptr<std::atomic<bool>> alive = m_alive;
        const std::shared_ptr<std::atomic<uint64_t>> publish_sequence = m_publish_sequence;
        const uint64_t sequence = publish_sequence->fetch_add(1, std::memory_order_acq_rel) + 1;
        auto publish = [this, alive, publish_sequence, sequence, view = std::move(view)]() mutable {
            const std::shared_ptr<std::atomic<bool>> guard = alive.lock();
            if (!guard || !guard->load(std::memory_order_acquire) ||
                publish_sequence->load(std::memory_order_acquire) != sequence)
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
    m_publish_sequence->fetch_add(1, std::memory_order_acq_rel);
    m_coordinator.set_observer({});
}

void SmartSlicingPresenter::set_view_changed(ViewChangedFn view_changed)
{
    m_view_changed = std::move(view_changed);
    if (m_view_changed)
        m_view_changed(m_view_model);
}

} // namespace Slic3r::GUI
