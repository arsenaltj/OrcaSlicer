#include "VersionedApplyWorkflow.hpp"

#include <utility>

namespace Slic3r::AI::SmartSlicing {

OfficialSliceResult VersionedApplyWorkflow::start(const VersionedApplyRequest& request,
                                                   const ApplyExpectedContext& current)
{
    if (m_active_transaction)
        return rejected("official_transaction_active");

    const ReadyApplyBindingResult binding = m_apply_service.capture_ready_binding(current);
    if (!binding.accepted())
        return rejected(apply_rejection_code_name(binding.rejection));

    const RiskConfirmationTokenResult token = m_apply_service.issue_confirmation_token(
        *binding.binding, current, request.confirmation);
    if (!token.accepted())
        return rejected(apply_rejection_code_name(token.rejection));

    ApplyCommand command;
    command.command_id = request.command_id;
    command.binding = *binding.binding;
    command.confirmation_token = token.token;
    ApplyGuardResult guarded = m_apply_service.make_atomic_plan(
        command, *binding.binding, current);
    if (!guarded.accepted())
        return rejected(apply_rejection_code_name(guarded.rejection));

    OfficialSliceResult applied = m_gateway.commit_plan(*guarded.plan);
    if (applied.phase != OfficialSlicePhase::Applied || !applied.apply_transaction ||
        !applied.apply_transaction->valid() ||
        applied.apply_transaction->workflow_id != binding.binding->workflow_id ||
        applied.apply_transaction->attempt_id != binding.binding->attempt_id ||
        applied.apply_transaction->plan_id != guarded.plan->plan_id ||
        applied.apply_transaction->candidate_id != guarded.plan->candidate.id) {
        return applied.phase == OfficialSlicePhase::Applied ?
            rejected("versioned_apply_result_invalid") : applied;
    }

    m_active_transaction = applied.apply_transaction;
    return m_gateway.start_committed_plan_slice(*m_active_transaction);
}

OfficialSliceResult VersionedApplyWorkflow::retry(
    const OfficialApplyTransactionIdentity& transaction)
{
    if (!matches_active(transaction))
        return rejected("slice_retry_not_allowed");
    return m_gateway.retry_official_slice(transaction);
}

OfficialSliceResult VersionedApplyWorkflow::poll()
{
    if (!m_active_transaction)
        return rejected("versioned_official_slice_not_active");
    return m_gateway.poll_committed_plan(*m_active_transaction);
}

OfficialSliceResult VersionedApplyWorkflow::notify_slice_completed(
    bool success, std::string diagnostic_code)
{
    if (!m_active_transaction)
        return rejected("versioned_official_slice_not_active");
    return m_gateway.complete_official_slice(
        *m_active_transaction, success, std::move(diagnostic_code));
}

OfficialSliceResult VersionedApplyWorkflow::undo(
    const OfficialApplyTransactionIdentity& transaction)
{
    if (!matches_active(transaction))
        return rejected("apply_undo_unavailable");
    OfficialSliceResult result = m_gateway.undo_committed_plan(transaction);
    if (result.diagnostic_code == "apply_undone" && !result.workspace_mutated)
        m_active_transaction.reset();
    return result;
}

OfficialSliceResult VersionedApplyWorkflow::rejected(std::string diagnostic_code)
{
    return {OfficialSlicePhase::Rejected, std::move(diagnostic_code), false, false};
}

bool VersionedApplyWorkflow::matches_active(
    const OfficialApplyTransactionIdentity& transaction) const
{
    return m_active_transaction && transaction == *m_active_transaction;
}

} // namespace Slic3r::AI::SmartSlicing
