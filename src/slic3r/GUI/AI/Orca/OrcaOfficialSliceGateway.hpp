#pragma once

#include "slic3r/AI/SmartSlicing/Ports/IOfficialSliceGateway.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace Slic3r::GUI {

struct OrcaApplyMutationResult
{
    bool success{false};
    bool workspace_mutated{false};
    std::string diagnostic_code;
};

struct OrcaVersionedApplyResult
{
    bool success{false};
    bool workspace_mutated{false};
    bool rollback_attempted{false};
    bool rollback_succeeded{false};
    std::string diagnostic_code;
    std::optional<AI::SmartSlicing::OfficialApplyTransactionIdentity> transaction;
};

class OrcaOfficialSliceGateway final : public AI::SmartSlicing::IOfficialSliceGateway
{
public:
    using RevisionFn = std::function<AI::SmartSlicing::WorkspaceRevision()>;
    using CompatibilityFn = std::function<std::string(const AI::SmartSlicing::SliceCandidate&)>;
    using ApplyFn = std::function<OrcaApplyMutationResult(const AI::SmartSlicing::SliceCandidate&)>;
    using ApplyPlanFn = std::function<OrcaVersionedApplyResult(const AI::SmartSlicing::AtomicApplyPlan&)>;
    using ActionFn = std::function<bool()>;
    using OwnerThreadFn = std::function<bool()>;
    using VersionedUndoAvailableFn =
        std::function<bool(const AI::SmartSlicing::OfficialApplyTransactionIdentity&)>;
    using VersionedUndoFn =
        std::function<bool(const AI::SmartSlicing::OfficialApplyTransactionIdentity&, std::string&)>;

    OrcaOfficialSliceGateway(RevisionFn revision, CompatibilityFn compatibility, ApplyFn apply,
                             ActionFn start_slice, ActionFn show_preview, ActionFn undo,
                             ApplyPlanFn apply_plan = {}, OwnerThreadFn owner_thread = {},
                             VersionedUndoAvailableFn versioned_undo_available = {},
                             VersionedUndoFn versioned_undo = {})
        : m_revision(std::move(revision))
        , m_compatibility(std::move(compatibility))
        , m_apply(std::move(apply))
        , m_start_slice(std::move(start_slice))
        , m_show_preview(std::move(show_preview))
        , m_undo(std::move(undo))
        , m_apply_plan(std::move(apply_plan))
        , m_owner_thread(std::move(owner_thread))
        , m_versioned_undo_available_fn(std::move(versioned_undo_available))
        , m_versioned_undo(std::move(versioned_undo))
    {}

    AI::SmartSlicing::OfficialSliceResult
    prepare(const AI::SmartSlicing::SliceCandidate& candidate,
            const AI::SmartSlicing::WorkspaceRevision& expected_revision) override
    {
        if (!revision_matches(candidate, expected_revision))
            return rejected("stale_revision");
        std::string diagnostic;
        try {
            diagnostic = m_compatibility ? m_compatibility(candidate) : "compatibility_check_unavailable";
        } catch (...) {
            return rejected("compatibility_check_failed");
        }
        if (!diagnostic.empty())
            return rejected(diagnostic);
        return {AI::SmartSlicing::OfficialSlicePhase::Prepared, {}, false, false};
    }

