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
enum class RepairCandidateStatus { None, Generating, Ready, Accepted, Rejected, Failed };

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
    // Boundary and topology details used by the post-generation import gate.
    size_t boundary_loops {0};
    std::vector<size_t> boundary_loop_sizes;
    size_t nonmanifold_vertices {0};
    size_t self_intersections {0};
    size_t isolated_components {0};
    // A repair is always a temporary candidate.  The original check remains
    // available while the candidate is being reviewed.
    RepairCandidateStatus candidate_status {RepairCandidateStatus::None};
    std::string candidate_path;
    std::string source_sha256;
    std::string candidate_sha256;
    size_t before_faces {0}, before_vertices {0};
    size_t after_faces {0}, after_vertices {0};
    bool appearance_mapped {false};
    double appearance_confidence {0.0};
    std::string appearance_reason;
    bool force_import_risk {false};
};

// Native workbench configuration is independent of the six-channel AI port.
// An absent slot is a UI placeholder; it is never a fabricated loaded spool.
struct WorkbenchFilamentSlot {
    size_t slot {0};
    std::string color, preset, material, reason;
    bool configured {false}, compatible {false}, removable {false}, exists {false};
};
struct WorkbenchMixedFilament {
    size_t slot {0};
    std::string color, recipe, reason;
    bool valid {false};
};
struct WorkbenchPrinterFilaments {
    std::string printer, identity, signature, reason;
    size_t nozzles {0}, physical_count {0}, idle_nozzles {0}, physical_capacity {32};
    std::vector<WorkbenchFilamentSlot> slots;
    std::vector<WorkbenchMixedFilament> mixed;
    bool ready {false};
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
    bool can_force_import {false};
    bool import_has_topology_risk {false};
    std::string import_topology_reason;
    bool can_print {false};
    AI::PrintablePaletteSnapshot project_palette;
    std::vector<AI::PhysicalFilamentChannel> project_channels;
    WorkbenchPrinterFilaments printer_filaments;
    bool can_apply_printer_filaments {false};
    std::string apply_filaments_reason;
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
