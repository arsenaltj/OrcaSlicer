#pragma once

#include "slic3r/AI/SmartSlicing/Domain/RecommendationTypes.hpp"
#include "slic3r/AI/SmartSlicing/Domain/WorkspaceRevision.hpp"
#include "slic3r/AI/SmartSlicing/Ports/IRecommendationSessionJournal.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

inline constexpr const char* BASELINE_GOAL_ID = "baseline";

enum class RecommendationSessionState { Idle, Recommending, Ready, Canceled, Stale };
enum class RecommendationTaskOutcome { Ready, Unavailable, Failed };
enum class RecommendationCancellationReason {
    None,
    User,
    ModeChanged,
    WorkspaceChanged,
    Deadline,
    Shutdown,
    Superseded,
};

class RecommendationCancellationToken
{
public:
    bool cancellation_requested() const;
    RecommendationCancellationReason reason() const;

private:
    friend class RecommendationSessionCoordinator;
    bool request(RecommendationCancellationReason reason);

    std::atomic<RecommendationCancellationReason> m_reason{RecommendationCancellationReason::None};
};

struct RecommendationTaskIdentity
{
    WorkflowId workflow_id{0};
    AttemptId attempt_id{0};
    WorkspaceRevision workspace_revision;
    CandidateId candidate_id;
    std::string goal_id;
};

struct RecommendationTaskResult
{
    RecommendationTaskIdentity identity;
    RecommendationTaskOutcome outcome{RecommendationTaskOutcome::Failed};
    std::vector<std::string> diagnostic_codes;
    std::optional<RecommendationEvidence> evidence;
    CandidateId selected_candidate_id;
    std::shared_ptr<const OwnerRiskConfirmationContract> risk_confirmation_contract;
};

struct StartRecommendationSessionCommand
{
    WorkflowId workflow_id{0};
    AttemptId attempt_id{0};
    WorkspaceRevision workspace_revision;
    CandidateId baseline_candidate_id;
    std::array<CandidateId, RECOMMENDATION_GOALS.size()> goal_candidate_ids;
    std::string strategy_version;
    std::optional<std::chrono::steady_clock::time_point> requested_at;
    std::optional<int64_t> requested_at_epoch_seconds;
};

struct CancelRecommendationSessionCommand
{
    WorkflowId workflow_id{0};
    AttemptId attempt_id{0};
    RecommendationCancellationReason reason{RecommendationCancellationReason::User};
};

struct SupersedeRecommendationSessionCommand
{
    WorkflowId workflow_id{0};
    AttemptId attempt_id{0};
};

struct WorkspaceRevisionChangedCommand
{
    WorkflowId workflow_id{0};
    AttemptId attempt_id{0};
    WorkspaceRevision workspace_revision;
};

using RecommendationSessionCommand = std::variant<StartRecommendationSessionCommand,
                                                  CancelRecommendationSessionCommand,
                                                  SupersedeRecommendationSessionCommand,
                                                  WorkspaceRevisionChangedCommand,
                                                  RecommendationTaskResult>;

struct RecommendationSessionSnapshot
{
    WorkflowId workflow_id{0};
    AttemptId attempt_id{0};
    WorkspaceRevision workspace_revision;
    RecommendationSessionState state{RecommendationSessionState::Idle};
    RecommendationCancellationReason cancellation_reason{RecommendationCancellationReason::None};
    std::string strategy_version;
    RecommendationSnapshot recommendation;
    uint64_t publication_revision{0};
};

struct InterruptedRecommendationGoalMetadata
{
    std::string goal_id;
    GoalResultStatus status{GoalResultStatus::Analyzing};
    std::vector<std::string> diagnostic_codes;
};

struct InterruptedRecommendationSessionMetadata
{
    WorkflowId workflow_id{0};
    AttemptId attempt_id{0};
    WorkspaceRevision workspace_revision;
    GoalResultStatus baseline_status{GoalResultStatus::Analyzing};
    std::vector<std::string> baseline_diagnostic_codes;
    std::array<InterruptedRecommendationGoalMetadata, RECOMMENDATION_GOALS.size()> goals;
    uint32_t contract_version{RECOMMENDATION_CONTRACT_VERSION};
    std::string strategy_version;
    int64_t started_at_epoch_seconds{0};
    int64_t updated_at_epoch_seconds{0};
    std::string interruption_diagnostic_code{"previous_session_interrupted"};
};

class RecommendationSessionCoordinator
{
public:
    using MonotonicClock = std::function<std::chrono::steady_clock::time_point()>;
    using WallClock = std::function<int64_t()>;
    using Observer = std::function<void(const RecommendationSessionSnapshot&)>;
    using DiscardHandler = std::function<void(const RecommendationTaskResult&)>;
    using CleanupHandler = std::function<void(RecommendationCancellationReason)>;

    RecommendationSessionCoordinator();
    RecommendationSessionCoordinator(MonotonicClock monotonic_clock, WallClock wall_clock);

    void enqueue(RecommendationSessionCommand command);
    bool process_next();
    size_t process_all();
    size_t pending_command_count() const;
    bool check_deadline();

    const RecommendationSessionSnapshot& snapshot() const;
    std::shared_ptr<const RecommendationCancellationToken> cancellation_token() const;
    void set_observer(Observer observer);
    void set_discard_handler(DiscardHandler handler);
    void set_cleanup_handler(CleanupHandler handler);
    void set_journal(IRecommendationSessionJournal& journal);
    std::optional<InterruptedRecommendationSessionMetadata> recover_interrupted_session();
    size_t discarded_result_count() const;
    size_t journal_error_count() const;

private:
    void assert_owner_thread() const;
    void process(StartRecommendationSessionCommand command);
    void process(const CancelRecommendationSessionCommand& command);
    void process(const SupersedeRecommendationSessionCommand& command);
    void process(const WorkspaceRevisionChangedCommand& command);
    void process(RecommendationTaskResult result);
    void publish();
    void discard(const RecommendationTaskResult& result);
    bool request_cancellation(RecommendationCancellationReason reason);
    bool deadline_due() const;
    bool active_work_remaining() const;
    RecommendationSessionJournalRecord journal_record() const;
    void persist_journal();
    void clear_journal();

    const std::thread::id m_owner_thread;
    MonotonicClock m_monotonic_clock;
    WallClock m_wall_clock;
    mutable std::mutex m_queue_mutex;
    std::deque<RecommendationSessionCommand> m_commands;
    RecommendationSessionSnapshot m_snapshot;
    std::chrono::steady_clock::time_point m_started_at;
    int64_t m_started_at_epoch_seconds{0};
    std::shared_ptr<RecommendationCancellationToken> m_cancellation_token;
    Observer m_observer;
    DiscardHandler m_discard_handler;
    CleanupHandler m_cleanup_handler;
    IRecommendationSessionJournal* m_journal{nullptr};
    bool m_cleanup_notified{false};
    size_t m_discarded_result_count{0};
    size_t m_journal_error_count{0};
};

} // namespace Slic3r::AI::SmartSlicing
