#pragma once

#include "slic3r/AI/SmartSlicing/Application/RecommendationSessionCoordinator.hpp"
#include "slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp"
#include "slic3r/AI/SmartSlicing/Domain/TrialMetrics.hpp"
#include "slic3r/AI/SmartSlicing/Domain/PrintabilityReport.hpp"
#include "slic3r/AI/SmartSlicing/Ports/IOfficialSliceGateway.hpp"

#include <array>
#include <algorithm>
#include <functional>
#include <optional>

namespace Slic3r::GUI {

inline std::vector<AI::SmartSlicing::ConfigPatchEntry> effective_candidate_parameters(
    std::vector<AI::SmartSlicing::ConfigPatchEntry> baseline,
    const AI::SmartSlicing::ParameterProposal& proposal)
{
    for (const auto& patch : proposal.entries) {
        auto existing = std::find_if(baseline.begin(), baseline.end(), [&](const auto& entry) {
            return entry.scope == patch.scope && entry.owner == patch.owner &&
                entry.target_id == patch.target_id && entry.key == patch.key;
        });
        if (existing == baseline.end()) baseline.push_back(patch);
        else *existing = patch;
    }
    return baseline;
}

// Native slicing has no Apply transaction, but its completion still belongs to
// the exact workspace that started it. Keep a pending run until its callback.
class NativeSliceSession
{
public:
    bool pending() const { return m_pending; }
    bool has_result() const { return m_revision.has_value(); }
    const AI::SmartSlicing::OfficialSliceResult& result() const { return m_result; }
    const std::optional<AI::SmartSlicing::WorkspaceRevision>& revision() const { return m_revision; }

    void start(const AI::SmartSlicing::WorkspaceRevision& revision)
    {
        m_revision = revision;
        m_pending = true;
        m_result = {AI::SmartSlicing::OfficialSlicePhase::Slicing, {}};
    }

    void refresh(const AI::SmartSlicing::WorkspaceRevision& current)
    {
        if (m_revision && *m_revision != current)
            m_result = {AI::SmartSlicing::OfficialSlicePhase::Failed, "workspace_changed"};
    }

    void complete(bool success, const AI::SmartSlicing::WorkspaceRevision& current,
                  const std::string& diagnostic)
    {
        if (!m_pending) return;
        m_pending = false;
        if (!m_revision || *m_revision != current) {
            m_result = {AI::SmartSlicing::OfficialSlicePhase::Failed, "workspace_changed"};
        } else {
            m_result = {success ? AI::SmartSlicing::OfficialSlicePhase::Completed :
                AI::SmartSlicing::OfficialSlicePhase::Failed,
                success ? std::string{} : diagnostic.empty() ? "official_slice_failed" : diagnostic};
        }
    }

    bool reset()
    {
        if (m_pending) return false;
        m_revision.reset();
        m_result = {};
        return true;
    }

private:
    std::optional<AI::SmartSlicing::WorkspaceRevision> m_revision;
    AI::SmartSlicing::OfficialSliceResult m_result;
    bool m_pending {false};
};

struct SmartSlicingWorkbenchState
{
    AI::SmartSlicing::RecommendationSessionSnapshot session;
    AI::SmartSlicing::RecommendationGoal selected_goal {AI::SmartSlicing::RecommendationGoal::Balanced};
    std::array<std::optional<AI::SmartSlicing::SliceCandidate>, 3> candidates;
    std::array<std::optional<AI::SmartSlicing::TrialMetrics>, 3> metrics;
    std::array<std::vector<AI::SmartSlicing::ConfigPatchEntry>, 3> effective_parameters;
    AI::SmartSlicing::OfficialSliceResult official;
    std::optional<AI::SmartSlicing::PrintabilityReport> preflight;
    std::vector<AI::SmartSlicing::MachineCapabilityReason> machine_reasons;
    std::vector<AI::SmartSlicing::MaterialCompatibilityReason> material_reasons;
    std::vector<std::string> palette;
    std::string diagnostic;
    bool analyzing {false};
    bool can_analyze {true};
    bool can_start {false};
    bool can_retry {false};
    bool can_keep_current_mesh {false};
};

using SmartSlicingWorkbenchListener = std::function<void(const SmartSlicingWorkbenchState&)>;

} // namespace Slic3r::GUI