    AI::SmartSlicing::OfficialSliceResult
    commit(const AI::SmartSlicing::SliceCandidate& candidate,
           const AI::SmartSlicing::WorkspaceRevision& expected_revision) override
    {
        if (m_versioned_transaction)
            return rejected("official_transaction_active");
        if (!revision_matches(candidate, expected_revision))
            return rejected("stale_revision");
        m_pending = false;
        m_preview_shown = false;
        m_workspace_mutated = false;
        m_can_undo = false;
        m_applied_revision.reset();
        try {
            OrcaApplyMutationResult applied = m_apply ? m_apply(candidate) : OrcaApplyMutationResult{};
            m_workspace_mutated = applied.workspace_mutated;
            m_can_undo          = applied.workspace_mutated;
            if (m_can_undo && m_revision)
                m_applied_revision = m_revision();
            if (!applied.success) {
                m_last = {AI::SmartSlicing::OfficialSlicePhase::Failed,
                          applied.diagnostic_code.empty() ? "candidate_apply_failed" : applied.diagnostic_code,
                          m_workspace_mutated, m_can_undo};
                return m_last;
            }
            if (!m_start_slice || !m_start_slice()) {
                m_last = {AI::SmartSlicing::OfficialSlicePhase::Failed, "official_slice_not_started",
                          m_workspace_mutated, m_can_undo};
                return m_last;
            }
            m_pending = true;
            m_last = {AI::SmartSlicing::OfficialSlicePhase::Slicing, {}, m_workspace_mutated, m_can_undo};
            return m_last;
        } catch (...) {
            if (!m_applied_revision)
                m_can_undo = false;
            m_last = {AI::SmartSlicing::OfficialSlicePhase::Failed, "candidate_apply_exception",
                      m_workspace_mutated, m_can_undo};
            return m_last;
        }
    }

    AI::SmartSlicing::OfficialSliceResult
    commit_plan(const AI::SmartSlicing::AtomicApplyPlan& plan) override
    {
        using namespace AI::SmartSlicing;
        if (!m_owner_thread || !m_owner_thread())
            return rejected("versioned_apply_requires_owner_thread");
        const bool consumed_plan_or_token =
            m_consumed_plan_ids.find(plan.plan_id) != m_consumed_plan_ids.end() ||
            m_consumed_confirmation_token_ids.find(plan.confirmation_token_id) !=
                m_consumed_confirmation_token_ids.end();
        const bool consumed_payload_matches =
            consumed_plan_or_token && m_consumed_candidate_payload_digest &&
            *m_consumed_candidate_payload_digest == apply_candidate_payload_digest(plan.candidate);
        if ((m_versioned_action_in_progress || m_versioned_transaction || m_pending) &&
            !consumed_payload_matches)
            return rejected("official_transaction_active");
        if (consumed_plan_or_token)
            return rejected("apply_plan_already_consumed");
        const std::string contract_error = validate_plan_contract(plan);
        if (!contract_error.empty())
            return rejected(contract_error);
        if (!revision_matches(plan.candidate, plan.binding.base_revision))
            return rejected("stale_revision");
        std::string diagnostic;
        try {
            diagnostic = m_compatibility ? m_compatibility(plan.candidate) :
                                           "compatibility_check_unavailable";
        } catch (...) {
            return rejected("compatibility_check_failed");
        }
        if (!diagnostic.empty())
            return rejected(std::move(diagnostic));
        if (!m_apply_plan)
            return rejected("versioned_apply_transaction_unavailable");

        m_consumed_plan_ids.insert(plan.plan_id);
        m_consumed_confirmation_token_ids.insert(plan.confirmation_token_id);
        m_consumed_candidate_payload_digest = apply_candidate_payload_digest(plan.candidate);
        m_versioned_action_in_progress = true;
        struct TransactionReset
        {
            bool& active;
            ~TransactionReset() { active = false; }
        } reset{m_versioned_action_in_progress};

        try {
            OrcaVersionedApplyResult applied = m_apply_plan(plan);
            if (!applied.success) {
                const bool state_unknown = applied.workspace_mutated ||
                    (applied.rollback_attempted && !applied.rollback_succeeded);
                return {OfficialSlicePhase::Failed,
                        applied.diagnostic_code.empty() ?
                            (state_unknown ? "versioned_apply_rollback_failed" : "versioned_apply_failed") :
                            std::move(applied.diagnostic_code),
                        state_unknown, false, std::move(applied.transaction)};
            }
            if (!applied.workspace_mutated || !applied.transaction ||
                !transaction_matches_plan(*applied.transaction, plan) ||
                !current_revision_matches(applied.transaction->applied_revision))
                return {OfficialSlicePhase::Failed, "versioned_apply_result_invalid",
                        applied.workspace_mutated, false, std::move(applied.transaction)};
            m_versioned_transaction = *applied.transaction;
            m_versioned_base_revision = plan.binding.base_revision;
            m_versioned_can_retry = false;
            m_versioned_can_print = false;
            m_versioned_preview_attempted = false;
            m_last = versioned_result(OfficialSlicePhase::Applied, {});
            return m_last;
        } catch (...) {
            return {OfficialSlicePhase::Failed, "versioned_apply_exception_state_unknown", true, false};
        }
    }

