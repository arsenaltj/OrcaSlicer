#pragma once

#include "slic3r/AI/SmartSlicing/Ports/IOfficialSliceGateway.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace Slic3r::GUI {

class Plater;

struct OrcaApplyMutationResult
{
    bool success{false};
    bool workspace_mutated{false};
    std::string diagnostic_code;
};

class OrcaOfficialSliceGateway final : public AI::SmartSlicing::IOfficialSliceGateway
{
public:
    using RevisionFn = std::function<AI::SmartSlicing::WorkspaceRevision()>;
    using CompatibilityFn = std::function<std::string(const AI::SmartSlicing::SliceCandidate&)>;
    using ApplyFn = std::function<OrcaApplyMutationResult(const AI::SmartSlicing::SliceCandidate&)>;
    using ActionFn = std::function<bool()>;

    OrcaOfficialSliceGateway(RevisionFn revision, CompatibilityFn compatibility, ApplyFn apply,
                             ActionFn start_slice, ActionFn show_preview, ActionFn undo)
        : m_revision(std::move(revision))
        , m_compatibility(std::move(compatibility))
        , m_apply(std::move(apply))
        , m_start_slice(std::move(start_slice))
        , m_show_preview(std::move(show_preview))
        , m_undo(std::move(undo))
    {}

    OrcaOfficialSliceGateway(Plater& plater, RevisionFn revision, ActionFn start_slice);

    AI::SmartSlicing::OfficialSliceResult
    prepare(const AI::SmartSlicing::SliceCandidate& candidate,
            const AI::SmartSlicing::WorkspaceRevision& expected_revision) override
    {
        if (m_pending)
            return rejected("official_slice_in_progress");
        if (has_pending_apply_recovery())
            return rejected("apply_recovery_required");
        clear_preparation();
        if (candidate.id.empty() || candidate.workflow_id == 0)
            return rejected("invalid_candidate_identity");
        if (!candidate.base_revision.valid() || !expected_revision.valid())
            return rejected("invalid_workspace_revision");
        if (candidate.status != AI::SmartSlicing::CandidateStatus::Ready)
            return rejected("candidate_not_ready");
        if (!revision_matches(candidate, expected_revision))
            return rejected("stale_revision");
        if (candidate.repair)
            return rejected("candidate_repair_unsupported");
        std::string diagnostic;
        try {
            diagnostic = m_compatibility ? m_compatibility(candidate) : "compatibility_check_unavailable";
        } catch (...) {
            return rejected("compatibility_check_failed");
        }
        if (!diagnostic.empty())
            return rejected(diagnostic);
        m_prepared_candidate = PreparedCandidateToken{
            candidate.id, candidate.workflow_id, expected_revision, candidate.placement, candidate.parameters};
        return {AI::SmartSlicing::OfficialSlicePhase::Prepared, {}, false, false};
    }

