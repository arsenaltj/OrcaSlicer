#include "OrcaTrialSliceExecutor.hpp"
#include "OrcaParameterAdvisor.hpp"
#include "OrcaParameterProposalAdapter.hpp"
#include "OrcaPlacementTransformValidator.hpp"

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

class ScopedTrialDeadline
{
public:
    ScopedTrialDeadline(std::chrono::seconds duration, std::function<void()> expired)
        : m_thread([this, duration, expired = std::move(expired)] {
              std::unique_lock<std::mutex> lock(m_mutex);
              if (!m_condition.wait_for(lock, duration, [this] { return m_finished; }))
                  expired();
          })
    {}

    ~ScopedTrialDeadline()
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

} // namespace

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
    std::set<uint64_t> seen_instances;
    for (const ObjectTransform& transform : placement.transforms) {
        if (!seen_instances.insert(transform.instance_id).second)
            return false;
        ModelObject* target_object = nullptr;
        ModelInstance* target = nullptr;
        for (ModelObject* object : model.objects) {
            if (object == nullptr || object->id().id != transform.object_id)
                continue;
            const auto instance = std::find_if(object->instances.begin(), object->instances.end(), [&transform](const ModelInstance* item) {
                return item != nullptr && item->id().id == transform.instance_id;
            });
            if (instance != object->instances.end()) {
                target_object = object;
                target = *instance;
            }
            break;
        }
        if (!orca_placement_target_is_eligible(target_object, target))
            return false;

        Transform3d matrix;
        for (Eigen::Index row = 0; row < matrix.rows(); ++row)
            for (Eigen::Index column = 0; column < matrix.cols(); ++column)
                matrix(row, column) = transform.matrix[static_cast<size_t>(row * matrix.cols() + column)];
        if (!matrix.matrix().allFinite() || std::abs(matrix.linear().determinant()) <= 1e-12 ||
            !matrix.matrix().row(3).isApprox(Eigen::RowVector4d(0.0, 0.0, 0.0, 1.0)))
            return false;
        if (!orca_placement_transform_preserves_geometry(target->get_matrix(), matrix))
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
    const auto brim = statistics.used_filaments_per_role.find(erBrim);
    metrics.brim_volume_mm3 = brim == statistics.used_filaments_per_role.end() ? 0.0 :
                                                                               std::max(0.0, brim->second.second);
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

TrialSliceResult OrcaTrialSliceExecutor::execute_trial_slice(const SliceCandidate& candidate)
{
    const std::lock_guard<std::mutex> execution_lock(m_execution_mutex);
    TrialSliceResult result;
    result.candidate_id  = candidate.id;
    result.base_revision = candidate.base_revision;
    try {
        OrcaTrialSliceInput input;
        bool prepared_session = false;
        {
            std::lock_guard<std::mutex> lock(m_session_mutex);
            if (m_session_input) {
                input = *m_session_input;
                prepared_session = true;
            }
        }
        if (!prepared_session) {
            m_cancel_requested.store(false, std::memory_order_release);
            m_timed_out.store(false, std::memory_order_release);
            input = m_input_provider();
        }
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
        ScopedTrialDeadline deadline(m_maximum_duration, [this] {
            m_timed_out.store(true, std::memory_order_release);
            cancel_trial_slice();
        });
        if (m_cancel_requested.load(std::memory_order_acquire)) {
            result.status          = TrialSliceStatus::Canceled;
            result.diagnostic_code = m_timed_out.load(std::memory_order_acquire) ? "workflow_timeout" : "trial_slice_canceled";
            return result;
        }
        if (!candidate.parameters.entries.empty()) {
            DynamicPrintConfig patched_config;
            const OrcaParameterApplyResult parameter_result = OrcaParameterProposalAdapter().validate_and_apply(
                candidate.parameters, input.plate_id, input.config, patched_config);
            if (!parameter_result.accepted) {
                result.diagnostic_code = parameter_result.diagnostic_code;
                return result;
            }
            input.config = std::move(patched_config);
        }
        if (!apply_placement(input.model, candidate.placement)) {
            result.diagnostic_code = "invalid_candidate_placement";
            return result;
        }

        std::vector<OrcaInstanceGeometrySnapshot> printable_instances;
        for (const ModelObject* object : input.model.objects) {
            if (object == nullptr)
                continue;
            for (const ModelInstance* instance : object->instances)
                if (const auto geometry = orca_printable_instance_geometry(object, instance))
                    printable_instances.push_back(*geometry);
        }
        const std::optional<double> bed_adhesion_risk =
            orca_bed_adhesion_risk_score(printable_instances);

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
        if (trial_print.objects().empty()) {
            result.status          = TrialSliceStatus::Failed;
            result.diagnostic_code = "trial_no_printable_objects";
            return result;
        }

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
        result.metrics->bed_adhesion_risk_score = bed_adhesion_risk;
        result.metrics->warning_codes.insert(result.metrics->warning_codes.end(), validation_warnings.size(),
                                             "native_validation_warning");
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

void OrcaTrialSliceExecutor::cancel_trial_slice()
{
    m_cancel_requested.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_active_print_mutex);
    if (m_active_print != nullptr)
        m_active_print->cancel();
}

} // namespace Slic3r::GUI