    AI::SmartSlicing::OfficialSliceResult start_committed_plan_slice(
        const AI::SmartSlicing::OfficialApplyTransactionIdentity& transaction) override
    {
        if (!matches_versioned(transaction) || m_last.phase != AI::SmartSlicing::OfficialSlicePhase::Applied)
            return versioned_rejected("official_slice_not_started");
        return start_versioned_slice(false);
    }

    AI::SmartSlicing::OfficialSliceResult retry_official_slice(
        const AI::SmartSlicing::OfficialApplyTransactionIdentity& transaction) override
    {
        if (!matches_versioned(transaction) ||
            m_last.phase != AI::SmartSlicing::OfficialSlicePhase::Failed ||
            !m_versioned_can_retry)
            return versioned_rejected("slice_retry_not_allowed");
        return start_versioned_slice(true);
    }

    AI::SmartSlicing::OfficialSliceResult complete_official_slice(
        const AI::SmartSlicing::OfficialApplyTransactionIdentity& transaction, bool success,
        std::string diagnostic_code = {}) override
    {
        using namespace AI::SmartSlicing;
        if (!matches_versioned(transaction) || !m_versioned_slice_pending)
            return versioned_rejected("versioned_official_slice_not_active");
        m_versioned_slice_pending = false;
        if (!current_revision_matches(transaction.applied_revision)) {
            m_versioned_can_retry = false;
            m_last = versioned_result(OfficialSlicePhase::Failed, "workspace_changed");
            return m_last;
        }
        if (!success) {
            m_versioned_can_retry = true;
            m_last = versioned_result(
                OfficialSlicePhase::Failed,
                diagnostic_code.empty() ? "official_slice_failed" : std::move(diagnostic_code));
            return m_last;
        }
        m_versioned_can_retry = false;
        m_last = versioned_result(OfficialSlicePhase::Completed, {});
        return m_last;
    }

    AI::SmartSlicing::OfficialSliceResult poll_committed_plan(
        const AI::SmartSlicing::OfficialApplyTransactionIdentity& transaction) override
    {
        using namespace AI::SmartSlicing;
        if (!matches_versioned(transaction))
            return versioned_rejected("versioned_official_slice_not_active");
        if (!current_revision_matches(transaction.applied_revision)) {
            m_versioned_can_retry = false;
            m_versioned_can_print = false;
            m_last = versioned_result(OfficialSlicePhase::Failed, "workspace_changed");
            return m_last;
        }
        if (m_last.phase == OfficialSlicePhase::Completed && !m_versioned_preview_attempted) {
            m_versioned_preview_attempted = true;
            bool shown = false;
            try {
                shown = m_show_preview && m_show_preview();
            } catch (...) {
                shown = false;
            }
            if (!shown) {
                m_versioned_can_retry = true;
                m_versioned_can_print = false;
                m_last = versioned_result(OfficialSlicePhase::Failed, "preview_navigation_failed");
            } else {
                m_versioned_can_print = true;
                m_last = versioned_result(OfficialSlicePhase::Completed, {});
            }
        }
        return m_last;
    }

