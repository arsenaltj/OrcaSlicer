#pragma once

#include "OrcaOrientationCandidateProvider.hpp"
#include "OrcaParameterAdvisor.hpp"
#include "OrcaPlacementCandidateProvider.hpp"

#include <functional>
#include <iterator>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

struct OrcaCandidateProposalInput
{
    AI::SmartSlicing::WorkspaceContext context;
    OrcaPlacementCandidateInput placement;
    OrcaOrientationCandidateInput orientation;
    OrcaParameterAdvisorInput parameters;
};

class OrcaCandidateProposalTask
{
public:
    using CancelPredicate = std::function<bool()>;

    explicit OrcaCandidateProposalTask(OrcaCandidateProposalInput input) : m_input(std::move(input)) {}

    std::vector<AI::SmartSlicing::SliceCandidate> execute(CancelPredicate canceled = {})
    {
        if (!canceled)
            canceled = [] { return false; };
        if (canceled())
            return {};

        m_input.placement.arrange_params.stopcondition = canceled;
        std::vector<AI::SmartSlicing::SliceCandidate> candidates =
            OrcaPlacementCandidateProvider().generate(std::move(m_input.placement), m_input.context.revision);
        if (canceled())
            return {};

        m_input.orientation.stopcondition = canceled;
        std::vector<AI::SmartSlicing::SliceCandidate> orientation_candidates =
            OrcaOrientationCandidateProvider().generate(std::move(m_input.orientation), m_input.context.revision);
        if (canceled())
            return {};
        candidates.insert(candidates.end(), std::make_move_iterator(orientation_candidates.begin()),
                          std::make_move_iterator(orientation_candidates.end()));

        AI::SmartSlicing::ParameterProposal parameter_proposal =
            OrcaParameterAdvisor(std::move(m_input.parameters)).advise(m_input.context);
        if (canceled())
            return {};
        if (parameter_proposal.entries.empty())
            return candidates;

        if (candidates.empty()) {
            AI::SmartSlicing::SliceCandidate candidate;
            candidate.id            = "parameter-brim-stability-v1";
            candidate.base_revision = m_input.context.revision;
            candidate.goal          = AI::SmartSlicing::CandidateGoal::Stability;
            candidate.explanation   = "small_or_slender_footprint_brim_candidate";
            candidate.parameters    = std::move(parameter_proposal);
            candidates.push_back(std::move(candidate));
        } else {
            for (AI::SmartSlicing::SliceCandidate& candidate : candidates)
                candidate.parameters = parameter_proposal;
        }
        return candidates;
    }

private:
    OrcaCandidateProposalInput m_input;
};

} // namespace Slic3r::GUI
