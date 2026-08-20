#pragma once

#include "SmartSlicingViewModel.hpp"

#include <array>
#include <functional>
#include <wx/panel.h>
#include <wx/timer.h>

class wxButton;
class wxPanel;
class wxRadioButton;
class wxStaticText;

namespace Slic3r::AI::SmartSlicing {
class SmartSlicingCoordinator;
}

namespace Slic3r::GUI {

class SmartSlicingPanel final : public wxPanel
{
public:
    using PlanCandidatesFn = std::function<std::vector<AI::SmartSlicing::SliceCandidate>()>;

    SmartSlicingPanel(wxWindow* parent, AI::SmartSlicing::SmartSlicingCoordinator& coordinator,
                      PlanCandidatesFn plan_candidates = {});
    void render(const SmartSlicingViewModel& view_model);

private:
    struct CandidateControls
    {
        wxPanel* panel{nullptr};
        wxRadioButton* selector{nullptr};
        wxStaticText* metrics{nullptr};
        wxStaticText* reason{nullptr};
        wxButton* retry{nullptr};
    };

    void on_revision_timer(wxTimerEvent& event);

    AI::SmartSlicing::SmartSlicingCoordinator& m_coordinator;
    PlanCandidatesFn m_plan_candidates;
    std::array<wxStaticText*, 4> m_stage_labels{};
    wxStaticText* m_summary{nullptr};
    wxStaticText* m_issues{nullptr};
    wxStaticText* m_p0_notice{nullptr};
    wxPanel* m_candidate_section{nullptr};
    std::array<CandidateControls, 3> m_candidate_controls{};
    std::array<std::string, 3> m_candidate_ids{};
    wxButton* m_keep_baseline{nullptr};
    wxButton* m_apply{nullptr};
    wxButton* m_start{nullptr};
    wxButton* m_cancel{nullptr};
    wxTimer m_revision_timer;
    bool m_can_plan_candidates{false};
};

} // namespace Slic3r::GUI