    AI::SmartSlicing::OfficialSliceResult
    commit(const AI::SmartSlicing::SliceCandidate& candidate,
           const AI::SmartSlicing::WorkspaceRevision& expected_revision) override
    {
        if (m_pending)
            return rejected("official_slice_in_progress");
        if (has_pending_apply_recovery())
            return rejected("apply_recovery_required");
        if (candidate.id.empty() || candidate.workflow_id == 0) {
            clear_preparation();
            return rejected("invalid_candidate_identity");
        }
        if (!candidate.base_revision.valid() || !expected_revision.valid()) {
            clear_preparation();
            return rejected("invalid_workspace_revision");
        }
        if (candidate.status != AI::SmartSlicing::CandidateStatus::Ready) {
            clear_preparation();
            return rejected("candidate_not_ready");
        }
        if (!revision_matches(candidate, expected_revision)) {
            clear_preparation();
            return rejected("stale_revision");
        }
        if (candidate.repair) {
            clear_preparation();
            return rejected("candidate_repair_unsupported");
        }
        const bool prepared = m_prepared_candidate &&
                              prepared_candidate_matches(*m_prepared_candidate, candidate, expected_revision);
        clear_preparation();
        if (!prepared)
            return rejected("candidate_not_prepared");
        try {
            const std::string diagnostic =
                m_compatibility ? m_compatibility(candidate) : "compatibility_check_unavailable";
            if (!diagnostic.empty())
                return rejected(diagnostic);
        } catch (...) {
            return rejected("compatibility_check_failed");
        }
        m_pending = false;
        m_preview_shown = false;
        m_workspace_mutated = false;
        m_can_undo = false;
        m_undo_revision.reset();
        m_official_slice_revision.reset();
        try {
            OrcaApplyMutationResult applied = m_apply ? m_apply(candidate) : OrcaApplyMutationResult{};
            m_workspace_mutated = applied.workspace_mutated;
            capture_undo_revision();
            if (!applied.success) {
                m_last = {AI::SmartSlicing::OfficialSlicePhase::Failed,
                          applied.diagnostic_code.empty() ? "candidate_apply_failed" : applied.diagnostic_code,
                          m_workspace_mutated, m_can_undo};
                return m_last;
            }
            const bool slice_started = m_start_slice && m_start_slice();
            capture_undo_revision();
            if (!slice_started) {
                m_last = {AI::SmartSlicing::OfficialSlicePhase::Failed, "official_slice_not_started",
                          m_workspace_mutated, m_can_undo};
                return m_last;
            }
            capture_official_slice_revision();
            m_pending = true;
            m_last = {AI::SmartSlicing::OfficialSlicePhase::Slicing, {}, m_workspace_mutated, m_can_undo};
            return m_last;
        } catch (...) {
            m_last = {AI::SmartSlicing::OfficialSlicePhase::Failed, "candidate_apply_exception",
                      m_workspace_mutated, m_can_undo};
            return m_last;
        }
    }

    AI::SmartSlicing::OfficialSliceResult poll() override
    {
        if (m_last.phase == AI::SmartSlicing::OfficialSlicePhase::Completed && !m_preview_shown) {
            m_preview_shown = true;
            if (!m_show_preview || !m_show_preview())
                m_last = {AI::SmartSlicing::OfficialSlicePhase::Failed, "preview_navigation_failed",
                          m_workspace_mutated, m_can_undo};
        }
        return m_last;
    }

    bool undo_last_apply() override
    {
        if (!m_can_undo)
            return false;
        if (!undo_revision_matches()) {
            disable_undo_recovery();
            return false;
        }
        bool undone = false;
        try {
            undone = m_undo && m_undo();
        } catch (...) {
            // Native recovery is one-shot; failure is projected below without escaping the gateway.
        }
        if (!undone) {
            disable_undo_recovery();
            return false;
        }
        disable_undo_recovery();
        m_workspace_mutated = false;
        m_pending = false;
        m_preview_shown = false;
        m_last = {AI::SmartSlicing::OfficialSlicePhase::Prepared, "apply_undone", false, false};
        return true;
    }

    void notify_slice_completed(bool success, std::string diagnostic_code = {})
    {
        if (!m_pending)
            return;
        m_pending = false;
        const OfficialSliceRevisionStatus revision_status = official_slice_revision_status();
        m_official_slice_revision.reset();
        if (revision_status != OfficialSliceRevisionStatus::Matches) {
            disable_undo_recovery();
            m_last = {AI::SmartSlicing::OfficialSlicePhase::Failed,
                      revision_status == OfficialSliceRevisionStatus::Changed ?
                          "official_slice_revision_changed" : "official_slice_revision_unavailable",
                      m_workspace_mutated, false};
            return;
        }
        m_last = {success ? AI::SmartSlicing::OfficialSlicePhase::Completed :
                            AI::SmartSlicing::OfficialSlicePhase::Failed,
                  success ? std::string{} : (diagnostic_code.empty() ? "official_slice_failed" : std::move(diagnostic_code)),
                  m_workspace_mutated, m_can_undo};
    }

private:
    enum class OfficialSliceRevisionStatus { Matches, Changed, Unavailable };

    bool has_pending_apply_recovery() const noexcept
    {
        return m_last.phase == AI::SmartSlicing::OfficialSlicePhase::Failed && m_can_undo;
    }

    struct PreparedCandidateToken
    {
        AI::SmartSlicing::CandidateId id;
        AI::SmartSlicing::WorkflowId workflow_id{0};
        AI::SmartSlicing::WorkspaceRevision revision;
        AI::SmartSlicing::PlacementCandidate placement;
        AI::SmartSlicing::ParameterProposal parameters;
    };

