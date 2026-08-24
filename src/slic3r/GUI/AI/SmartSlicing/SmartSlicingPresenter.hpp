#pragma once

#include "SmartSlicingViewModel.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace Slic3r::GUI {

class SmartSlicingPresenter
{
public:
    using ViewChangedFn = std::function<void(const SmartSlicingViewModel&)>;
    using DispatchFn = std::function<void(std::function<void()>)>;

    explicit SmartSlicingPresenter(AI::SmartSlicing::SmartSlicingCoordinator& coordinator, DispatchFn dispatch = {});
    ~SmartSlicingPresenter();

    const SmartSlicingViewModel& view_model() const { return m_view_model; }
    void set_view_changed(ViewChangedFn view_changed);

private:
    SmartSlicingViewModel m_view_model;
    AI::SmartSlicing::SmartSlicingCoordinator& m_coordinator;
    ViewChangedFn m_view_changed;
    DispatchFn m_dispatch;
    std::shared_ptr<std::atomic<bool>> m_alive;
    std::shared_ptr<std::atomic<uint64_t>> m_publish_sequence;
};

} // namespace Slic3r::GUI
