#pragma once

#include "slic3r/AI/SmartSlicing/Domain/WorkflowState.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

enum class SmartSlicingStageStatus { Waiting, Active, Complete, NeedsAttention, Disabled };
enum class LegacyAIWorkflowStatus { Waiting, Running, Success, Warning, Failed };

struct SmartSlicingStageView
{
    SmartSlicingStageStatus status{SmartSlicingStageStatus::Waiting};
};

struct SmartSlicingViewModel
{
    std::array<SmartSlicingStageView, 4> stages{};
    std::array<LegacyAIWorkflowStatus, 6> legacy_steps{};
    std::string summary_key{"ready_to_start"};
    std::string detail;
    std::vector<std::pair<std::string, std::string>> issues;
    size_t issue_count{0};
    bool can_start{true};
    bool can_cancel{false};
    bool is_stale{false};

    static SmartSlicingViewModel from_snapshot(const AI::SmartSlicing::WorkflowSnapshot& snapshot);
};

} // namespace Slic3r::GUI
