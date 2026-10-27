#pragma once

#include "slic3r/AI/SmartSlicing/Application/RecommendationSessionCoordinator.hpp"
#include "slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp"
#include "slic3r/AI/SmartSlicing/Domain/TrialMetrics.hpp"
#include "slic3r/AI/SmartSlicing/Domain/PrintabilityReport.hpp"
#include "slic3r/AI/SmartSlicing/Ports/IOfficialSliceGateway.hpp"
#include "slic3r/AI/SmartSlicing/Domain/SmartSliceTypes.hpp"

#include <array>
#include <algorithm>
#include <functional>
#include <optional>
#include <tuple>

namespace Slic3r::GUI {

enum class WorkbenchSliceRoute { Unavailable, AiRetry, AiCandidate, Native };
enum class SlicingRecommendationSource { Local, Cloud };
enum class CloudRecommendationStatus { Idle, Preparing, Requesting, Ready, Failed };

class CloudRecommendationGate
{
public:
    uint64_t begin(const AI::SmartSlicing::WorkspaceRevision& revision)
    {
        m_revision = revision;
        return ++m_attempt;
    }

    void cancel()
    {
        ++m_attempt;
        m_revision.reset();
    }

    bool accepts(uint64_t attempt, const AI::SmartSlicing::WorkspaceRevision& current) const
    {
        return m_revision && m_revision->valid() && attempt == m_attempt && *m_revision == current;
    }

    uint64_t attempt() const { return m_attempt; }
    const std::optional<AI::SmartSlicing::WorkspaceRevision>& revision() const { return m_revision; }

private:
    uint64_t m_attempt {0};
    std::optional<AI::SmartSlicing::WorkspaceRevision> m_revision;
};

struct CloudRecommendationView
{
    CloudRecommendationStatus status {CloudRecommendationStatus::Idle};
    std::string diagnostic;
    std::string diagnostic_id;
    std::string failed_goal;
    std::string failed_parameter;
    std::string failed_value;
    std::array<std::string, 3> summaries;
    std::vector<std::string> warnings;
};

// The native worker and the SmartSlice state are updated by different event
// paths.  Keep the UI locked while either source still reports an active
// slice, so a stale phase cannot make the user start a second job or hide the
// progress/cancel controls early.
inline bool workbench_slice_running(bool native_background_slicing,
                                    AI::SmartSlicing::OfficialSlicePhase phase)
{
    return native_background_slicing || phase == AI::SmartSlicing::OfficialSlicePhase::Slicing;
}

inline WorkbenchSliceRoute workbench_slice_route(bool native_tab, bool running, bool can_retry,
    bool can_start_ai, bool can_start_native)
{
    if (running) return WorkbenchSliceRoute::Unavailable;
    if (!native_tab && can_retry) return WorkbenchSliceRoute::AiRetry;
    if (!native_tab && can_start_ai) return WorkbenchSliceRoute::AiCandidate;
    return native_tab && can_start_native ? WorkbenchSliceRoute::Native : WorkbenchSliceRoute::Unavailable;
}

// The coordinator's calculation goal is not a user selection. A selection is
// usable only in the workspace the user reviewed, including after Undo/Redo.
class WorkbenchGoalSelection {
public:
    bool select(AI::SmartSlicing::RecommendationGoal goal, const AI::SmartSlicing::WorkspaceRevision& revision) {
        const auto& goals = AI::SmartSlicing::RECOMMENDATION_GOALS;
        if (!revision.valid() || std::find(goals.begin(), goals.end(), goal) == goals.end()) return false;
        m_goal = goal; m_revision = revision; return true;
    }
    std::optional<AI::SmartSlicing::RecommendationGoal> selected(
        const AI::SmartSlicing::WorkspaceRevision& revision) const {
        return m_revision && *m_revision == revision ? m_goal : std::nullopt;
    }
    bool has_selection() const { return m_goal.has_value(); }
    void clear() { m_goal.reset(); m_revision.reset(); }
private:
    std::optional<AI::SmartSlicing::RecommendationGoal> m_goal;
    std::optional<AI::SmartSlicing::WorkspaceRevision> m_revision;
};

// Keep the mode used by the last successful native slice independent from the
// revision-bound selection that is still eligible to start a new slice.
class WorkbenchSliceGoalHistory {
public:
    void begin(AI::SmartSlicing::RecommendationGoal goal)
    {
        m_pending_goal = goal;
        m_pending_revision.reset();
    }