    static bool prepared_candidate_matches(const PreparedCandidateToken& prepared,
                                           const AI::SmartSlicing::SliceCandidate& candidate,
                                           const AI::SmartSlicing::WorkspaceRevision& expected_revision)
    {
        if (prepared.id != candidate.id || prepared.workflow_id != candidate.workflow_id ||
            prepared.revision != expected_revision ||
            prepared.revision != candidate.base_revision ||
            prepared.placement.transforms.size() != candidate.placement.transforms.size() ||
            prepared.parameters.intent != candidate.parameters.intent ||
            prepared.parameters.entries.size() != candidate.parameters.entries.size() ||
            prepared.parameters.explanation_codes != candidate.parameters.explanation_codes)
            return false;
        for (size_t index = 0; index < prepared.placement.transforms.size(); ++index) {
            const auto& lhs = prepared.placement.transforms[index];
            const auto& rhs = candidate.placement.transforms[index];
            if (lhs.object_id != rhs.object_id || lhs.instance_id != rhs.instance_id || lhs.matrix != rhs.matrix)
                return false;
        }
        for (size_t index = 0; index < prepared.parameters.entries.size(); ++index) {
            const auto& lhs = prepared.parameters.entries[index];
            const auto& rhs = candidate.parameters.entries[index];
            if (lhs.scope != rhs.scope || lhs.owner != rhs.owner || lhs.target_id != rhs.target_id ||
                lhs.key != rhs.key || lhs.expected_value != rhs.expected_value || lhs.new_value != rhs.new_value ||
                lhs.reason_code != rhs.reason_code)
                return false;
        }
        return true;
    }

    void clear_preparation() noexcept
    {
        m_prepared_candidate.reset();
    }

    void disable_undo_recovery() noexcept
    {
        m_can_undo = false;
        m_undo_revision.reset();
        m_last.can_undo = false;
    }

    void capture_undo_revision() noexcept
    {
        if (!m_workspace_mutated || !m_revision) {
            m_can_undo = false;
            m_undo_revision.reset();
            return;
        }
        try {
            AI::SmartSlicing::WorkspaceRevision revision = m_revision();
            if (revision.valid()) {
                m_undo_revision = std::move(revision);
                m_can_undo = true;
            }
        } catch (...) {
        }
    }

    void capture_official_slice_revision() noexcept
    {
        m_official_slice_revision.reset();
        if (!m_revision)
            return;
        try {
            AI::SmartSlicing::WorkspaceRevision revision = m_revision();
            if (revision.valid())
                m_official_slice_revision = std::move(revision);
        } catch (...) {
        }
    }

    OfficialSliceRevisionStatus official_slice_revision_status() noexcept
    {
        if (!m_official_slice_revision || !m_revision)
            return OfficialSliceRevisionStatus::Unavailable;
        try {
            const AI::SmartSlicing::WorkspaceRevision current = m_revision();
            if (!current.valid())
                return OfficialSliceRevisionStatus::Unavailable;
            return current == *m_official_slice_revision ? OfficialSliceRevisionStatus::Matches :
                                                           OfficialSliceRevisionStatus::Changed;
        } catch (...) {
            return OfficialSliceRevisionStatus::Unavailable;
        }
    }

    bool undo_revision_matches() noexcept
    {
        if (!m_undo_revision || !m_revision) {
            m_can_undo = false;
            m_last.can_undo = false;
            return false;
        }
        try {
            if (m_revision() == *m_undo_revision)
                return true;
        } catch (...) {
        }
        m_can_undo = false;
        m_last.can_undo = false;
        return false;
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
    AI::SmartSlicing::OfficialSliceResult m_last;
    bool m_pending{false};
    bool m_workspace_mutated{false};
    bool m_can_undo{false};
    bool m_preview_shown{false};
    std::optional<PreparedCandidateToken> m_prepared_candidate;
    std::optional<AI::SmartSlicing::WorkspaceRevision> m_undo_revision;
    std::optional<AI::SmartSlicing::WorkspaceRevision> m_official_slice_revision;
};

} // namespace Slic3r::GUI
