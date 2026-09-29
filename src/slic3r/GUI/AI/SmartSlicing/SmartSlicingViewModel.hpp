#pragma once

#include "slic3r/AI/SmartSlicing/Domain/WorkflowState.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

enum class SmartSlicingStageStatus { Waiting, Active, Complete, NeedsAttention, Disabled };
enum class LegacyAIWorkflowStatus { Waiting, Running, Success, Warning, Failed };
enum class SmartSlicingMode { AI, Orca };
enum class SmartSlicingPurpose { Decoration, General, Functional };
enum class SmartSlicingGoalState {
    Analyzing,
    Ready,
    Unavailable,
    Failed,
    Stale,
    Applied,
    OfficialSlicing,
    ApplyFailed,
};

struct SmartSlicingStageView
{
    SmartSlicingStageStatus status{SmartSlicingStageStatus::Waiting};
};

struct SmartSlicingCandidateView
{
    std::string id;
    std::string explanation;
    std::string diagnostic_code;
    std::vector<std::string> evidence_codes;
    std::vector<AI::SmartSlicing::ConfigPatchEntry> parameter_changes;
    size_t placement_change_count{0};
    std::optional<double> estimated_time_seconds;
    std::optional<double> filament_volume_mm3;
    std::optional<double> support_volume_mm3;
    std::optional<double> flush_volume_mm3;
    std::optional<double> wipe_tower_volume_mm3;
    std::optional<size_t> tool_changes;
    std::optional<double> time_delta_seconds;
    std::optional<double> filament_delta_mm3;
    std::optional<double> support_delta_mm3;
    std::optional<double> flush_delta_mm3;
    std::optional<double> wipe_tower_delta_mm3;
    std::optional<long long> tool_change_delta;
    std::optional<bool> physical_slots_compatible;
    std::optional<bool> color_mapping_degraded;
    std::optional<bool> prime_tower_enabled;
    size_t layer_tool_sequence_count{0};
    bool recommended{false};
    bool selected{false};
    bool failed{false};
    bool can_retry{false};
    bool can_select{false};
};

struct SmartSlicingBaselineView
{
    std::string candidate_id;
    AI::SmartSlicing::GoalResultStatus status{AI::SmartSlicing::GoalResultStatus::Analyzing};
    std::vector<std::string> diagnostic_codes;
    std::string summary_key{"native_baseline"};
    std::optional<double> estimated_time_seconds;
    std::optional<double> filament_volume_mm3;
};

struct SmartSlicingGoalActions
{
    bool can_view_details{false};
    bool can_select{false};
    bool can_apply{false};
    bool can_retry_slice{false};
    bool can_undo{false};
    bool can_reanalyze{false};
};

struct SmartSlicingGoalView
{
    std::string goal_id;
    std::string task_candidate_id;
    std::string candidate_id;
    AI::SmartSlicing::GoalResultStatus status{AI::SmartSlicing::GoalResultStatus::Analyzing};
    SmartSlicingGoalState state{SmartSlicingGoalState::Analyzing};
    std::vector<std::string> diagnostic_codes;
    std::optional<AI::SmartSlicing::RecommendationEvidence> evidence;
    SmartSlicingGoalActions actions;
};

struct SmartSlicingViewModel
{
    SmartSlicingMode mode{SmartSlicingMode::AI};
    SmartSlicingPurpose purpose{SmartSlicingPurpose::General};
    std::string mode_id{"ai"};
    std::string purpose_id{"general"};
    std::string native_baseline_summary_key{"native_baseline"};
    bool native_baseline_available{false};
    std::array<SmartSlicingStageView, 4> stages{};
    std::array<LegacyAIWorkflowStatus, 6> legacy_steps{};
    std::string summary_key{"ready_to_start"};
    std::string detail;
    std::vector<std::pair<std::string, std::string>> issues;
    std::vector<SmartSlicingCandidateView> candidates;
    SmartSlicingBaselineView baseline;
    std::array<SmartSlicingGoalView, 3> goal_results{{
        {"balanced"},
        {"speed"},
        {"quality"},
    }};
    uint32_t recommendation_contract_version{AI::SmartSlicing::RECOMMENDATION_CONTRACT_VERSION};
    size_t issue_count{0};
    bool can_start{true};
    bool can_cancel{false};
    bool can_plan_candidates{false};
    bool can_apply{false};
    bool can_undo_apply{false};
    bool needs_polling{false};
    bool is_stale{false};
    bool has_report{false};
    bool can_add_model{false};
    bool can_recheck{false};
    bool has_recommendation_contract{false};

    static SmartSlicingViewModel from_snapshot(const AI::SmartSlicing::WorkflowSnapshot& snapshot);
};

} // namespace Slic3r::GUI