    void begin(AI::SmartSlicing::RecommendationGoal goal,
               const AI::SmartSlicing::WorkspaceRevision& revision)
    {
        m_pending_goal = goal;
        m_pending_revision = revision.valid() ? std::optional<AI::SmartSlicing::WorkspaceRevision>(revision) : std::nullopt;
    }

    void complete(bool success, const AI::SmartSlicing::WorkspaceRevision& revision)
    {
        const bool same_model_and_plate = !m_pending_revision ||
            (m_pending_revision->model_revision == revision.model_revision &&
             m_pending_revision->plate_revision == revision.plate_revision);
        if (success && m_pending_goal && revision.valid() && same_model_and_plate) {
            m_last_sliced_goal = m_pending_goal;
            m_last_sliced_revision = revision;
        }
        m_pending_goal.reset();
        m_pending_revision.reset();
    }

    void cancel() { m_pending_goal.reset(); m_pending_revision.reset(); }

    std::optional<AI::SmartSlicing::RecommendationGoal> pending_goal() const { return m_pending_goal; }

    std::optional<AI::SmartSlicing::RecommendationGoal> last_sliced_goal(
        const AI::SmartSlicing::WorkspaceRevision& current) const
    {
        if (!m_last_sliced_goal || !m_last_sliced_revision || !current.valid() ||
            m_last_sliced_revision->model_revision != current.model_revision ||
            m_last_sliced_revision->plate_revision != current.plate_revision)
            return std::nullopt;
        return m_last_sliced_goal;
    }

private:
    std::optional<AI::SmartSlicing::RecommendationGoal> m_pending_goal;
    std::optional<AI::SmartSlicing::WorkspaceRevision> m_pending_revision;
    std::optional<AI::SmartSlicing::RecommendationGoal> m_last_sliced_goal;
    std::optional<AI::SmartSlicing::WorkspaceRevision> m_last_sliced_revision;
};

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

    bool completed_for(const AI::SmartSlicing::WorkspaceRevision& current) const
    {
        return !m_pending && current.valid() && m_revision && *m_revision == current &&
            m_result.phase == AI::SmartSlicing::OfficialSlicePhase::Completed;
    }

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

// Lightweight recommendations are not presented as final print-time
// statistics. The Redesign workbench may run one native trial slice for the
// currently displayed goal and publish the resulting metrics separately.
enum class ExactEstimateStatus { None, Running, Ready, Failed };

struct ExactEstimateView
{
    ExactEstimateStatus status{ExactEstimateStatus::None};
    AI::SmartSlicing::RecommendationGoal goal{AI::SmartSlicing::RecommendationGoal::Balanced};
    AI::SmartSlicing::WorkspaceRevision revision;
    std::optional<double> estimated_time_seconds;
    std::optional<double> material_volume_mm3;
    std::optional<double> material_grams;
    std::optional<size_t> tool_changes;
    std::string source;
    std::string diagnostic;
    std::optional<AI::SmartSlicing::SliceRecommendation> parameters;
};

inline bool same_slice_parameters(const AI::SmartSlicing::SliceRecommendation& a,
                                  const AI::SmartSlicing::SliceRecommendation& b)
{
    const auto key = [](const auto& p) {
        return std::tie(p.mode, p.layer_height_mm, p.wall_loops, p.sparse_infill_density_percent,
            p.sparse_infill_pattern, p.outer_wall_speed_mm_s, p.enable_support, p.support_type,
            p.support_threshold_angle_deg, p.adaptive_layer_height, p.arachne_wall_generator, p.line_width_mm);
    };
    return key(a) == key(b);
}

inline bool exact_estimate_matches(const ExactEstimateView& estimate,
    const AI::SmartSlicing::WorkspaceRevision& revision, AI::SmartSlicing::RecommendationGoal goal,
    const AI::SmartSlicing::SliceRecommendation& parameters)
{
    return estimate.revision == revision && estimate.goal == goal && estimate.parameters &&
        same_slice_parameters(*estimate.parameters, parameters);
}

// Keep the reviewed numbers separate from the cancellable trial task. Internal
// plan application may change the revision; this binds the old reference to
// that specific native run without making it a valid estimate for new inputs.
class WorkbenchSliceEstimate
{
public:
    void capture(const ExactEstimateView& estimate,
                 const AI::SmartSlicing::WorkspaceRevision& reviewed,
                 const AI::SmartSlicing::WorkspaceRevision& applied,
                 AI::SmartSlicing::RecommendationGoal goal,
                 const AI::SmartSlicing::SliceRecommendation& parameters)
    {
        m_estimate.reset();
        if (reviewed.valid() && applied.valid() && estimate.status == ExactEstimateStatus::Ready &&
            exact_estimate_matches(estimate, reviewed, goal, parameters)) {
            m_estimate = estimate;
            m_applied = applied;
        }
    }

