#include "RecommendationSessionCoordinator.hpp"

#include "slic3r/AI/SmartSlicing/Domain/RiskConfirmationPolicy.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace Slic3r::AI::SmartSlicing {
namespace {

bool valid_start(const StartRecommendationSessionCommand& command)
{
    return command.workflow_id != 0 && command.attempt_id != 0 && command.workspace_revision.valid() &&
           !command.baseline_candidate_id.empty() && !command.strategy_version.empty() &&
           std::all_of(command.goal_candidate_ids.begin(), command.goal_candidate_ids.end(),
                       [](const CandidateId& candidate_id) { return !candidate_id.empty(); });
}

GoalResultStatus result_status(RecommendationTaskOutcome outcome)
{
    switch (outcome) {
    case RecommendationTaskOutcome::Ready: return GoalResultStatus::Ready;
    case RecommendationTaskOutcome::Unavailable: return GoalResultStatus::Unavailable;
    case RecommendationTaskOutcome::Failed: return GoalResultStatus::Failed;
    }
    return GoalResultStatus::Failed;
}

bool terminal(GoalResultStatus status)
{
    return status == GoalResultStatus::Ready || status == GoalResultStatus::Unavailable ||
           status == GoalResultStatus::Failed || status == GoalResultStatus::Stale ||
           status == GoalResultStatus::Applied;
}

void mark_stale(GoalResult& result)
{
    if (result.status != GoalResultStatus::Applied)
        result.status = GoalResultStatus::Stale;
    result.risk_confirmation_contract.reset();
}

void clear_risk_confirmation_contracts(RecommendationSessionSnapshot& snapshot)
{
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS)
        snapshot.recommendation.goal_result(goal).risk_confirmation_contract.reset();
}

bool known_status(GoalResultStatus status)
{
    switch (status) {
    case GoalResultStatus::Analyzing:
    case GoalResultStatus::Ready:
    case GoalResultStatus::Unavailable:
    case GoalResultStatus::Failed:
    case GoalResultStatus::Stale:
    case GoalResultStatus::Applied: return true;
    }
    return false;
}

bool valid_diagnostic_code(const std::string& code)
{
    return valid_recommendation_code(code);
}

bool valid_diagnostic_codes(const std::vector<std::string>& codes)
{
    return std::all_of(codes.begin(), codes.end(), valid_diagnostic_code);
}

std::vector<std::string> journal_diagnostic_codes(const std::vector<std::string>& codes)
{
    std::vector<std::string> result;
    result.reserve(codes.size());
    for (const std::string& code : codes) {
        if (valid_diagnostic_code(code))
            result.push_back(code);
        else if (std::find(result.begin(), result.end(), "invalid_diagnostic_code") == result.end())
            result.emplace_back("invalid_diagnostic_code");
    }
    return result;
}

bool valid_journal_result(const RecommendationSessionJournalResult& result,
                          const std::string& expected_goal_id)
{
    return result.goal_id == expected_goal_id && known_status(result.status) &&
           !result.candidate_id.empty() && valid_diagnostic_codes(result.diagnostic_codes);
}

bool valid_journal_record(const RecommendationSessionJournalRecord& record)
{
    if (record.workflow_id == 0 || record.attempt_id == 0 || !record.workspace_revision.valid() ||
        record.contract_version != RECOMMENDATION_CONTRACT_VERSION || record.strategy_version.empty() ||
        record.started_at_epoch_seconds < 0 || record.updated_at_epoch_seconds < record.started_at_epoch_seconds ||
        !valid_journal_result(record.baseline, BASELINE_GOAL_ID))
        return false;
    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index) {
        if (!valid_journal_result(record.goals[index], recommendation_goal_id(RECOMMENDATION_GOALS[index])))
            return false;
    }
    return true;
}

int64_t system_epoch_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool is_external_cancel_reason(RecommendationCancellationReason reason)
{
    return reason == RecommendationCancellationReason::User ||
           reason == RecommendationCancellationReason::ModeChanged ||
           reason == RecommendationCancellationReason::Shutdown;
}

} // namespace

bool RecommendationCancellationToken::cancellation_requested() const
{
    return reason() != RecommendationCancellationReason::None;
}

RecommendationCancellationReason RecommendationCancellationToken::reason() const
{
    return m_reason.load(std::memory_order_acquire);
}