    AI::SmartSlicing::OfficialSliceResult undo_committed_plan(
        const AI::SmartSlicing::OfficialApplyTransactionIdentity& transaction) override
    {
        using namespace AI::SmartSlicing;
        if (!matches_versioned(transaction) || m_versioned_slice_pending || !m_versioned_undo ||
            !current_revision_matches(transaction.applied_revision) ||
            !versioned_undo_available(transaction))
            return versioned_rejected("apply_undo_unavailable");

        std::string diagnostic;
        bool undone = false;
        m_versioned_action_in_progress = true;
        try {
            undone = m_versioned_undo && m_versioned_undo(transaction, diagnostic);
        } catch (...) {
            diagnostic = "apply_undo_failed";
        }
        m_versioned_action_in_progress = false;
        if (!undone || !m_versioned_base_revision ||
            !current_revision_matches(*m_versioned_base_revision)) {
            m_versioned_can_retry = false;
            m_versioned_can_print = false;
            m_last = versioned_result(OfficialSlicePhase::Failed,
                diagnostic.empty() ? "apply_undo_failed" : std::move(diagnostic));
            return m_last;
        }

        m_versioned_transaction.reset();
        m_versioned_base_revision.reset();
        m_versioned_can_retry = false;
        m_versioned_can_print = false;
        m_versioned_preview_attempted = false;
        m_versioned_slice_pending = false;
        m_last = {OfficialSlicePhase::Prepared, "apply_undone", false, false};
        return m_last;
    }

    AI::SmartSlicing::OfficialSliceResult poll() override
    {
        if (m_versioned_transaction)
            return rejected("official_transaction_active");
        if (m_last.phase == AI::SmartSlicing::OfficialSlicePhase::Completed && !m_preview_shown) {
            m_preview_shown = true;
            if (!m_show_preview || !m_show_preview())
                m_last = {AI::SmartSlicing::OfficialSlicePhase::Failed, "preview_navigation_failed",
                          m_workspace_mutated, m_can_undo};
        }
        return m_last;
    }

    bool retire_committed_plan(const AI::SmartSlicing::OfficialApplyTransactionIdentity& transaction) override
    {
        if (!m_owner_thread || !m_owner_thread() || !matches_versioned(transaction) ||
            m_versioned_slice_pending || m_versioned_action_in_progress ||
            (m_last.phase != AI::SmartSlicing::OfficialSlicePhase::Completed &&
             m_last.phase != AI::SmartSlicing::OfficialSlicePhase::Failed)) return false;
        // The project history and consumed command identities outlive this UI session.
        m_versioned_transaction.reset();
        m_versioned_base_revision.reset();
        m_versioned_can_retry = false;
        m_versioned_can_print = false;
        m_versioned_preview_attempted = false;
        m_last = {};
        return true;
    }

    bool undo_last_apply() override
    {
        if (m_versioned_transaction)
            return false;
        if (!m_can_undo || !m_applied_revision || !m_revision ||
            m_revision() != *m_applied_revision || !m_undo || !m_undo())
            return false;
        m_can_undo = false;
        m_workspace_mutated = false;
        m_pending = false;
        m_preview_shown = false;
        m_last = {AI::SmartSlicing::OfficialSlicePhase::Prepared, "apply_undone", false, false};
        return true;
    }

    void notify_slice_completed(bool success, std::string diagnostic_code = {})
    {
        if (m_versioned_transaction)
            return;
        if (!m_pending)
            return;
        m_pending = false;
        m_last = {success ? AI::SmartSlicing::OfficialSlicePhase::Completed :
                            AI::SmartSlicing::OfficialSlicePhase::Failed,
                  success ? std::string{} : (diagnostic_code.empty() ? "official_slice_failed" : std::move(diagnostic_code)),
                  m_workspace_mutated, m_can_undo};
    }

private:
    AI::SmartSlicing::OfficialSliceResult start_versioned_slice(bool retry)
    {
        using namespace AI::SmartSlicing;
        if (!m_versioned_transaction || m_versioned_action_in_progress ||
            !current_revision_matches(m_versioned_transaction->applied_revision)) {
            m_versioned_can_retry = false;
            return versioned_rejected(retry ? "slice_retry_not_allowed" : "official_slice_not_started");
        }

        m_versioned_action_in_progress = true;
        bool started = false;
        try {
            started = m_start_slice && m_start_slice();
        } catch (...) {
            started = false;
        }
        m_versioned_action_in_progress = false;
        m_versioned_can_print = false;
        m_versioned_preview_attempted = false;
        if (!started) {
            m_versioned_slice_pending = false;
            m_versioned_can_retry = true;
            m_last = versioned_result(OfficialSlicePhase::Failed, "official_slice_not_started");
            return m_last;
        }

        m_versioned_slice_pending = true;
        m_versioned_can_retry = false;
        m_last = versioned_result(OfficialSlicePhase::Slicing, {});
        return m_last;
    }

