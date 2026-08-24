#pragma once

#include "slic3r/AI/SmartSlicing/Domain/WorkflowState.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::GUI {

enum class SmartSlicingStageStatus { Waiting, Active, Complete, NeedsAttention, Disabled };
enum class LegacyAIWorkflowStatus { Waiting, Running, Success, Warning, Failed };

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
    std::optional<double> estimated_time_seconds;
    std::optional<double> filament_volume_mm3;
    std::optional<double> support_volume_mm3;
    std::optional<double> brim_volume_mm3;
    std::optional<double> bed_adhesion_risk_score;
    std::optional<double> flush_volume_mm3;
    std::optional<double> wipe_tower_volume_mm3;
    std::optional<size_t> tool_changes;
    std::optional<double> time_delta_seconds;
    std::optional<double> filament_delta_mm3;
    std::optional<double> support_delta_mm3;
    std::optional<double> brim_volume_delta_mm3;
    std::optional<double> bed_adhesion_risk_delta;
    std::optional<double> flush_delta_mm3;
    std::optional<double> wipe_tower_delta_mm3;
    std::optional<long long> tool_change_delta;
    std::optional<bool> physical_slots_compatible;
    std::optional<bool> color_mapping_degraded;
    std::optional<bool> prime_tower_enabled;
    std::vector<std::string> warning_codes;
    size_t layer_tool_sequence_count{0};
    size_t repair_operation_count{0};
    size_t transformed_instance_count{0};
    size_t plate_parameter_change_count{0};
    size_t object_parameter_change_count{0};
    size_t material_parameter_change_count{0};
    size_t workspace_parameter_change_count{0};
    std::optional<double> brim_width_before;
    std::optional<double> brim_width_after;
    std::optional<std::string> brim_type_before;
    std::optional<std::string> brim_type_after;
    bool repair_changes_geometry_semantics{false};
    bool recommended{false};
    bool selected{false};
    bool failed{false};
    bool can_retry{false};
    bool can_select{false};
};

struct SmartSlicingIssueView
{
    std::string code;
    std::string evidence;
    uint64_t object_id{0};
};

struct SmartSlicingViewModel
{
    std::array<SmartSlicingStageView, 4> stages{};
    std::array<LegacyAIWorkflowStatus, 6> legacy_steps{};
    std::string summary_key{"ready_to_start"};
    std::string detail;
    std::vector<SmartSlicingIssueView> issues;
    std::vector<SmartSlicingCandidateView> candidates;
    size_t issue_count{0};
    bool can_start{true};
    bool can_cancel{false};
    bool can_accept_risk{false};
    bool can_plan_candidates{false};
    bool can_apply{false};
    bool can_undo_apply{false};
    bool needs_polling{false};
    bool is_stale{false};

    static SmartSlicingViewModel from_snapshot(const AI::SmartSlicing::WorkflowSnapshot& snapshot);
};

} // namespace Slic3r::GUI