bool RecommendationCancellationToken::request(RecommendationCancellationReason reason)
{
    RecommendationCancellationReason expected = RecommendationCancellationReason::None;
    return reason != RecommendationCancellationReason::None &&
           m_reason.compare_exchange_strong(expected, reason, std::memory_order_acq_rel);
}

RecommendationSessionCoordinator::RecommendationSessionCoordinator()
    : RecommendationSessionCoordinator([] { return std::chrono::steady_clock::now(); }, system_epoch_seconds)
{}

RecommendationSessionCoordinator::RecommendationSessionCoordinator(MonotonicClock monotonic_clock,
                                                                     WallClock wall_clock)
    : m_owner_thread(std::this_thread::get_id())
    , m_monotonic_clock(std::move(monotonic_clock))
    , m_wall_clock(std::move(wall_clock))
    , m_cancellation_token(std::make_shared<RecommendationCancellationToken>())
{
    if (!m_monotonic_clock || !m_wall_clock)
        throw std::invalid_argument("recommendation_session_clock_required");
}

void RecommendationSessionCoordinator::enqueue(RecommendationSessionCommand command)
{
    if (StartRecommendationSessionCommand* start = std::get_if<StartRecommendationSessionCommand>(&command)) {
        if (!start->requested_at)
            start->requested_at = m_monotonic_clock();
        if (!start->requested_at_epoch_seconds)
            start->requested_at_epoch_seconds = m_wall_clock();
    }
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    m_commands.push_back(std::move(command));
}

bool RecommendationSessionCoordinator::process_next()
{
    assert_owner_thread();
    check_deadline();
    RecommendationSessionCommand command;
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        if (m_commands.empty())
            return false;
        command = std::move(m_commands.front());
        m_commands.pop_front();
    }
    std::visit([this](auto&& value) { process(std::forward<decltype(value)>(value)); }, std::move(command));
    return true;
}

size_t RecommendationSessionCoordinator::process_all()
{
    assert_owner_thread();
    size_t processed = 0;
    while (process_next())
        ++processed;
    return processed;
}

size_t RecommendationSessionCoordinator::pending_command_count() const
{
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    return m_commands.size();
}

bool RecommendationSessionCoordinator::check_deadline()
{
    assert_owner_thread();
    if (!deadline_due())
        return false;

    if (m_snapshot.recommendation.baseline.status == GoalResultStatus::Analyzing) {
        m_snapshot.recommendation.baseline.status = GoalResultStatus::Unavailable;
        m_snapshot.recommendation.baseline.diagnostic_codes = {"deadline_exceeded"};
    }
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS) {
        GoalResult& result = m_snapshot.recommendation.goal_result(goal);
        if (result.status == GoalResultStatus::Analyzing) {
            result.status = GoalResultStatus::Unavailable;
            result.diagnostic_codes = {"deadline_exceeded"};
        }
    }
    m_snapshot.state = RecommendationSessionState::Ready;
    request_cancellation(RecommendationCancellationReason::Deadline);
    publish();
    return true;
}

const RecommendationSessionSnapshot& RecommendationSessionCoordinator::snapshot() const
{
    assert_owner_thread();
    return m_snapshot;
}

std::shared_ptr<const RecommendationCancellationToken> RecommendationSessionCoordinator::cancellation_token() const
{
    return std::atomic_load(&m_cancellation_token);
}

void RecommendationSessionCoordinator::set_observer(Observer observer)
{
    assert_owner_thread();
    m_observer = std::move(observer);
    if (m_observer)
        m_observer(m_snapshot);
}

void RecommendationSessionCoordinator::set_discard_handler(DiscardHandler handler)
{
    assert_owner_thread();
    m_discard_handler = std::move(handler);
}

void RecommendationSessionCoordinator::set_cleanup_handler(CleanupHandler handler)
{
    assert_owner_thread();
    m_cleanup_handler = std::move(handler);
}

void RecommendationSessionCoordinator::set_journal(IRecommendationSessionJournal& journal)
{
    assert_owner_thread();
    m_journal = &journal;
}

