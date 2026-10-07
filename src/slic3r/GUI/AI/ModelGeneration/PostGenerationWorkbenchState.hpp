#pragma once

#include "PostGenerationUiState.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r::GUI {

enum class WorkbenchCheckStatus { NotRun, Running, Normal, Attention, Invalid, Failed };

struct WorkbenchCheckResult {
    WorkbenchCheckStatus status {WorkbenchCheckStatus::NotRun};
    std::string model_sha256;
    std::string summary;
    size_t boundary_edges {0};
    size_t nonmanifold_edges {0};
};

struct PostGenerationWorkbenchState {
    PostGenerationUiState actions;
    std::uint64_t revision {0};
    std::string asset_id;
    std::string model_path;
    std::string candidate_path;
    bool editing {false};
    bool portrait_enabled {false};
    bool portrait_available {false};
    std::string portrait_unavailable_reason;
    WorkbenchCheckResult check;
    std::vector<std::string> palette;
    size_t faces {0};
    size_t vertices {0};
    bool dirty {false};
};

using PostGenerationWorkbenchListener = std::function<void(const PostGenerationWorkbenchState&)>;

}