    bool matches_versioned(
        const AI::SmartSlicing::OfficialApplyTransactionIdentity& transaction) const
    {
        return m_versioned_transaction && transaction == *m_versioned_transaction;
    }

    bool versioned_undo_available(
        const AI::SmartSlicing::OfficialApplyTransactionIdentity& transaction) const
    {
        try {
            return !m_versioned_slice_pending &&
                   (!m_versioned_undo_available_fn || m_versioned_undo_available_fn(transaction));
        } catch (...) {
            return false;
        }
    }

    AI::SmartSlicing::OfficialSliceResult versioned_result(
        AI::SmartSlicing::OfficialSlicePhase phase, std::string diagnostic_code) const
    {
        const bool can_undo = m_versioned_transaction &&
            current_revision_matches(m_versioned_transaction->applied_revision) &&
            versioned_undo_available(*m_versioned_transaction);
        return {phase, std::move(diagnostic_code), true, can_undo, m_versioned_transaction,
                m_versioned_can_retry, m_versioned_can_print};
    }

    AI::SmartSlicing::OfficialSliceResult versioned_rejected(std::string diagnostic_code) const
    {
        AI::SmartSlicing::OfficialSliceResult result = versioned_result(
            AI::SmartSlicing::OfficialSlicePhase::Rejected, std::move(diagnostic_code));
        result.can_retry_slice = false;
        result.can_print = false;
        return result;
    }

    static std::string validate_plan_contract(const AI::SmartSlicing::AtomicApplyPlan& plan)
    {
        using namespace AI::SmartSlicing;
        if (plan.schema != APPLY_PLAN_SCHEMA || plan.version != APPLY_PLAN_VERSION)
            return "apply_plan_contract_invalid";
        if (plan.plan_id.empty() || plan.confirmation_token_id.empty() ||
            plan.binding.workflow_id == 0 || plan.binding.attempt_id == 0 ||
            !plan.binding.base_revision.valid() || plan.binding.goal_task_candidate_id.empty() ||
            plan.binding.selected_candidate_id.empty() || plan.binding.strategy_version.empty() ||
            plan.binding.selection_policy_version.empty() || plan.binding.parameter_policy_version.empty() ||
            plan.binding.risk_evidence_digest.empty() ||
            plan.binding.risk_confirmation_policy_version.empty() ||
            plan.binding.risk_confirmation_evidence_digest.empty() ||
            plan.binding.intent_evidence_revision == 0 ||
            recommendation_goal_id(plan.binding.goal)[0] == '\0' ||
            plan.binding.machine_registry_version.empty() || plan.binding.material_registry_version.empty() ||
            plan.binding.machine_capability_evidence_digest.empty() ||
            plan.binding.material_compatibility_evidence_digest.empty() ||
            plan.binding.candidate_payload_digest.empty())
            return "apply_plan_binding_invalid";
        if (plan.candidate.id != plan.binding.selected_candidate_id ||
            plan.candidate.base_revision != plan.binding.base_revision ||
            !candidate_goal_matches(plan.candidate.goal, plan.binding.goal) ||
            recommendation_goal_id(plan.candidate.parameters.goal)[0] == '\0' ||
            plan.candidate.parameters.goal != plan.binding.goal ||
            plan.candidate.parameters.policy_version != plan.binding.parameter_policy_version)
            return "apply_plan_candidate_identity_mismatch";
        if (plan.candidate.status != CandidateStatus::Ready ||
            plan.candidate.repair.has_value() ||
            (plan.candidate.placement.transforms.empty() && plan.candidate.parameters.entries.empty()))
            return "apply_plan_candidate_invalid";
        if (apply_candidate_payload_digest(plan.candidate) != plan.binding.candidate_payload_digest)
            return "apply_plan_candidate_digest_mismatch";
        std::unordered_set<int> confirmations;
        for (const RiskConfirmationKind confirmation : plan.binding.required_confirmations) {
            if (std::string(risk_confirmation_kind_name(confirmation)) == "unknown" ||
                !confirmations.insert(static_cast<int>(confirmation)).second)
                return "apply_plan_binding_invalid";
        }
        return {};
    }

