#pragma once

#include "slic3r/AI/SmartSlicing/Ports/IOrcaWorkspace.hpp"
#include "OrcaPlacementCandidateProvider.hpp"
#include "OrcaTrialSliceExecutor.hpp"
#include "slic3r/AI/SmartSlicing/Application/BrimParameterAdvisor.hpp"

namespace Slic3r::GUI {

class Plater;

// Read-only anti-corruption layer for smart slicing. Capturing or inspecting
// through this adapter must never dirty the project or invalidate official
// slicing results.
class OrcaSmartSlicingAdapter final : public AI::SmartSlicing::IOrcaWorkspace
{
public:
    explicit OrcaSmartSlicingAdapter(Plater* plater,
        std::shared_ptr<AI::SmartSlicing::IParameterAdvisor> advisor = AI::SmartSlicing::baseline_parameter_advisor(),
        std::shared_ptr<const AI::Placement::IPlacementEngine> placement = AI::Placement::baseline_engine())
        : m_plater(plater), m_parameter_advisor(advisor ? std::move(advisor) : AI::SmartSlicing::baseline_parameter_advisor()),
          m_placement_engine(placement ? std::move(placement) : AI::Placement::baseline_engine()) {}

    AI::SmartSlicing::WorkspaceRevision current_revision() const override;
    AI::SmartSlicing::WorkspaceContext capture_context() const override;
    OrcaTrialSliceInput capture_trial_slice_input() const;
    std::vector<AI::SmartSlicing::SliceCandidate>
    candidate_proposals(const AI::SmartSlicing::WorkspaceRevision& revision) const;

private:
    AI::SmartSlicing::WorkspaceContext capture_context_impl(bool include_diagnostics) const;

    Plater* m_plater{nullptr};
    std::shared_ptr<AI::SmartSlicing::IParameterAdvisor> m_parameter_advisor;
    std::shared_ptr<const AI::Placement::IPlacementEngine> m_placement_engine;
};

} // namespace Slic3r::GUI
