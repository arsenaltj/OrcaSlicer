#include "OrcaTrialSliceExecutor.hpp"
#include "OrcaParameterProposalAdapter.hpp"

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <condition_variable>
#include <cmath>
#include <map>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Slic3r::GUI {
namespace {

using namespace AI::SmartSlicing;

class ScopedTrialGCode
{
public:
    ScopedTrialGCode()
        : m_requested_path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca-trial-%%%%-%%%%.gcode"))
    {}

    ~ScopedTrialGCode()
    {
        boost::system::error_code error;
        if (!m_actual_path.empty())
            boost::filesystem::remove(m_actual_path, error);
        error.clear();
        boost::filesystem::remove(m_requested_path, error);
    }

    std::string requested_path() const { return m_requested_path.string(); }
    void set_actual_path(std::string path) { m_actual_path = std::move(path); }
    bool exceeds(uint64_t maximum_bytes) const
    {
        boost::system::error_code error;
        const boost::filesystem::path path = m_actual_path.empty() ? m_requested_path : boost::filesystem::path(m_actual_path);
        if (!boost::filesystem::exists(path, error) || error)
            return false;
        const uintmax_t size = boost::filesystem::file_size(path, error);
        return !error && size > maximum_bytes;
    }

private:
    boost::filesystem::path m_requested_path;
    boost::filesystem::path m_actual_path;
};

class ScopedTrialCancellation
{
public:
    ScopedTrialCancellation(
        std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<const RecommendationCancellationToken> cancellation_token,
        std::function<void()> expired,
        std::function<void()> canceled)
        : m_thread([this, deadline, cancellation_token = std::move(cancellation_token),
                    expired = std::move(expired), canceled = std::move(canceled)] {
              std::unique_lock<std::mutex> lock(m_mutex);
              while (!m_finished) {
                  if (cancellation_token && cancellation_token->cancellation_requested()) {
                      lock.unlock();
                      canceled();
                      return;
                  }
                  const auto now = std::chrono::steady_clock::now();
                  if (now >= deadline) {
                      lock.unlock();
                      expired();
                      return;
                  }
                  m_condition.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(25)),
                                         [this] { return m_finished; });
              }
          })
    {}

    ~ScopedTrialCancellation()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_finished = true;
        }
        m_condition.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_finished{false};
    std::thread m_thread;
};

double sum_values(const std::map<size_t, double>& values)
{
    return std::accumulate(values.begin(), values.end(), 0.0,
                           [](double total, const auto& entry) { return total + entry.second; });
}

const char* scope_name(ConfigScope scope)
{
    switch (scope) {
    case ConfigScope::Plate: return "plate";
    case ConfigScope::Object: return "object";
    case ConfigScope::Material: return "material";
    case ConfigScope::Workspace: return "workspace";
    }
    return "unknown";
}

struct PlacementIntentCheck
{
    bool conflict{false};
    MetricValue<bool> preserved;
};

PlacementIntentCheck check_placement_intent(
    const PlacementCandidate& placement,
    const IntentConstraintSnapshot& constraints)
{
    PlacementIntentCheck result;
    if (placement.transforms.empty()) {
        result.preserved = MetricValue<bool>::known(true, {"no_candidate_placement_change"});
        return result;
    }

    bool evidence_unknown = false;
    bool plate_evidence = false;
    for (const IntentConstraintRecord& record : constraints.records) {
        if (record.type != IntentConstraintType::PlatePlacementLock)
            continue;
        plate_evidence = true;
        if (record.state == IntentConstraintState::Active)
            result.conflict = true;
        else if (record.state == IntentConstraintState::Unknown)
            evidence_unknown = true;
    }
    if (!plate_evidence)
        evidence_unknown = true;

    for (const ObjectTransform& transform : placement.transforms) {
        bool target_evidence = false;
        for (const IntentConstraintRecord& record : constraints.records) {
            const bool object_lock = record.type == IntentConstraintType::ObjectPlacementLock &&
                                     record.object_id == transform.object_id;
            const bool instance_lock = record.type == IntentConstraintType::InstancePlacementLock &&
                                       record.object_id == transform.object_id &&
                                       (record.instance_id == 0 || record.instance_id == transform.instance_id);
            if (!object_lock && !instance_lock)
                continue;
            target_evidence = true;
            if (record.state == IntentConstraintState::Active)
                result.conflict = true;
            else if (record.state == IntentConstraintState::Unknown)
                evidence_unknown = true;
        }
        if (!target_evidence)
            evidence_unknown = true;
    }

    if (result.conflict)
        result.preserved = MetricValue<bool>::known(false, {"orca_placement_lock_conflict"});
    else if (evidence_unknown)
        result.preserved = MetricValue<bool>::unknown({"placement_lock_evidence_unavailable"});
    else
        result.preserved = MetricValue<bool>::known(true, {"orca_placement_intent_constraints"});
    return result;
}

} // namespace

