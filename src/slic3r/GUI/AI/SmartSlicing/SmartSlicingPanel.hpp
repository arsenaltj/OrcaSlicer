#pragma once

#include "SmartSlicingViewModel.hpp"
#include "slic3r/GUI/AI/AIWindowAppearance.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <thread>
#include <wx/scrolwin.h>
#include <wx/timer.h>

class wxButton;
class Button;
class wxPanel;
class wxStaticText;

namespace Slic3r::AI::SmartSlicing {
class SmartSlicingCoordinator;
}

namespace Slic3r::GUI {
class Plater;
class OrcaModelPreparationPanel;

class SmartSlicingPanel final : public wxPanel, public AIThemeOwner
{
public:
    using PlanCandidatesFn = std::function<std::vector<AI::SmartSlicing::SliceCandidate>()>;
    using CancelTrialFn = std::function<void()>;

    SmartSlicingPanel(wxWindow* parent, AI::SmartSlicing::SmartSlicingCoordinator& coordinator,
                      PlanCandidatesFn plan_candidates = {}, CancelTrialFn cancel_trial = {},
                      std::function<void()> add_model = {}, Plater* plater = nullptr,
                      std::function<void()> native_settings = {});
    ~SmartSlicingPanel() override;
    void render(const SmartSlicingViewModel& view_model);
    void apply_ai_theme(bool update_fonts) override;
    void refresh_preparation_selection();

private:
    struct CandidateControls
    {
        wxPanel* panel{nullptr};
        Button* selector{nullptr};
        Button* retry{nullptr};
    };

    void on_revision_timer(wxTimerEvent& event);
    bool run_in_background(std::function<void()> work);

    AI::SmartSlicing::SmartSlicingCoordinator& m_coordinator;
    wxScrolledWindow* m_content{nullptr};
    OrcaModelPreparationPanel* m_preparation{nullptr};
    wxPanel* m_footer{nullptr};
    Button* m_confirm{nullptr};
    PlanCandidatesFn m_plan_candidates;
    CancelTrialFn m_cancel_trial;
    std::array<wxStaticText*, 4> m_stage_labels{};
    wxStaticText* m_summary{nullptr};
    wxStaticText* m_issues{nullptr};
    wxPanel* m_diagnostics{nullptr};
    wxStaticText* m_diagnostic_text{nullptr};
    Button* m_add_model{nullptr};
    wxStaticText* m_p0_notice{nullptr};
    wxPanel* m_candidate_section{nullptr};
    wxStaticText* m_candidate_hint{nullptr};
    wxPanel* m_candidate_details{nullptr};
    wxStaticText* m_candidate_title{nullptr};
    wxStaticText* m_candidate_metrics{nullptr};
    wxStaticText* m_candidate_reason{nullptr};
    std::array<CandidateControls, 3> m_candidate_controls{};
    std::array<std::string, 3> m_candidate_ids{};
    Button* m_keep_baseline{nullptr};
    Button* m_apply{nullptr};
    Button* m_undo_apply{nullptr};
    Button* m_start{nullptr};
    Button* m_cancel{nullptr};
    wxTimer m_revision_timer;
    bool m_can_plan_candidates{false};
    bool m_can_recheck{false};
    std::atomic<bool> m_worker_running{false};
    std::thread m_worker;
};

} // namespace Slic3r::GUI
