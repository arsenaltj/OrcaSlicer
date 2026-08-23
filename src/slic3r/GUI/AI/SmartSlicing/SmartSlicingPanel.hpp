#pragma once

#include "SmartSlicingViewModel.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
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
    using CancelPredicate = std::function<bool()>;
    using CandidatePlanTask =
        std::function<std::vector<AI::SmartSlicing::SliceCandidate>(CancelPredicate)>;
    using PrepareCandidatesFn = std::function<CandidatePlanTask()>;
    using CancelTrialFn = std::function<void()>;
    using FocusIssueFn = std::function<void(uint64_t)>;

    SmartSlicingPanel(wxWindow* parent, AI::SmartSlicing::SmartSlicingCoordinator& coordinator,
                      PrepareCandidatesFn prepare_candidates = {}, CancelTrialFn cancel_trial = {},
                      FocusIssueFn focus_issue = {});
    ~SmartSlicingPanel() override;
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
    bool run_in_background(std::function<void()> work);

    AI::SmartSlicing::SmartSlicingCoordinator& m_coordinator;
    PrepareCandidatesFn m_prepare_candidates;
    CancelTrialFn m_cancel_trial;
    FocusIssueFn m_focus_issue;
    std::array<wxStaticText*, 4> m_stage_labels{};
    wxStaticText* m_summary{nullptr};
    wxStaticText* m_issues{nullptr};
    std::array<wxButton*, 5> m_issue_focus_buttons{};
    std::array<uint64_t, 5> m_issue_object_ids{};
    wxStaticText* m_p0_notice{nullptr};
    wxPanel* m_candidate_section{nullptr};
    std::array<CandidateControls, 3> m_candidate_controls{};
    std::array<std::string, 3> m_candidate_ids{};
    wxButton* m_keep_baseline{nullptr};
    wxButton* m_apply{nullptr};
    wxButton* m_undo_apply{nullptr};
    wxButton* m_start{nullptr};
    wxButton* m_cancel{nullptr};
    wxTimer m_revision_timer;
    bool m_can_accept_risk{false};
    bool m_can_plan_candidates{false};
    std::atomic<bool> m_cancel_requested{false};
    std::atomic<bool> m_worker_running{false};
    std::thread m_worker;
};

} // namespace Slic3r::GUI
