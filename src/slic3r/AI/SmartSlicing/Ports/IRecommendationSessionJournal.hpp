#pragma once

#include "slic3r/AI/SmartSlicing/Domain/RecommendationTypes.hpp"
#include "slic3r/AI/SmartSlicing/Domain/WorkspaceRevision.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

struct RecommendationSessionJournalResult
{
    std::string goal_id;
    GoalResultStatus status{GoalResultStatus::Analyzing};
    CandidateId candidate_id;
    std::vector<std::string> diagnostic_codes;
};

struct RecommendationSessionJournalRecord
{
    WorkflowId workflow_id{0};
    AttemptId attempt_id{0};
    WorkspaceRevision workspace_revision;
    RecommendationSessionJournalResult baseline;
    std::array<RecommendationSessionJournalResult, RECOMMENDATION_GOALS.size()> goals;
    uint32_t contract_version{RECOMMENDATION_CONTRACT_VERSION};
    std::string strategy_version;
    int64_t started_at_epoch_seconds{0};
    int64_t updated_at_epoch_seconds{0};
};

class IRecommendationSessionJournal
{
public:
    virtual ~IRecommendationSessionJournal() = default;
    virtual std::optional<RecommendationSessionJournalRecord> load() = 0;
    virtual void save(const RecommendationSessionJournalRecord& record) = 0;
    virtual void clear() = 0;
};

} // namespace Slic3r::AI::SmartSlicing
