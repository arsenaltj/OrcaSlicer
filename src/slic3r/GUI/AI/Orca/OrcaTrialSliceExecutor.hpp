#pragma once

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/AI/SmartSlicing/Domain/WorkspaceContext.hpp"
#include "slic3r/AI/SmartSlicing/Ports/ITrialSliceExecutor.hpp"

#include <atomic>
#include <functional>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

class Print;
struct GCodeProcessorResult;

namespace GUI {

inline std::optional<bool>
orca_physical_slots_compatible(AI::SmartSlicing::PhysicalSlotCompatibility compatibility)
{
    using AI::SmartSlicing::PhysicalSlotCompatibility;
    switch (compatibility) {
    case PhysicalSlotCompatibility::NotApplicable:
    case PhysicalSlotCompatibility::Compatible: return true;
    case PhysicalSlotCompatibility::Incompatible:
    case PhysicalSlotCompatibility::InvalidTemperatureRange: return false;
    case PhysicalSlotCompatibility::Unavailable: return std::nullopt;
    }
    return std::nullopt;
}

std::optional<double> orca_filament_volume_excluding_multicolor_waste(
    double total_volume_mm3, double flush_volume_mm3, double wipe_tower_volume_mm3);

struct OrcaTrialSliceInput
{
    Model model;
    DynamicPrintConfig config;
    int plate_index{0};
    int64_t plate_id{-1};
    std::string plate_name;
    std::vector<std::vector<DynamicPrintConfig>> extruder_filament_info;
    std::optional<bool> physical_slots_compatible;
    std::optional<bool> color_mapping_degraded;
};

class OrcaTrialSliceExecutor final : public AI::SmartSlicing::ITrialSliceExecutor
{
public:
    using InputProvider = std::function<OrcaTrialSliceInput()>;

    explicit OrcaTrialSliceExecutor(InputProvider input_provider);
    ~OrcaTrialSliceExecutor() override;

    void prepare_session_input(OrcaTrialSliceInput input);
    void clear_session_input();
    void set_resource_limits(std::chrono::seconds maximum_duration, uint64_t maximum_memory_bytes,
                             uint64_t maximum_temporary_disk_bytes);

    AI::SmartSlicing::TrialSliceResult execute_trial_slice(const AI::SmartSlicing::SliceCandidate& candidate) override;
    void cancel_trial_slice() override;

private:
    class ActivePrintGuard;

    bool apply_placement(Model& model, const AI::SmartSlicing::PlacementCandidate& placement) const;
    static AI::SmartSlicing::SlicingMetrics extract_metrics(const GCodeProcessorResult& result,
                                                             const std::vector<int>& expected_filament_mapping,
                                                             bool prime_tower_enabled,
                                                             std::optional<bool> physical_slots_compatible,
                                                             std::optional<bool> color_mapping_degraded);

    InputProvider m_input_provider;
    std::mutex m_execution_mutex;
    std::atomic<bool> m_cancel_requested{false};
    std::atomic<bool> m_timed_out{false};
    std::mutex m_active_print_mutex;
    Print* m_active_print{nullptr};
    std::mutex m_session_mutex;
    std::optional<OrcaTrialSliceInput> m_session_input;
    std::chrono::seconds m_maximum_duration{std::chrono::minutes(30)};
    uint64_t m_maximum_memory_bytes{2ull * 1024ull * 1024ull * 1024ull};
    uint64_t m_maximum_temporary_disk_bytes{512ull * 1024ull * 1024ull};
};

} // namespace GUI
} // namespace Slic3r
