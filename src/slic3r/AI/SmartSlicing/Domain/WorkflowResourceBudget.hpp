#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Slic3r::AI::SmartSlicing {

struct WorkflowResourceBudget
{
    size_t maximum_candidates{3};
    std::chrono::seconds maximum_elapsed{std::chrono::minutes(30)};
    uint64_t maximum_memory_bytes{2ull * 1024ull * 1024ull * 1024ull};
    uint64_t maximum_temporary_disk_bytes{512ull * 1024ull * 1024ull};
};

struct WorkflowResourceUsage
{
    uint64_t memory_bytes{0};
    uint64_t temporary_disk_bytes{0};
};

inline std::string workflow_budget_violation(const WorkflowResourceBudget& budget,
                                             size_t candidate_count,
                                             std::chrono::steady_clock::duration elapsed,
                                             const WorkflowResourceUsage& usage)
{
    if (candidate_count > budget.maximum_candidates)
        return "candidate_budget_exceeded";
    if (elapsed > budget.maximum_elapsed)
        return "workflow_timeout";
    if (usage.memory_bytes > budget.maximum_memory_bytes)
        return "workflow_memory_budget_exceeded";
    if (usage.temporary_disk_bytes > budget.maximum_temporary_disk_bytes)
        return "workflow_disk_budget_exceeded";
    return {};
}

} // namespace Slic3r::AI::SmartSlicing
