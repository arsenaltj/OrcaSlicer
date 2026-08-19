#pragma once

#include "SmartSlicingViewModel.hpp"

#include <array>
#include <wx/panel.h>
#include <wx/timer.h>

class wxButton;
class wxStaticText;

namespace Slic3r::AI::SmartSlicing {
class SmartSlicingCoordinator;
}

namespace Slic3r::GUI {

class SmartSlicingPanel final : public wxPanel
{
public:
    SmartSlicingPanel(wxWindow* parent, AI::SmartSlicing::SmartSlicingCoordinator& coordinator);
    void render(const SmartSlicingViewModel& view_model);

private:
    void on_revision_timer(wxTimerEvent& event);

    AI::SmartSlicing::SmartSlicingCoordinator& m_coordinator;
    std::array<wxStaticText*, 4> m_stage_labels{};
    wxStaticText* m_summary{nullptr};
    wxStaticText* m_issues{nullptr};
    wxStaticText* m_p0_notice{nullptr};
    wxButton* m_start{nullptr};
    wxButton* m_cancel{nullptr};
    wxTimer m_revision_timer;
};

} // namespace Slic3r::GUI
