#pragma once

#include "CandidateSearchSessionPlanner.hpp"
#include "CandidateSelectionTaskMapper.hpp"
#include "slic3r/AI/SmartSlicing/Domain/MachineCapabilitySnapshot.hpp"
#include "slic3r/AI/SmartSlicing/Domain/RiskAssessment.hpp"
#include "slic3r/AI/SmartSlicing/Domain/TrialMetrics.hpp"
#include "slic3r/AI/SmartSlicing/Ports/ITrialSliceExecutor.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

inline constexpr size_t TRIAL_SLICE_TOTAL_LIMIT = 10;
inline constexpr size_t TRIAL_SLICE_PER_GOAL_LIMIT = 3;

struct TrialSliceTask
{
    RecommendationTaskIdentity identity;
    CandidateId baseline_candidate_id;
    std::string strategy_version;
    std::optional<RecommendationGoal> goal;
    double estimated_cost{0.0};
    PlacementCandidate placement;
    ParameterProposal parameters;
    std::chrono::steady_clock::time_point deadline;

    bool baseline() const { return !goal.has_value(); }
};

struct TrialEvaluationFacts
{
    MetricValue<bool> manual_intent_preserved;
    MetricValue<int> effective_wall_loops;
    MetricValue<double> effective_infill_percent;
    MetricValue<bool> critical_surface_significantly_improved;
};

struct VersionedTrialSliceResult
{
    RecommendationTaskIdentity identity;
    TrialSliceStatus status{TrialSliceStatus::Failed};
    std::optional<TrialMetrics> metrics;
    std::optional<RiskAssessment> risks;
    TrialEvaluationFacts evaluation_facts;
    std::vector<std::string> diagnostic_codes;
};

using VersionedTrialExecute = std::function<VersionedTrialSliceResult(
    const TrialSliceTask&, const std::shared_ptr<const RecommendationCancellationToken>&)>;
using RecommendationResultPublisher = std::function<void(RecommendationTaskResult)>;

struct TrialSliceSchedulerInput
{
    CandidateSearchResult search_result;
    CandidateSearchSessionPlan session_plan;
    UsagePurpose usage_purpose{UsagePurpose::General};
    MachineSupportStatus machine_support_status{MachineSupportStatus::PendingValidation};
    std::shared_ptr<const RecommendationCancellationToken> cancellation_token;
    std::chrono::steady_clock::time_point deadline;
};

enum class TrialSliceSchedulerStatus { Completed, Canceled, DeadlineExceeded, Blocked, InvalidInput };

struct TrialSliceSchedulerReport
{
    TrialSliceSchedulerStatus status{TrialSliceSchedulerStatus::InvalidInput};
    size_t total_execution_count{0};
    size_t baseline_execution_count{0};
    std::array<size_t, RECOMMENDATION_GOALS.size()> goal_execution_counts{};
    size_t maximum_concurrency{0};
    std::vector<CandidateId> execution_order;
    std::vector<std::string> diagnostic_codes;
};

class TrialSliceScheduler
{
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    TrialSliceScheduler();
    explicit TrialSliceScheduler(Clock clock);

    TrialSliceSchedulerReport run(const TrialSliceSchedulerInput& input,
                                  const VersionedTrialExecute& execute,
                                  const RecommendationResultPublisher& publish) const;

private:
    Clock m_clock;
};

} // namespace Slic3r::AI::SmartSlicing