MetricValue<std::vector<LayerToolSequence>> extract_orca_layer_tool_sequences(
    const GCodeProcessorResult& result)
{
    std::map<size_t, std::vector<size_t>> sequences_by_layer;
    for (const GCodeProcessorResult::MoveVertex& move : result.moves) {
        if (move.type != EMoveType::Extrude)
            continue;
        std::vector<size_t>& sequence = sequences_by_layer[static_cast<size_t>(move.layer_id)];
        const size_t tool_id = static_cast<size_t>(move.extruder_id);
        if (sequence.empty() || sequence.back() != tool_id)
            sequence.push_back(tool_id);
    }
    if (sequences_by_layer.empty())
        return MetricValue<std::vector<LayerToolSequence>>::unknown(
            {"orca_gcode_extrusion_moves_unavailable"});

    std::vector<LayerToolSequence> sequences;
    sequences.reserve(sequences_by_layer.size());
    for (auto& [layer_index, tool_ids] : sequences_by_layer)
        sequences.push_back({layer_index, std::move(tool_ids)});
    return MetricValue<std::vector<LayerToolSequence>>::known(
        std::move(sequences), {"orca_gcode_extrusion_move_sequence"});
}

class OrcaTrialSliceExecutor::ActivePrintGuard
{
public:
    ActivePrintGuard(OrcaTrialSliceExecutor& executor, Print& print) : m_executor(executor), m_print(print)
    {
        std::lock_guard<std::mutex> lock(m_executor.m_active_print_mutex);
        m_executor.m_active_print = &m_print;
        if (m_executor.m_cancel_requested.load(std::memory_order_acquire))
            m_print.cancel();
    }

    ~ActivePrintGuard()
    {
        std::lock_guard<std::mutex> lock(m_executor.m_active_print_mutex);
        if (m_executor.m_active_print == &m_print)
            m_executor.m_active_print = nullptr;
    }

private:
    OrcaTrialSliceExecutor& m_executor;
    Print& m_print;
};

OrcaTrialSliceExecutor::OrcaTrialSliceExecutor(InputProvider input_provider) : m_input_provider(std::move(input_provider))
{
    if (!m_input_provider)
        throw std::invalid_argument("A trial slice input provider is required.");
}

OrcaTrialSliceExecutor::~OrcaTrialSliceExecutor() { cancel_trial_slice(); }

void OrcaTrialSliceExecutor::prepare_session_input(OrcaTrialSliceInput input)
{
    std::lock_guard<std::mutex> lock(m_session_mutex);
    m_session_input = std::move(input);
    m_cancel_requested.store(false, std::memory_order_release);
    m_timed_out.store(false, std::memory_order_release);
}

void OrcaTrialSliceExecutor::clear_session_input()
{
    std::lock_guard<std::mutex> lock(m_session_mutex);
    m_session_input.reset();
}

void OrcaTrialSliceExecutor::set_resource_limits(std::chrono::seconds maximum_duration, uint64_t maximum_memory_bytes,
                                                uint64_t maximum_temporary_disk_bytes)
{
    m_maximum_duration = maximum_duration;
    m_maximum_memory_bytes = maximum_memory_bytes;
    m_maximum_temporary_disk_bytes = maximum_temporary_disk_bytes;
}