    std::optional<ExactEstimateView> for_slice(
        const AI::SmartSlicing::WorkspaceRevision& current,
        AI::SmartSlicing::RecommendationGoal goal,
        const AI::SmartSlicing::SliceRecommendation& parameters) const
    {
        if (!m_estimate || !current.valid() || m_applied != current ||
            m_estimate->goal != goal || !m_estimate->parameters ||
            !same_slice_parameters(*m_estimate->parameters, parameters))
            return std::nullopt;
        return m_estimate;
    }

private:
    std::optional<ExactEstimateView> m_estimate;
    AI::SmartSlicing::WorkspaceRevision m_applied;
};

struct SmartSlicingWorkbenchState
{
    AI::SmartSlicing::RecommendationSessionSnapshot session;
    AI::SmartSlicing::RecommendationGoal selected_goal {AI::SmartSlicing::RecommendationGoal::Balanced};
    bool has_selected_goal {false};
    std::optional<AI::SmartSlicing::RecommendationGoal> pending_slice_goal;
    std::optional<AI::SmartSlicing::RecommendationGoal> last_sliced_goal;
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
    bool can_start_native {false};
    std::string native_blocked_reason;
    bool native_execution {false};
    bool official_statistics_ready {false};
    bool can_keep_current_mesh {false};
    SlicingRecommendationSource recommendation_source {SlicingRecommendationSource::Local};
    CloudRecommendationView cloud_recommendation;