std::optional<InterruptedRecommendationSessionMetadata>
RecommendationSessionCoordinator::recover_interrupted_session()
{
    assert_owner_thread();
    if (!m_journal || m_snapshot.state != RecommendationSessionState::Idle)
        return std::nullopt;

    std::optional<RecommendationSessionJournalRecord> record;
    try {
        record = m_journal->load();
    } catch (...) {
        ++m_journal_error_count;
        return std::nullopt;
    }
    if (!record)
        return std::nullopt;
    if (!valid_journal_record(*record)) {
        ++m_journal_error_count;
        clear_journal();
        return std::nullopt;
    }

    const bool interrupted = record->baseline.status == GoalResultStatus::Analyzing ||
                             std::any_of(record->goals.begin(), record->goals.end(),
                                         [](const RecommendationSessionJournalResult& result) {
                                             return result.status == GoalResultStatus::Analyzing;
                                         });
    if (!interrupted) {
        clear_journal();
        return std::nullopt;
    }

    InterruptedRecommendationSessionMetadata metadata;
    metadata.workflow_id = record->workflow_id;
    metadata.attempt_id = record->attempt_id;
    metadata.workspace_revision = record->workspace_revision;
    metadata.baseline_status = record->baseline.status;
    metadata.baseline_diagnostic_codes = record->baseline.diagnostic_codes;
    metadata.contract_version = record->contract_version;
    metadata.strategy_version = record->strategy_version;
    metadata.started_at_epoch_seconds = record->started_at_epoch_seconds;
    metadata.updated_at_epoch_seconds = record->updated_at_epoch_seconds;
    for (size_t index = 0; index < metadata.goals.size(); ++index) {
        metadata.goals[index].goal_id = record->goals[index].goal_id;
        metadata.goals[index].status = record->goals[index].status;
        metadata.goals[index].diagnostic_codes = record->goals[index].diagnostic_codes;
    }
    clear_journal();
    return metadata;
}

size_t RecommendationSessionCoordinator::discarded_result_count() const
{
    assert_owner_thread();
    return m_discarded_result_count;
}

size_t RecommendationSessionCoordinator::journal_error_count() const
{
    assert_owner_thread();
    return m_journal_error_count;
}

void RecommendationSessionCoordinator::assert_owner_thread() const
{
    if (std::this_thread::get_id() != m_owner_thread)
        throw std::logic_error("recommendation_session_owner_thread_required");
}

void RecommendationSessionCoordinator::process(StartRecommendationSessionCommand command)
{
    if (!valid_start(command))
        return;
    if (m_snapshot.workflow_id != 0 &&
        (command.workflow_id < m_snapshot.workflow_id ||
         (command.workflow_id == m_snapshot.workflow_id && command.attempt_id <= m_snapshot.attempt_id)))
        return;

    if (m_snapshot.state == RecommendationSessionState::Recommending ||
        m_snapshot.state == RecommendationSessionState::Ready)
        request_cancellation(RecommendationCancellationReason::Superseded);

    m_snapshot = {};
    m_snapshot.workflow_id = command.workflow_id;
    m_snapshot.attempt_id = command.attempt_id;
    m_snapshot.workspace_revision = std::move(command.workspace_revision);
    m_snapshot.state = RecommendationSessionState::Recommending;
    m_snapshot.strategy_version = std::move(command.strategy_version);
    m_snapshot.recommendation.baseline.candidate_id = std::move(command.baseline_candidate_id);
    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index)
        m_snapshot.recommendation.goal_result(RECOMMENDATION_GOALS[index]).candidate_id =
            std::move(command.goal_candidate_ids[index]);
    m_started_at = command.requested_at ? *command.requested_at : m_monotonic_clock();
    m_started_at_epoch_seconds = command.requested_at_epoch_seconds ?
                                     *command.requested_at_epoch_seconds :
                                     m_wall_clock();
    std::atomic_store(&m_cancellation_token, std::make_shared<RecommendationCancellationToken>());
    m_cleanup_notified = false;
    publish();
}

void RecommendationSessionCoordinator::process(const CancelRecommendationSessionCommand& command)
{
    if (command.workflow_id != m_snapshot.workflow_id || command.attempt_id != m_snapshot.attempt_id ||
        m_snapshot.state == RecommendationSessionState::Idle ||
        m_snapshot.state == RecommendationSessionState::Canceled ||
        m_snapshot.state == RecommendationSessionState::Stale || !is_external_cancel_reason(command.reason))
        return;
    m_snapshot.state = RecommendationSessionState::Canceled;
    clear_risk_confirmation_contracts(m_snapshot);
    request_cancellation(command.reason);
    publish();
}