bool OrcaTrialSliceExecutor::apply_placement(Model& model, const PlacementCandidate& placement) const
{
    for (const ObjectTransform& transform : placement.transforms) {
        ModelInstance* target = nullptr;
        for (ModelObject* object : model.objects) {
            if (object == nullptr || object->id().id != transform.object_id)
                continue;
            const auto instance = std::find_if(object->instances.begin(), object->instances.end(), [&transform](const ModelInstance* item) {
                return item != nullptr && item->id().id == transform.instance_id;
            });
            if (instance != object->instances.end())
                target = *instance;
            break;
        }
        if (target == nullptr)
            return false;

        Transform3d matrix;
        for (Eigen::Index row = 0; row < matrix.rows(); ++row)
            for (Eigen::Index column = 0; column < matrix.cols(); ++column)
                matrix(row, column) = transform.matrix[static_cast<size_t>(row * matrix.cols() + column)];
        if (!matrix.matrix().allFinite() || std::abs(matrix.linear().determinant()) <= 1e-12 ||
            !matrix.matrix().row(3).isApprox(Eigen::RowVector4d(0.0, 0.0, 0.0, 1.0)))
            return false;
        target->set_transformation(Geometry::Transformation(matrix));
    }
    return true;
}

SlicingMetrics OrcaTrialSliceExecutor::extract_metrics(const GCodeProcessorResult& result,
                                                       const std::vector<int>& expected_filament_mapping,
                                                       bool prime_tower_enabled)
{
    const PrintEstimatedStatistics& statistics = result.print_statistics;
    SlicingMetrics metrics;
    metrics.estimated_time_seconds = statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
    metrics.filament_volume_mm3    = sum_values(statistics.total_volumes_per_extruder);
    metrics.support_volume_mm3     = sum_values(statistics.support_volumes_per_extruder);
    metrics.flush_volume_mm3       = sum_values(statistics.flush_per_filament);
    metrics.wipe_tower_volume_mm3  = sum_values(statistics.wipe_tower_volumes_per_extruder);
    metrics.tool_changes           = statistics.total_filament_changes;
    metrics.physical_slots_compatible = true;
    metrics.prime_tower_enabled       = prime_tower_enabled;
    if (!result.filament_maps.empty()) {
        metrics.filament_to_physical_slot = result.filament_maps;
        metrics.color_mapping_degraded    = result.filament_maps != expected_filament_mapping;
    }
    metrics.filament_change_sequence.reserve(result.filament_change_sequence.size());
    for (const unsigned int filament : result.filament_change_sequence)
        metrics.filament_change_sequence.push_back(static_cast<size_t>(filament));
    metrics.layer_tool_sequences.reserve(result.layer_filaments.size());
    for (const auto& layer_entry : result.layer_filaments) {
        const std::vector<unsigned int>& filaments = layer_entry.first;
        std::vector<size_t> sequence;
        sequence.reserve(filaments.size());
        for (const unsigned int filament : filaments)
            sequence.push_back(static_cast<size_t>(filament));
        metrics.layer_tool_sequences.push_back(std::move(sequence));
    }
    std::sort(metrics.layer_tool_sequences.begin(), metrics.layer_tool_sequences.end());
    metrics.warning_codes.reserve(result.warnings.size());
    for (const GCodeProcessorResult::SliceWarning& warning : result.warnings)
        metrics.warning_codes.push_back(!warning.error_code.empty() ? warning.error_code : "gcode_warning");
    return metrics;
}