    static bool candidate_goal_matches(AI::SmartSlicing::CandidateGoal candidate_goal,
                                       AI::SmartSlicing::RecommendationGoal recommendation_goal)
    {
        using namespace AI::SmartSlicing;
        switch (candidate_goal) {
        case CandidateGoal::Stability: return recommendation_goal == RecommendationGoal::Balanced;
        case CandidateGoal::Speed: return recommendation_goal == RecommendationGoal::Speed;
        case CandidateGoal::Quality: return recommendation_goal == RecommendationGoal::Quality;
        case CandidateGoal::MaterialSaving: return false;
        }
        return false;
    }

    static bool transaction_matches_plan(
        const AI::SmartSlicing::OfficialApplyTransactionIdentity& transaction,
        const AI::SmartSlicing::AtomicApplyPlan& plan)
    {
        return transaction.valid() && transaction.plan_id == plan.plan_id &&
               transaction.candidate_id == plan.candidate.id &&
               transaction.workflow_id == plan.binding.workflow_id &&
               transaction.attempt_id == plan.binding.attempt_id &&
               transaction.applied_revision != plan.binding.base_revision;
    }

    bool current_revision_matches(const AI::SmartSlicing::WorkspaceRevision& expected) const
    {
        try {
            return m_revision && m_revision() == expected;
        } catch (...) {
            return false;
        }
    }

    bool revision_matches(const AI::SmartSlicing::SliceCandidate& candidate,
                          const AI::SmartSlicing::WorkspaceRevision& expected_revision) const
    {
        try {
            return candidate.base_revision == expected_revision && m_revision && m_revision() == expected_revision;
        } catch (...) {
            return false;
        }
    }

    static AI::SmartSlicing::OfficialSliceResult rejected(std::string diagnostic_code)
    {
        return {AI::SmartSlicing::OfficialSlicePhase::Rejected, std::move(diagnostic_code), false, false};
    }

    RevisionFn m_revision;
    CompatibilityFn m_compatibility;
    ApplyFn m_apply;
    ActionFn m_start_slice;
    ActionFn m_show_preview;
    ActionFn m_undo;
    ApplyPlanFn m_apply_plan;
    OwnerThreadFn m_owner_thread;
    VersionedUndoAvailableFn m_versioned_undo_available_fn;
    VersionedUndoFn m_versioned_undo;
    AI::SmartSlicing::OfficialSliceResult m_last;
    std::optional<AI::SmartSlicing::WorkspaceRevision> m_applied_revision;
    bool m_pending{false};
    bool m_workspace_mutated{false};
    bool m_can_undo{false};
    bool m_preview_shown{false};
    std::optional<AI::SmartSlicing::OfficialApplyTransactionIdentity> m_versioned_transaction;
    std::optional<AI::SmartSlicing::WorkspaceRevision> m_versioned_base_revision;
    bool m_versioned_action_in_progress{false};
    bool m_versioned_slice_pending{false};
    bool m_versioned_can_retry{false};
    bool m_versioned_can_print{false};
    bool m_versioned_preview_attempted{false};
    std::unordered_set<std::string> m_consumed_plan_ids;
    std::unordered_set<std::string> m_consumed_confirmation_token_ids;
    std::optional<std::string> m_consumed_candidate_payload_digest;
};

} // namespace Slic3r::GUI
