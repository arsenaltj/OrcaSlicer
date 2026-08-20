#pragma once

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/AI/SmartSlicing/Ports/ITrialSliceExecutor.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {

class Print;
struct GCodeProcessorResult;

namespace GUI {

struct OrcaTrialSliceInput
{
    Model model;
    DynamicPrintConfig config;
    int plate_index{0};
    std::string plate_name;
    std::vector<std::vector<DynamicPrintConfig>> extruder_filament_info;
};

class OrcaTrialSliceExecutor final : public AI::SmartSlicing::ITrialSliceExecutor
{
public:
    using InputProvider = std::function<OrcaTrialSliceInput()>;

    explicit OrcaTrialSliceExecutor(InputProvider input_provider);
    ~OrcaTrialSliceExecutor() override;

    AI::SmartSlicing::TrialSliceResult execute_trial_slice(const AI::SmartSlicing::SliceCandidate& candidate) override;
    void cancel_trial_slice() override;

private:
    class ActivePrintGuard;

    bool apply_placement(Model& model, const AI::SmartSlicing::PlacementCandidate& placement) const;
    static AI::SmartSlicing::SlicingMetrics extract_metrics(const GCodeProcessorResult& result);

    InputProvider m_input_provider;
    std::atomic<bool> m_cancel_requested{false};
    std::mutex m_active_print_mutex;
    Print* m_active_print{nullptr};
};

} // namespace GUI
} // namespace Slic3r