TrialMetrics OrcaTrialSliceExecutor::extract_trial_metrics(
    const GCodeProcessorResult& result,
    const DynamicPrintConfig& effective_config,
    const Model& effective_model,
    const ParameterProposal& parameters,
    const std::vector<int>& expected_filament_mapping,
    bool prime_tower_enabled)
{
    const PrintEstimatedStatistics& statistics = result.print_statistics;
    TrialMetrics metrics;
    metrics.estimated_time_seconds = MetricValue<double>::known(
        statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time,
        {"orca_gcode_time_estimator"});
    const double model_volume = sum_values(statistics.model_volumes_per_extruder);
    const double support_volume = sum_values(statistics.support_volumes_per_extruder);
    const double flush_volume = sum_values(statistics.flush_per_filament);
    const double wipe_tower_volume = sum_values(statistics.wipe_tower_volumes_per_extruder);
    metrics.model_material_volume_mm3 = MetricValue<double>::known(model_volume, {"orca_model_volume"});
    metrics.support_material_volume_mm3 = MetricValue<double>::known(support_volume, {"orca_support_volume"});
    metrics.flush_material_volume_mm3 = MetricValue<double>::known(flush_volume, {"orca_flush_volume"});
    metrics.wipe_tower_material_volume_mm3 =
        MetricValue<double>::known(wipe_tower_volume, {"orca_wipe_tower_volume"});
    metrics.total_material_volume_mm3 = MetricValue<double>::known(
        model_volume + support_volume + flush_volume + wipe_tower_volume,
        {"computed_from_orca_material_components"});
    metrics.tool_change_count = MetricValue<size_t>::known(
        statistics.total_filament_changes, {"orca_filament_change_count"});
    metrics.tool_change_time_seconds = MetricValue<double>::known(
        statistics.total_tool_change_time, {"orca_tool_change_time"});

    std::set<size_t> used_tools;
    for (const auto& layer_entry : result.layer_filaments) {
        for (const unsigned int filament : layer_entry.first)
            used_tools.insert(static_cast<size_t>(filament));
    }
    for (const GCodeProcessorResult::MoveVertex& move : result.moves) {
        if (move.type != EMoveType::Extrude)
            continue;
        used_tools.insert(static_cast<size_t>(move.extruder_id));
    }
    metrics.layer_tool_sequences = extract_orca_layer_tool_sequences(result);

    if (used_tools.size() <= 1) {
        metrics.physical_slots_compatible = MetricValue<bool>::not_applicable({"single_material"});
        metrics.color_mapping_degraded = MetricValue<bool>::not_applicable({"single_material"});
    } else {
        metrics.physical_slots_compatible = MetricValue<bool>::unknown({"physical_slot_evidence_unavailable"});
        if (!result.filament_maps.empty() && !expected_filament_mapping.empty())
            metrics.color_mapping_degraded = MetricValue<bool>::known(
                result.filament_maps != expected_filament_mapping, {"orca_filament_mapping"});
        else
            metrics.color_mapping_degraded = MetricValue<bool>::unknown({"filament_mapping_unavailable"});
    }
    metrics.materials_compatible = MetricValue<bool>::unknown({"material_compatibility_evidence_unavailable"});
    metrics.wipe_tower_enabled = MetricValue<bool>::known(prime_tower_enabled, {"orca_effective_config"});

    metrics.native_diagnostics.reserve(result.warnings.size());
    for (const GCodeProcessorResult::SliceWarning& warning : result.warnings) {
        NativeDiagnostic diagnostic;
        diagnostic.severity = warning.level >= 2 ? NativeDiagnosticSeverity::Error : NativeDiagnosticSeverity::Warning;
        diagnostic.code = warning.error_code.empty() ? "gcode_warning" : warning.error_code;
        diagnostic.message = warning.msg;
        metrics.native_diagnostics.push_back(std::move(diagnostic));
    }

    metrics.effective_parameters.reserve(parameters.entries.size());
    for (const ConfigPatchEntry& entry : parameters.entries) {
        if (effective_config.option(entry.key) == nullptr)
            continue;
        metrics.effective_parameters.push_back({
            std::string(scope_name(entry.scope)) + ":" + std::to_string(entry.target_id),
            entry.key,
            effective_config.opt_serialize(entry.key),
            "orca_effective_config"});
    }

    for (const ModelObject* object : effective_model.objects) {
        if (object == nullptr)
            continue;
        for (const ModelInstance* instance : object->instances) {
            if (instance == nullptr)
                continue;
            ObjectTransformSummary summary;
            summary.object_id = object->id().id;
            summary.instance_id = instance->id().id;
            const Transform3d& matrix = instance->get_matrix();
            for (Eigen::Index row = 0; row < matrix.rows(); ++row)
                for (Eigen::Index column = 0; column < matrix.cols(); ++column)
                    summary.transform[static_cast<size_t>(row * matrix.cols() + column)] = matrix(row, column);
            metrics.object_transforms.push_back(std::move(summary));
        }
    }
    return metrics;
}

TrialSliceResult OrcaTrialSliceExecutor::execute_trial_slice(const SliceCandidate& candidate)
{
    return execute_trial_slice_impl(candidate, nullptr, {}, nullptr, nullptr);
}

