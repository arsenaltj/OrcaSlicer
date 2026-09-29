#pragma once

#include "SmartSlicingViewModel.hpp"
#include "slic3r/AI/SmartSlicing/Application/OwnerThreadCallbackGate.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <optional>
#include <thread>
#include <wx/scrolwin.h>
#include <wx/timer.h>

class wxButton;
class wxChoice;
class wxPanel;
class wxRadioButton;
class wxStaticText;
class wxCollapsiblePane;

namespace Slic3r::GUI {
class Plater;

class SmartSlicingPanel final : public wxScrolledWindow
{
public:
    using PlanCandidatesFn = std::function<std::vector<AI::SmartSlicing::SliceCandidate>()>;
    using CancelTrialFn = std::function<void()>;
    using ExecuteTrialFn = std::function<AI::SmartSlicing::TrialSliceResult(
        const AI::SmartSlicing::SliceCandidate&)>;
    using OwnerDispatchFn = std::function<void(std::function<void()>)>;
    using ModeChangedFn = std::function<void(SmartSlicingMode)>;
    using PurposeChangedFn = std::function<void(SmartSlicingPurpose)>;

    SmartSlicingPanel(wxWindow* parent, AI::SmartSlicing::SmartSlicingCoordinator& coordinator,
                      PlanCandidatesFn plan_candidates = {}, CancelTrialFn cancel_trial = {},
                      std::function<void()> add_model = {}, Plater* plater = nullptr,
                      ExecuteTrialFn execute_trial = {}, OwnerDispatchFn owner_dispatch = {},
                      ModeChangedFn mode_changed = {}, PurposeChangedFn purpose_changed = {});
    ~SmartSlicingPanel() override;
    void render(const SmartSlicingViewModel& view_model);
    void shutdown_async();

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
    bool run_trial_task(std::optional<AI::SmartSlicing::CandidateTrialTask> task);
    void complete_trial_task(AI::SmartSlicing::CandidateTrialTask task,
                             AI::SmartSlicing::TrialSliceResult result);

    AI::SmartSlicing::SmartSlicingCoordinator& m_coordinator;
    PlanCandidatesFn m_plan_candidates;
    CancelTrialFn m_cancel_trial;
    ExecuteTrialFn m_execute_trial;
    OwnerDispatchFn m_owner_dispatch;
    ModeChangedFn m_mode_changed;
    PurposeChangedFn m_purpose_changed;
    AI::SmartSlicing::OwnerThreadCallbackGate m_callback_gate;
    wxStaticText* m_mode_label{nullptr};
    wxStaticText* m_purpose_label{nullptr};
    wxStaticText* m_baseline_label{nullptr};
    wxChoice* m_mode_choice{nullptr};
    wxChoice* m_purpose_choice{nullptr};
    std::array<wxStaticText*, 3> m_goal_cards{};
    std::array<wxStaticText*, 4> m_stage_labels{};
    wxStaticText* m_summary{nullptr};
    wxStaticText* m_issues{nullptr};
    wxCollapsiblePane* m_diagnostics{nullptr};
    wxStaticText* m_diagnostic_text{nullptr};
    wxButton* m_add_model{nullptr};
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
    bool m_can_plan_candidates{false};
    bool m_can_recheck{false};
    std::atomic<bool> m_worker_running{false};
    bool m_shutdown{false};
    std::thread m_worker;
};

} // namespace Slic3r::GUI
