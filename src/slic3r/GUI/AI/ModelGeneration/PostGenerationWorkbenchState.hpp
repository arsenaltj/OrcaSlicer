#pragma once

#include "PostGenerationUiState.hpp"
#include "PortraitOptimization.hpp"
#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r::GUI {

enum class WorkbenchCheckStatus { NotRun, Running, Normal, Attention, Invalid, Failed };
enum class WorkbenchCheckPhase { Idle, Inspecting, Repairing, Rechecking };

struct WorkbenchCheckResult {
    WorkbenchCheckStatus status {WorkbenchCheckStatus::NotRun};
    std::string model_sha256;
    std::string summary;
    size_t boundary_edges {0};
    size_t nonmanifold_edges {0};
    WorkbenchCheckPhase phase {WorkbenchCheckPhase::Idle};
    std::string repair_reason;
    size_t removed_faces {0};
    size_t reversed_faces {0};
    size_t degenerate_faces {0}, duplicate_faces {0}, inconsistent_edges {0};
};

struct PostGenerationWorkbenchState {
    PostGenerationUiState actions;
    std::uint64_t revision {0};
    std::string asset_id;
    std::string model_path;
    std::string candidate_path;
    bool editing {false};
    SemanticMode semantic_mode {SemanticMode::General};
    PortraitOptimizationState portrait_optimization;
    bool portrait_enabled {false};
    bool portrait_available {false};
    std::string portrait_unavailable_reason;
    WorkbenchCheckResult check;
    std::vector<std::string> palette;
    size_t faces {0};
    size_t vertices {0};
    bool dirty {false};
    bool can_reoptimize_regions {false};
    std::string reoptimization_reason;
    bool can_edit_project_colors {false};
    bool can_import_for_slicing {false};
    bool can_print {false};
    AI::PrintablePaletteSnapshot project_palette;
    std::vector<AI::PhysicalFilamentChannel> project_channels;
};

inline bool workbench_region_optimization_allowed(const PostGenerationUiState& actions,
    bool candidate_ready, bool semantic_available)
{
    return actions.can_edit && !candidate_ready && semantic_available;
}

inline bool workbench_physical_slot_exists(const std::vector<AI::PhysicalFilamentChannel>& channels, size_t slot)
{
    return std::any_of(channels.begin(), channels.end(),
        [slot](const auto& channel) { return channel.slot == slot; });
}

using PostGenerationWorkbenchListener = std::function<void(const PostGenerationWorkbenchState&)>;

}