TrialSliceResult OrcaTrialSliceExecutor::execute_trial_slice_impl(
    const SliceCandidate& candidate,
    const TrialSliceTask* versioned_task,
    const std::shared_ptr<const RecommendationCancellationToken>& cancellation_token,
    TrialMetrics* trial_metrics,
    TrialEvaluationFacts* evaluation_facts)
{
    TrialSliceResult result;
    result.candidate_id  = candidate.id;
    result.base_revision = candidate.base_revision;
    try {
        OrcaTrialSliceInput input;
        if (versioned_task != nullptr) {
            std::lock_guard<std::mutex> lock(m_session_mutex);
            if (!m_session_input) {
                result.diagnostic_code = "versioned_trial_session_input_unavailable";
                return result;
            }
            input = *m_session_input;
        } else {
            m_cancel_requested.store(false, std::memory_order_release);
            input = m_input_provider();
        }
        m_timed_out.store(false, std::memory_order_release);
        const auto local_deadline = std::chrono::steady_clock::now() + m_maximum_duration;
        const auto effective_deadline = versioned_task != nullptr ?
                                            std::min(local_deadline, versioned_task->deadline) :
                                            local_deadline;
        if ((cancellation_token && cancellation_token->cancellation_requested()) ||
            m_cancel_requested.load(std::memory_order_acquire)) {
            result.status = TrialSliceStatus::Canceled;
            result.diagnostic_code = "trial_slice_canceled";
            return result;
        }
        if (std::chrono::steady_clock::now() >= effective_deadline) {
            m_timed_out.store(true, std::memory_order_release);
            result.status = TrialSliceStatus::Canceled;
            result.diagnostic_code = "workflow_timeout";
            return result;
        }
        ScopedTrialCancellation cancellation(
            effective_deadline, cancellation_token,
            [this] {
                m_timed_out.store(true, std::memory_order_release);
                cancel_trial_slice();
            },
            [this] { cancel_trial_slice(); });
        uint64_t estimated_model_bytes = 0;
        for (const ModelObject* object : input.model.objects) {
            if (object == nullptr)
                continue;
            for (const ModelVolume* volume : object->volumes) {
                if (volume == nullptr)
                    continue;
                const uint64_t facet_count = static_cast<uint64_t>(volume->mesh().facets_count());
                if (facet_count > std::numeric_limits<uint64_t>::max() / 128ull ||
                    estimated_model_bytes > std::numeric_limits<uint64_t>::max() - facet_count * 128ull) {
                    estimated_model_bytes = std::numeric_limits<uint64_t>::max();
                    break;
                }
                estimated_model_bytes += facet_count * 128ull;
            }
            if (estimated_model_bytes == std::numeric_limits<uint64_t>::max())
                break;
        }
        if (estimated_model_bytes > m_maximum_memory_bytes) {
            result.diagnostic_code = "workflow_memory_budget_exceeded";
            return result;
        }
        if (m_cancel_requested.load(std::memory_order_acquire)) {
            result.status          = TrialSliceStatus::Canceled;
            result.diagnostic_code = m_timed_out.load(std::memory_order_acquire) ? "workflow_timeout" : "trial_slice_canceled";
            return result;
        }
        if (!candidate.parameters.entries.empty()) {
            DynamicPrintConfig patched_config;
            const OrcaParameterApplyResult parameter_result = OrcaParameterProposalAdapter().validate_and_apply(
                candidate.parameters, input.plate_id, input.config, input.intent_constraints,
                input.profile_bounds, patched_config);
            if (!parameter_result.accepted) {
                result.diagnostic_code = parameter_result.diagnostic_code;
                return result;
            }
            input.config = std::move(patched_config);
        }
        const PlacementIntentCheck placement_intent =
            check_placement_intent(candidate.placement, input.intent_constraints);
        if (placement_intent.conflict) {
            result.diagnostic_code = "placement_intent_conflict";
            return result;
        }
        if (!apply_placement(input.model, candidate.placement)) {
            result.diagnostic_code = "invalid_candidate_placement";
            return result;
        }

        if (evaluation_facts != nullptr) {
            evaluation_facts->manual_intent_preserved = placement_intent.preserved;
            if (const auto* wall_loops = input.config.option<ConfigOptionInt>("wall_loops"))
                evaluation_facts->effective_wall_loops =
                    MetricValue<int>::known(wall_loops->value, {"orca_effective_config"});
            if (const auto* infill = input.config.option<ConfigOptionPercent>("sparse_infill_density"))
                evaluation_facts->effective_infill_percent =
                    MetricValue<double>::known(infill->value, {"orca_effective_config"});
            evaluation_facts->critical_surface_significantly_improved =
                MetricValue<bool>::unknown({"critical_surface_risk_evidence_unavailable"});
        }
        const DynamicPrintConfig effective_config = input.config;

        Print trial_print;
        ActivePrintGuard active_print(*this, trial_print);
        trial_print.set_status_silent();
        trial_print.set_plate_index(input.plate_index);
        trial_print.set_plate_name(input.plate_name);
        trial_print.set_extruder_filament_info(input.extruder_filament_info);
        for (ModelObject* object : input.model.objects)
            if (object != nullptr)
                trial_print.auto_assign_extruders(object);
        const auto* filament_mapping = input.config.option<ConfigOptionInts>("filament_map");
        const std::vector<int> expected_filament_mapping = filament_mapping != nullptr ? filament_mapping->values :
                                                                                    std::vector<int>{};
        const bool prime_tower_enabled = input.config.opt_bool("enable_prime_tower");
        trial_print.apply(input.model, std::move(input.config));

        std::vector<StringObjectException> validation_warnings;
        const StringObjectException validation_error = trial_print.validate(&validation_warnings);
        if (!validation_error.string.empty()) {
            result.diagnostic_code = "trial_validation_failed";
            return result;
        }

        trial_print.process();
        ScopedTrialGCode temporary_gcode;
        GCodeProcessorResult gcode_result;
        temporary_gcode.set_actual_path(trial_print.export_gcode(temporary_gcode.requested_path(), &gcode_result, nullptr));
        if (temporary_gcode.exceeds(m_maximum_temporary_disk_bytes)) {
            result.diagnostic_code = "workflow_disk_budget_exceeded";
            return result;
        }
        result.metrics = extract_metrics(gcode_result, expected_filament_mapping, prime_tower_enabled);
        result.metrics->warning_codes.insert(result.metrics->warning_codes.end(), validation_warnings.size(),
                                             "native_validation_warning");
        if (trial_metrics != nullptr) {
            *trial_metrics = extract_trial_metrics(gcode_result, effective_config, input.model,
                                                   candidate.parameters, expected_filament_mapping,
                                                   prime_tower_enabled);
            if (versioned_task != nullptr && input.evidence_revision.valid() &&
                input.evidence_revision == versioned_task->identity.workspace_revision) {
                trial_metrics->physical_slots_compatible = input.physical_slots_compatible;
                trial_metrics->materials_compatible = input.materials_compatible;
                trial_metrics->color_mapping_degraded = input.color_mapping_degraded;
            }
            for (const StringObjectException& warning : validation_warnings)
                trial_metrics->native_diagnostics.push_back(
                    {NativeDiagnosticSeverity::Warning, "native_validation_warning", warning.string});
        }
        result.status = TrialSliceStatus::Succeeded;
        return result;
    } catch (const CanceledException&) {
        result.status          = TrialSliceStatus::Canceled;
        result.diagnostic_code = m_timed_out.load(std::memory_order_acquire) ? "workflow_timeout" : "trial_slice_canceled";
    } catch (...) {
        result.status          = TrialSliceStatus::Failed;
        result.diagnostic_code = "trial_slice_exception";
    }
    return result;
}

