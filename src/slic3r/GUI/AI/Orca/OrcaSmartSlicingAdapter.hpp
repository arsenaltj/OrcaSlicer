#pragma once

#include "slic3r/AI/SmartSlicing/Ports/IOrcaWorkspace.hpp"
#include "slic3r/AI/SmartSlicing/Infrastructure/InMemoryUserMarkedRegionRegistry.hpp"
#include "OrcaCandidateSearchAdapter.hpp"
#include "OrcaModelFeatureAnalyzer.hpp"
#include "OrcaPlacementCandidateProvider.hpp"
#include "OrcaTrialSliceExecutor.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace Slic3r::GUI {

class Plater;

// Read-only anti-corruption layer for smart slicing. Capturing or inspecting
// through this adapter must never dirty the project or invalidate official
// slicing results.
class OrcaSmartSlicingAdapter final : public AI::SmartSlicing::IOrcaWorkspace
{
public:
    explicit OrcaSmartSlicingAdapter(Plater* plater) : m_plater(plater) {}

    AI::SmartSlicing::WorkspaceRevision current_revision() const override;
    AI::SmartSlicing::WorkspaceContext capture_context() const override;
    bool set_usage_purpose(AI::SmartSlicing::UsagePurpose purpose);
    AI::SmartSlicing::UsagePurpose usage_purpose() const { return m_usage_purpose; }
    AI::SmartSlicing::ProtectedRegionBindingResult mark_user_marked_region(
        uint64_t object_id, uint64_t volume_id, std::vector<AI::ProtectedFacetRange> ranges);
    bool clear_user_marked_region(uint64_t object_id, uint64_t volume_id);
    AI::SmartSlicing::ProtectedRegionSourceResult query_user_marked_region(uint64_t object_id,
                                                                            uint64_t volume_id) const;
    OrcaCandidateSearchCaptureResult capture_candidate_search_input() const;
    OrcaTrialSliceInput capture_trial_slice_input() const;
    std::vector<AI::SmartSlicing::SliceCandidate>
    candidate_proposals(const AI::SmartSlicing::WorkspaceRevision& revision) const;
    OrcaModelFeatureCaptureResult capture_model_feature_inputs(
        const OrcaModelFeatureTarget& target,
        const AI::SmartSlicing::ModelFeatureAnalysisLimits& limits) const;

private:
    std::optional<AI::SmartSlicing::ProtectedRegionBindingTarget> current_region_target(
        uint64_t object_id, uint64_t volume_id) const;
    AI::SmartSlicing::WorkspaceContext capture_context_impl(bool include_diagnostics) const;

    Plater* m_plater{nullptr};
    AI::SmartSlicing::UsagePurpose m_usage_purpose{AI::SmartSlicing::UsagePurpose::General};
    mutable AI::SmartSlicing::InMemoryUserMarkedRegionRegistry m_user_marked_regions;
};

} // namespace Slic3r::GUI
