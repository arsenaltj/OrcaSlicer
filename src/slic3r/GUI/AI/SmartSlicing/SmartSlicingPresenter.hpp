#pragma once

#include "SmartSlicingViewModel.hpp"
#include "slic3r/AI/SmartSlicing/Application/OwnerThreadCallbackGate.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"

#include <functional>

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
    void publish_recommendation_snapshot(const AI::SmartSlicing::RecommendationSnapshot& recommendation);
    void set_mode(SmartSlicingMode mode);
    void set_purpose(SmartSlicingPurpose purpose);

private:
    SmartSlicingViewModel m_view_model;
    AI::SmartSlicing::SmartSlicingCoordinator& m_coordinator;
    ViewChangedFn m_view_changed;
    DispatchFn m_dispatch;
    AI::SmartSlicing::OwnerThreadCallbackGate m_callback_gate;
};

} // namespace Slic3r::GUI
