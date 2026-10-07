#pragma once

#include "slic3r/AI/SmartSlicing/Domain/GoalContract.hpp"
#include "slic3r/AI/SmartSlicing/Domain/ModelFeatureSnapshot.hpp"
#include "slic3r/AI/SmartSlicing/Domain/ParameterProposalValidator.hpp"
#include "slic3r/AI/SmartSlicing/Domain/PlacementCandidate.hpp"
#include "slic3r/AI/SmartSlicing/Domain/ProtectedRegionBinding.hpp"
#include "slic3r/AI/SmartSlicing/Domain/WorkspaceRevision.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

inline constexpr const char* CANDIDATE_TRIAL_COST_POLICY_VERSION = "candidate-trial-cost/v1";
inline constexpr double CANDIDATE_TRIAL_PARAMETER_TEMPLATE_STRIDE = 1000.0;

enum class OrientationStrategy { Current, StablePlane, LowSupport, ProtectedSurface };

struct OrientationSearchOption
{
    std::string option_id;
    OrientationStrategy strategy{OrientationStrategy::Current};
    std::array<double, 16> transform{};
    FeatureAvailability evidence_availability{FeatureAvailability::Unknown};
    std::string evidence_source;
    std::string evidence_version;
    double static_cost{0.0};
};

struct CandidateSearchObjectInput
{
    uint64_t object_id{0};
    uint64_t instance_id{0};
    std::array<double, 16> current_transform{};
    bool locked{false};
    std::vector<OrientationSearchOption> orientation_options;
    std::vector<ModelFeatureSnapshot> model_features;
    ProtectedRegionBindingStatus protected_region_status{ProtectedRegionBindingStatus::Unknown};
    std::vector<ProtectedRegionSnapshot> protected_regions;
};

struct ProfileParameterValue
{
    ConfigValue value{false};
    bool valid{false};
    std::string source_code;
    std::string source_version;
};

struct CandidateSearchParameterInput
{
    ConfigScope scope{ConfigScope::Plate};
    PresetOwner owner{PresetOwner::Process};
    int64_t target_id{0};
    std::string key;
    ConfigValue current_value{false};
    std::string current_source_code;
    std::string current_source_version;
    std::vector<ProfileParameterValue> options;
    std::vector<ParameterBoundEvidence> bounds;
};

struct CandidateSearchInput
{
    WorkspaceRevision workspace_revision;
    UsagePurpose usage_purpose{UsagePurpose::General};
    std::string parameter_policy_version{PARAMETER_POLICY_VERSION};
    bool plate_locked{false};
    bool may_increase_process_speed{false};
    bool tpu_speed_restricted{false};
    std::vector<CandidateSearchObjectInput> objects;
    std::vector<CandidateSearchParameterInput> profile_parameters;
    IntentConstraintSnapshot intent_constraints;
    std::function<NativeParameterValidationResult(const ParameterProposal&)> native_validator;
};

struct CandidateSearchDraft
{
    CandidateId candidate_id;
    RecommendationGoal goal{RecommendationGoal::Balanced};
    double estimated_trial_cost{0.0};
    PlacementCandidate placement;
    ParameterProposal parameters;
    CandidateStatus status{CandidateStatus::Draft};
    std::vector<std::string> diagnostic_codes;
    std::vector<std::string> explanation_codes;
};

struct RejectedCandidateDraft
{
    CandidateId candidate_id;
    RecommendationGoal goal{RecommendationGoal::Balanced};
    std::vector<std::string> diagnostic_codes;
};

struct GoalCandidateDrafts
{
    RecommendationGoal goal{RecommendationGoal::Balanced};
    size_t board_beam_count{0};
    size_t generated_static_draft_count{0};
    std::vector<CandidateSearchDraft> drafts;
    std::vector<CandidateSearchDraft> selected_for_trial;
    std::vector<RejectedCandidateDraft> rejected_drafts;
    std::vector<std::string> diagnostic_codes;
};

struct CandidateSearchObjectSummary
{
    uint64_t object_id{0};
    uint64_t instance_id{0};
    size_t normalized_orientation_count{0};
    bool locked{false};
};

struct BaselineDescriptor
{
    CandidateId candidate_id;
    WorkspaceRevision workspace_revision;
    ParameterProposal parameters;
    size_t trial_slots{1};
};

struct CandidateSearchResult
{
    std::string budget_version;
    std::string trial_cost_policy_version;
    BaselineDescriptor baseline;
    std::array<GoalCandidateDrafts, RECOMMENDATION_GOALS.size()> goals;
    std::vector<CandidateSearchObjectSummary> objects;
    size_t total_trial_slots{1};

    GoalCandidateDrafts& goal(RecommendationGoal value);
    const GoalCandidateDrafts& goal(RecommendationGoal value) const;
};

class CandidateSearchPipeline
{
public:
    CandidateSearchResult search(const CandidateSearchInput& input) const;
};

const char* orientation_strategy_name(OrientationStrategy strategy);

} // namespace Slic3r::AI::SmartSlicing