void RecommendationSessionCoordinator::process(
    const SupersedeRecommendationSessionCommand& command)
{
    if (command.workflow_id != m_snapshot.workflow_id ||
        command.attempt_id != m_snapshot.attempt_id ||
        (m_snapshot.state != RecommendationSessionState::Recommending &&
         m_snapshot.state != RecommendationSessionState::Ready))
        return;
    m_snapshot.state = RecommendationSessionState::Canceled;
    clear_risk_confirmation_contracts(m_snapshot);
    request_cancellation(RecommendationCancellationReason::Superseded);
    publish();
}

void RecommendationSessionCoordinator::process(const WorkspaceRevisionChangedCommand& command)
{
    if (command.workflow_id != m_snapshot.workflow_id || command.attempt_id != m_snapshot.attempt_id ||
        command.workspace_revision == m_snapshot.workspace_revision ||
        m_snapshot.state == RecommendationSessionState::Idle ||
        m_snapshot.state == RecommendationSessionState::Canceled ||
        m_snapshot.state == RecommendationSessionState::Stale)
        return;
    m_snapshot.state = RecommendationSessionState::Stale;
    if (m_snapshot.recommendation.baseline.status != GoalResultStatus::Applied)
        m_snapshot.recommendation.baseline.status = GoalResultStatus::Stale;
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS)
        mark_stale(m_snapshot.recommendation.goal_result(goal));
    request_cancellation(RecommendationCancellationReason::WorkspaceChanged);
    publish();
}

void RecommendationSessionCoordinator::process(RecommendationTaskResult result)
{
    const std::shared_ptr<RecommendationCancellationToken> token = std::atomic_load(&m_cancellation_token);
    if (m_snapshot.state == RecommendationSessionState::Idle ||
        m_snapshot.state == RecommendationSessionState::Canceled ||
        m_snapshot.state == RecommendationSessionState::Stale ||
        (token && token->cancellation_requested()) ||
        result.identity.workflow_id != m_snapshot.workflow_id ||
        result.identity.attempt_id != m_snapshot.attempt_id ||
        result.identity.workspace_revision != m_snapshot.workspace_revision) {
        discard(result);
        return;
    }

    if (result.identity.goal_id == BASELINE_GOAL_ID) {
        BaselineResult& baseline = m_snapshot.recommendation.baseline;
        if (result.identity.candidate_id != baseline.candidate_id || terminal(baseline.status) ||
             !valid_diagnostic_codes(result.diagnostic_codes) || result.evidence ||
             !result.selected_candidate_id.empty() || result.risk_confirmation_contract) {
            discard(result);
            return;
        }
        baseline.status = result_status(result.outcome);
        baseline.diagnostic_codes = std::move(result.diagnostic_codes);
        publish();
        return;
    }

    const RecommendationGoalParseResult parsed = parse_recommendation_goal_id(result.identity.goal_id);
    if (!parsed) {
        discard(result);
        return;
    }
    GoalResult& goal = m_snapshot.recommendation.goal_result(*parsed.goal);
    const bool ready_payload_valid = result.outcome != RecommendationTaskOutcome::Ready ||
                                     (!result.selected_candidate_id.empty() && result.evidence &&
                                      validate_recommendation_evidence(*result.evidence).empty());
    const bool terminal_payload_valid = result.outcome == RecommendationTaskOutcome::Ready ||
                                         (result.selected_candidate_id.empty() && !result.evidence &&
                                          !result.risk_confirmation_contract);
    bool risk_confirmation_contract_valid = true;
    if (result.outcome == RecommendationTaskOutcome::Ready &&
        result.risk_confirmation_contract) {
        if (!result.evidence) {
            risk_confirmation_contract_valid = false;
        } else {
            const RiskConfirmationPublicationIdentity expected_identity{
                result.identity.workflow_id,
                result.identity.attempt_id,
                result.identity.workspace_revision,
                *parsed.goal,
                result.identity.candidate_id,
                result.selected_candidate_id,
            };
            risk_confirmation_contract_valid =
                validate_owner_risk_confirmation_contract(
                    *result.risk_confirmation_contract, expected_identity, *result.evidence) ==
                RiskConfirmationContractValidationCode::None;
        }
    }
    if (result.identity.candidate_id != goal.candidate_id || terminal(goal.status) ||
        !valid_diagnostic_codes(result.diagnostic_codes) || !ready_payload_valid ||
        !terminal_payload_valid || !risk_confirmation_contract_valid) {
        discard(result);
        return;
    }
    goal.status = result_status(result.outcome);
    goal.diagnostic_codes = std::move(result.diagnostic_codes);
    if (result.outcome == RecommendationTaskOutcome::Ready)
        goal.evidence = std::move(result.evidence);
    else
        goal.evidence.reset();
    goal.selected_candidate_id = std::move(result.selected_candidate_id);
    goal.risk_confirmation_contract = std::move(result.risk_confirmation_contract);
    m_snapshot.state = RecommendationSessionState::Ready;
    publish();
}