VersionedTrialSliceResult OrcaTrialSliceExecutor::execute_versioned_trial_slice(
    const TrialSliceTask& task,
    const std::shared_ptr<const RecommendationCancellationToken>& cancellation_token)
{
    VersionedTrialSliceResult result;
    result.identity = task.identity;
    if (!cancellation_token || task.identity.candidate_id.empty() ||
        !task.identity.workspace_revision.valid()) {
        result.diagnostic_codes.emplace_back("versioned_trial_input_invalid");
        return result;
    }

    SliceCandidate candidate;
    candidate.id = task.identity.candidate_id;
    candidate.base_revision = task.identity.workspace_revision;
    candidate.placement = task.placement;
    candidate.parameters = task.parameters;
    TrialMetrics metrics;
    TrialEvaluationFacts facts;
    const TrialSliceResult legacy = execute_trial_slice_impl(
        candidate, &task, cancellation_token, &metrics, &facts);
    result.status = legacy.status;
    result.evaluation_facts = std::move(facts);
    if (legacy.status == TrialSliceStatus::Succeeded) {
        result.metrics = std::move(metrics);
        result.risks = RiskAssessment{};
    }
    if (!legacy.diagnostic_code.empty())
        result.diagnostic_codes.push_back(legacy.diagnostic_code);
    return result;
}

void OrcaTrialSliceExecutor::cancel_trial_slice()
{
    m_cancel_requested.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_active_print_mutex);
    if (m_active_print != nullptr)
        m_active_print->cancel();
}

} // namespace Slic3r::GUI
