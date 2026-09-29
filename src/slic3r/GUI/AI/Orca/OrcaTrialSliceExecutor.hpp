#pragma once

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/AI/SmartSlicing/Application/TrialSliceScheduler.hpp"
#include "slic3r/AI/SmartSlicing/Domain/IntentConstraintSnapshot.hpp"
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

struct OrcaTrialSliceInput
{
    Model model;
    DynamicPrintConfig config;
    int plate_index{0};
    int64_t plate_id{-1};
    std::string plate_name;
    std::vector<std::vector<DynamicPrintConfig>> extruder_filament_info;
    AI::SmartSlicing::IntentConstraintSnapshot intent_constraints;
    std::vector<AI::SmartSlicing::ParameterBoundEvidence> profile_bounds;
    AI::SmartSlicing::WorkspaceRevision evidence_revision;
    AI::SmartSlicing::MetricValue<bool> physical_slots_compatible;
    AI::SmartSlicing::MetricValue<bool> materials_compatible;
    AI::SmartSlicing::MetricValue<bool> color_mapping_degraded;
};

AI::SmartSlicing::MetricValue<std::vector<AI::SmartSlicing::LayerToolSequence>>
extract_orca_layer_tool_sequences(const GCodeProcessorResult& result);

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
    AI::SmartSlicing::VersionedTrialSliceResult execute_versioned_trial_slice(
        const AI::SmartSlicing::TrialSliceTask& task,
        const std::shared_ptr<const AI::SmartSlicing::RecommendationCancellationToken>& cancellation_token);
    void cancel_trial_slice() override;

private:
    class ActivePrintGuard;

    bool apply_placement(Model& model, const AI::SmartSlicing::PlacementCandidate& placement) const;
    AI::SmartSlicing::TrialSliceResult execute_trial_slice_impl(
        const AI::SmartSlicing::SliceCandidate& candidate,
        const AI::SmartSlicing::TrialSliceTask* versioned_task,
        const std::shared_ptr<const AI::SmartSlicing::RecommendationCancellationToken>& cancellation_token,
        AI::SmartSlicing::TrialMetrics* trial_metrics,
        AI::SmartSlicing::TrialEvaluationFacts* evaluation_facts);
    static AI::SmartSlicing::SlicingMetrics extract_metrics(const GCodeProcessorResult& result,
                                                             const std::vector<int>& expected_filament_mapping,
                                                             bool prime_tower_enabled);
    static AI::SmartSlicing::TrialMetrics extract_trial_metrics(
        const GCodeProcessorResult& result,
        const DynamicPrintConfig& effective_config,
        const Model& effective_model,
        const AI::SmartSlicing::ParameterProposal& parameters,
        const std::vector<int>& expected_filament_mapping,
        bool prime_tower_enabled);

    InputProvider m_input_provider;
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