    // Lightweight, millisecond-scale recommendations are the user-facing
    // SmartSlice plans.  Once selected they are translated into the native
    // plate config and sent through Orca's normal slicing entry point.  The
    // legacy candidate arrays remain available for compatibility with the
    // isolated trial workflow, but are not a prerequisite for native slicing.
    std::optional<AI::SmartSlicing::SmartAnalysisResult> lightweight_analysis;
    bool lightweight_analyzing {false};
    bool lightweight_stale {false};
    ExactEstimateView exact_estimate;
    std::optional<ExactEstimateView> slice_estimate;
};

enum class RecommendationPresentationStage {
    Idle, PreparingCloud, RequestingCloud, Analyzing, EstimatingCloud, EstimatingLocal, CloudReady, LocalReady, CloudFailed
};

inline bool workbench_estimate_pending(const SmartSlicingWorkbenchState& state,
                                      AI::SmartSlicing::RecommendationGoal goal)
{
    if (state.official_statistics_ready && goal == state.selected_goal)
        return false;
    const auto& estimate = state.exact_estimate;
    if (estimate.status == ExactEstimateStatus::None || estimate.status == ExactEstimateStatus::Running ||
        estimate.goal != goal ||
        (state.session.workspace_revision.valid() && estimate.revision != state.session.workspace_revision))
        return true;
    if (state.lightweight_analysis && state.lightweight_analysis->completed) {
        const auto mode = goal == AI::SmartSlicing::RecommendationGoal::Speed ? AI::SmartSlicing::SliceMode::SPEED :
            goal == AI::SmartSlicing::RecommendationGoal::Quality ? AI::SmartSlicing::SliceMode::QUALITY :
            AI::SmartSlicing::SliceMode::BALANCED;
        if (const auto* parameters = state.lightweight_analysis->find(mode))
            return !exact_estimate_matches(estimate, state.session.workspace_revision, goal, *parameters);
    }
    return false;
}

inline RecommendationPresentationStage workbench_recommendation_stage(
    const SmartSlicingWorkbenchState& state, AI::SmartSlicing::RecommendationGoal displayed_goal)
{
    const bool cloud = state.recommendation_source == SlicingRecommendationSource::Cloud;
    if (cloud && state.cloud_recommendation.status == CloudRecommendationStatus::Preparing)
        return RecommendationPresentationStage::PreparingCloud;
    if (cloud && state.cloud_recommendation.status == CloudRecommendationStatus::Requesting)
        return RecommendationPresentationStage::RequestingCloud;
    if (state.analyzing)
        return RecommendationPresentationStage::Analyzing;
    if (state.cloud_recommendation.status == CloudRecommendationStatus::Failed)
        return RecommendationPresentationStage::CloudFailed;
    if (cloud) {
        if (state.cloud_recommendation.status != CloudRecommendationStatus::Ready)
            return RecommendationPresentationStage::Idle;
        if (workbench_estimate_pending(state, displayed_goal))
            return RecommendationPresentationStage::EstimatingCloud;
        return RecommendationPresentationStage::CloudReady;
    }
    if (state.lightweight_analysis && state.lightweight_analysis->completed)
        return workbench_estimate_pending(state, displayed_goal) ?
            RecommendationPresentationStage::EstimatingLocal : RecommendationPresentationStage::LocalReady;
    return RecommendationPresentationStage::Idle;
}

inline bool workbench_recommendation_parameters_ready(const SmartSlicingWorkbenchState& state)
{
    return state.recommendation_source != SlicingRecommendationSource::Cloud ||
        state.cloud_recommendation.status == CloudRecommendationStatus::Ready;
}

inline bool workbench_analysis_busy(const SmartSlicingWorkbenchState& state)
{
    if (state.official_statistics_ready) return false;
    return state.analyzing || state.cloud_recommendation.status == CloudRecommendationStatus::Preparing ||
        state.cloud_recommendation.status == CloudRecommendationStatus::Requesting ||
        state.exact_estimate.status == ExactEstimateStatus::Running ||
        (state.lightweight_analysis &&
         state.lightweight_analysis->completed &&
         (state.recommendation_source == SlicingRecommendationSource::Local ||
          state.cloud_recommendation.status == CloudRecommendationStatus::Ready) &&
         workbench_estimate_pending(state, state.selected_goal));
}

inline bool workbench_show_reanalyze(const SmartSlicingWorkbenchState& state)
{
    return state.can_analyze && !workbench_analysis_busy(state) &&
        (state.lightweight_analysis.has_value() || !state.diagnostic.empty() ||
         state.cloud_recommendation.status == CloudRecommendationStatus::Failed);
}

inline bool workbench_goal_is_currently_selected(
    const SmartSlicingWorkbenchState& state, AI::SmartSlicing::RecommendationGoal goal)
{
    return state.has_selected_goal && state.selected_goal == goal;
}

inline bool workbench_goal_was_last_sliced(
    const SmartSlicingWorkbenchState& state, AI::SmartSlicing::RecommendationGoal goal)
{
    return state.last_sliced_goal && *state.last_sliced_goal == goal;
}

inline bool workbench_goal_is_pending(
    const SmartSlicingWorkbenchState& state, AI::SmartSlicing::RecommendationGoal goal)
{
    return state.pending_slice_goal && *state.pending_slice_goal == goal;
}

using SmartSlicingWorkbenchListener = std::function<void(const SmartSlicingWorkbenchState&)>;

} // namespace Slic3r::GUI
