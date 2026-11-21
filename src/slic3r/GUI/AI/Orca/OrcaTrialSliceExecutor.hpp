#pragma once

#include "OrcaParameterProposalAdapter.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/AI/SmartSlicing/Application/TrialSliceScheduler.hpp"
#include "slic3r/AI/SmartSlicing/Domain/IntentConstraintSnapshot.hpp"
#include "slic3r/AI/SmartSlicing/Ports/ITrialSliceExecutor.hpp"
#include "slic3r/AI/SmartSlicing/Domain/SmartSliceTypes.hpp"

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
    DynamicPrintConfig plate_overrides;
    int plate_index{0};
    int64_t plate_id{-1};
    bool automatic_tower{false};
    std::string plate_name;
    Vec3d plate_origin{Vec3d::Zero()};
    bool check_filament_compatibility{true};
    bool has_user_marked_surface{false};
    std::vector<std::vector<DynamicPrintConfig>> extruder_filament_info;
    AI::SmartSlicing::IntentConstraintSnapshot intent_constraints;
    std::vector<AI::SmartSlicing::ParameterBoundEvidence> profile_bounds;
    AI::SmartSlicing::WorkspaceRevision evidence_revision;
    AI::SmartSlicing::MetricValue<bool> physical_slots_compatible;
    AI::SmartSlicing::MetricValue<bool> materials_compatible;
    AI::SmartSlicing::MetricValue<bool> color_mapping_degraded;
};

inline const char* tower_position_diagnostic(WipeTowerPositionStatus status)
{
    switch (status) {
    case WipeTowerPositionStatus::Outside: return "trial_tower_outside";
    case WipeTowerPositionStatus::Exclusion: return "trial_tower_excluded";
    case WipeTowerPositionStatus::ModelOverlap: return "trial_tower_model_overlap";
    default: return "trial_tower_position";
    }
}

struct OrcaTrialSliceControl
{
    std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::time_point::max()};
    std::function<bool()> canceled;
    // A retry at an anchor derived from the generated base keeps that anchor fixed.
    bool allow_estimated_tower_relocation{true};
};

struct OrcaTrialSliceDetails
{
    // Coordinates are local to the captured plate. A retry hint is never
    // committed: only a trial that validates and exports may set tower_position.
    std::optional<Vec2d> tower_retry_position;
    std::optional<Vec2d> tower_position;
    std::optional<Print::WipeTowerPositionGeometry> tower_geometry;
    DynamicPrintConfig plate_patch;
    std::vector<std::pair<uint64_t, std::vector<double>>> layer_profiles;
    bool variable_layers{false};
    bool preserves_manual_settings{false};
    std::string support_type;
    std::optional<int> support_angle;
    std::optional<uint64_t> support_interface_signature;
    bool plate_only_support{false};
    bool mixed_nominal_layer_heights{false};
    double effective_layer_height{0.};
    double mean_layer_height{0.};
    size_t layer_count{0};
    std::optional<std::pair<double, double>> layer_range;
    std::optional<double> support_grams;
    std::optional<double> flush_grams;
    std::optional<double> tower_grams;
    AI::SmartSlicing::PlacementCandidate placement;
    double orientation_degrees{0.};
    std::vector<std::array<Vec3f, 3>> original_preview;
    std::vector<std::array<Vec3f, 3>> oriented_preview;
};

AI::SmartSlicing::MetricValue<std::vector<AI::SmartSlicing::LayerToolSequence>>
extract_orca_layer_tool_sequences(const GCodeProcessorResult& result);

class OrcaTrialSliceExecutor final : public AI::SmartSlicing::ITrialSliceExecutor
{
public:
    using InputProvider = std::function<OrcaTrialSliceInput()>;

    explicit OrcaTrialSliceExecutor(InputProvider input_provider);
    ~OrcaTrialSliceExecutor() override;

    void prepare_session_input(OrcaTrialSliceInput input,
        const std::vector<AI::SmartSlicing::SliceCandidate>& candidates = {});
    void clear_session_input();
    void set_resource_limits(std::chrono::seconds maximum_duration, uint64_t maximum_memory_bytes,
                             uint64_t maximum_temporary_disk_bytes);

    AI::SmartSlicing::TrialSliceResult execute_trial_slice(const AI::SmartSlicing::SliceCandidate& candidate) override;
    static bool same_native_recommendation_input(const OrcaTrialSliceInput& input,
        const AI::SmartSlicing::SliceRecommendation& lhs, const AI::SmartSlicing::SliceRecommendation& rhs);
    AI::SmartSlicing::TrialSliceResult execute_recommendation_slice(
        OrcaTrialSliceInput input,
        const AI::SmartSlicing::SliceRecommendation& recommendation,
        const AI::SmartSlicing::WorkspaceRevision& base_revision,
        OrcaTrialSliceDetails& details,
        const OrcaTrialSliceControl& control = {});
    AI::SmartSlicing::VersionedTrialSliceResult execute_versioned_trial_slice(
        const AI::SmartSlicing::TrialSliceTask& task,
        const std::shared_ptr<const AI::SmartSlicing::RecommendationCancellationToken>& cancellation_token);
    void cancel_trial_slice() override;
    // The displayed alternative is prepared on this isolated input.
    static bool prepare_orientation(OrcaTrialSliceInput& input, OrcaTrialSliceDetails& preview,
                                    const std::function<bool()>& canceled);

private:
    class ActivePrintGuard;

    bool apply_placement(Model& model, const AI::SmartSlicing::PlacementCandidate& placement) const;
    AI::SmartSlicing::TrialSliceResult execute_trial_slice_impl(
        const AI::SmartSlicing::SliceCandidate& candidate,
        const AI::SmartSlicing::TrialSliceTask* versioned_task,
        const std::shared_ptr<const AI::SmartSlicing::RecommendationCancellationToken>& cancellation_token,
        AI::SmartSlicing::TrialMetrics* trial_metrics,
        AI::SmartSlicing::TrialEvaluationFacts* evaluation_facts,
        std::optional<OrcaTrialSliceInput> supplied_input = std::nullopt,
        std::optional<DynamicPrintConfig> override_config = std::nullopt,
        OrcaTrialSliceDetails* details = nullptr, bool request_adaptive_layers = false,
        const OrcaTrialSliceControl& control = {});
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
    struct PreparedParameters {
        AI::SmartSlicing::SliceCandidate candidate;
        OrcaParameterApplyResult result;
        std::vector<OrcaObjectParameterPatch> patches;
    };
    std::vector<PreparedParameters> m_prepared_parameters;
    std::chrono::seconds m_maximum_duration{std::chrono::minutes(30)};
    uint64_t m_maximum_memory_bytes{2ull * 1024ull * 1024ull * 1024ull};
    uint64_t m_maximum_temporary_disk_bytes{512ull * 1024ull * 1024ull};
};

} // namespace GUI
} // namespace Slic3r