void RecommendationSessionCoordinator::publish()
{
    ++m_snapshot.publication_revision;
    persist_journal();
    if (m_observer)
        m_observer(m_snapshot);
}

void RecommendationSessionCoordinator::discard(const RecommendationTaskResult& result)
{
    ++m_discarded_result_count;
    if (m_discard_handler)
        m_discard_handler(result);
}

bool RecommendationSessionCoordinator::request_cancellation(RecommendationCancellationReason reason)
{
    const std::shared_ptr<RecommendationCancellationToken> token = std::atomic_load(&m_cancellation_token);
    if (!token || !token->request(reason))
        return false;
    m_snapshot.cancellation_reason = reason;
    if (!m_cleanup_notified) {
        m_cleanup_notified = true;
        if (m_cleanup_handler)
            m_cleanup_handler(reason);
    }
    return true;
}

bool RecommendationSessionCoordinator::deadline_due() const
{
    const std::shared_ptr<RecommendationCancellationToken> token = std::atomic_load(&m_cancellation_token);
    if ((m_snapshot.state != RecommendationSessionState::Recommending &&
         m_snapshot.state != RecommendationSessionState::Ready) ||
        !active_work_remaining() ||
        (token && token->cancellation_requested()))
        return false;
    return m_monotonic_clock() - m_started_at >= std::chrono::minutes(10);
}

bool RecommendationSessionCoordinator::active_work_remaining() const
{
    if (m_snapshot.recommendation.baseline.status == GoalResultStatus::Analyzing)
        return true;
    return std::any_of(RECOMMENDATION_GOALS.begin(), RECOMMENDATION_GOALS.end(),
                       [this](RecommendationGoal goal) {
                           return m_snapshot.recommendation.goal_result(goal).status ==
                                  GoalResultStatus::Analyzing;
                       });
}

RecommendationSessionJournalRecord RecommendationSessionCoordinator::journal_record() const
{
    RecommendationSessionJournalRecord record;
    record.workflow_id = m_snapshot.workflow_id;
    record.attempt_id = m_snapshot.attempt_id;
    record.workspace_revision = m_snapshot.workspace_revision;
    record.baseline = {BASELINE_GOAL_ID,
                       m_snapshot.recommendation.baseline.status,
                       m_snapshot.recommendation.baseline.candidate_id,
                       journal_diagnostic_codes(m_snapshot.recommendation.baseline.diagnostic_codes)};
    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index) {
        const RecommendationGoal goal = RECOMMENDATION_GOALS[index];
        const GoalResult& result = m_snapshot.recommendation.goal_result(goal);
        record.goals[index] = {recommendation_goal_id(goal), result.status, result.candidate_id,
                               journal_diagnostic_codes(result.diagnostic_codes)};
    }
    record.contract_version = m_snapshot.recommendation.contract_version;
    record.strategy_version = m_snapshot.strategy_version;
    record.started_at_epoch_seconds = m_started_at_epoch_seconds;
    record.updated_at_epoch_seconds = std::max(m_started_at_epoch_seconds, m_wall_clock());
    return record;
}

void RecommendationSessionCoordinator::persist_journal()
{
    if (!m_journal)
        return;
    if (m_snapshot.state != RecommendationSessionState::Recommending &&
        m_snapshot.state != RecommendationSessionState::Ready) {
        clear_journal();
        return;
    }
    if (!active_work_remaining()) {
        clear_journal();
        return;
    }
    try {
        m_journal->save(journal_record());
    } catch (...) {
        ++m_journal_error_count;
    }
}

void RecommendationSessionCoordinator::clear_journal()
{
    if (!m_journal)
        return;
    try {
        m_journal->clear();
    } catch (...) {
        ++m_journal_error_count;
    }
}

} // namespace Slic3r::AI::SmartSlicing
