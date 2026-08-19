#pragma once

#include "SmartSlicingViewModel.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"

#include <functional>

namespace Slic3r::GUI {

class SmartSlicingPresenter
{
public:
    using ViewChangedFn = std::function<void(const SmartSlicingViewModel&)>;

    explicit SmartSlicingPresenter(AI::SmartSlicing::SmartSlicingCoordinator& coordinator);
    ~SmartSlicingPresenter();

    const SmartSlicingViewModel& view_model() const { return m_view_model; }
    void set_view_changed(ViewChangedFn view_changed);

private:
    SmartSlicingViewModel m_view_model;
    AI::SmartSlicing::SmartSlicingCoordinator& m_coordinator;
    ViewChangedFn m_view_changed;
};

} // namespace Slic3r::GUI
