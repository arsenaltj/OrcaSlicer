#include "OrcaTrialSliceExecutor.hpp"
#include "OrcaParameterProposalAdapter.hpp"
#include "SmartSliceApplier.hpp"

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Orient.hpp"
#include "libslic3r/ClipperUtils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

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

// Only compare a plate-only support candidate to the same process and pose.
// An identical set of deposited interface paths on the same layers is a
// conservative check that the candidate retains the original contacts.
std::optional<uint64_t> support_interface_signature(const Print& print)
{
    std::vector<uint64_t> signatures;
    size_t point_count = 0;
    const auto mix = [](uint64_t digest, uint64_t v) { return (digest ^ v) * 1099511628211ull; };
    const auto visit = [&](const auto& self, const ExtrusionEntity& entity, uint64_t start) -> void {
        if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
            for (const auto* child : *collection)
                if (child && point_count <= 400000) self(self, *child, start);
        } else if (entity.role() == erSupportMaterialInterface) {
            Polylines lines;
            entity.collect_polylines(lines);
            for (const auto& line : lines) {
                point_count += line.points.size();
                if (point_count > 400000) return;
                const auto fingerprint = [&](bool reverse) {
                    uint64_t hash = mix(start, line.points.size());
                    for (size_t i = 0; i < line.points.size(); ++i) {
                        const auto& point = line.points[reverse ? line.points.size() - 1 - i : i];
                        hash = mix(mix(hash, uint64_t(point.x())), uint64_t(point.y()));
                    }
                    return hash;
                };
                signatures.push_back(std::min(fingerprint(false), fingerprint(true)));
            }
        }
    };
    for (const auto* object : print.objects()) {
        for (const auto* layer : object->support_layers()) {
            const uint64_t id = mix(1469598103934665603ull, object->model_object()->id().id);
            visit(visit, layer->support_fills, mix(id, uint64_t(scale_(layer->print_z))));
            if (point_count > 400000) return std::nullopt;
        }
    }
    if (signatures.empty()) return std::nullopt;
    std::sort(signatures.begin(), signatures.end());
    return std::accumulate(signatures.begin(), signatures.end(), uint64_t(signatures.size()), mix);
}

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
        std::function<bool()> cancellation_requested,
        std::function<void()> expired,
        std::function<void()> canceled)
        : m_thread([this, deadline, cancellation_requested = std::move(cancellation_requested),
                    expired = std::move(expired), canceled = std::move(canceled)] {
              std::unique_lock<std::mutex> lock(m_mutex);
              while (!m_finished) {
                  if (cancellation_requested()) {
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

void OrcaTrialSliceExecutor::prepare_session_input(
    OrcaTrialSliceInput input, const std::vector<SliceCandidate>& candidates)
{
    std::vector<PreparedParameters> prepared;
    std::vector<ModelObject*> targets;
    for (auto* object : input.model.objects)
        if (object != nullptr && object->printable && std::any_of(object->instances.begin(), object->instances.end(),
                [](const ModelInstance* instance) { return instance != nullptr && instance->printable; }))
            targets.push_back(object);
    for (const auto& candidate : candidates) {
        if (candidate.parameters.entries.empty())
            continue;
        PreparedParameters parameters;
        parameters.candidate = candidate;
        parameters.result = OrcaParameterProposalAdapter().prepare_object_patches(
            candidate.parameters, input.plate_id, input.config, targets, parameters.patches);
        prepared.push_back(std::move(parameters));
    }
    std::lock_guard<std::mutex> lock(m_session_mutex);
    m_prepared_parameters = std::move(prepared);
    m_session_input = std::move(input);
    m_cancel_requested.store(false, std::memory_order_release);
    m_timed_out.store(false, std::memory_order_release);
}

void OrcaTrialSliceExecutor::clear_session_input()
{
    std::lock_guard<std::mutex> lock(m_session_mutex);
    m_session_input.reset();
    m_prepared_parameters.clear();
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

bool OrcaTrialSliceExecutor::prepare_orientation(OrcaTrialSliceInput& input, OrcaTrialSliceDetails& preview,
                                                 const std::function<bool()>& canceled)
{
    if (canceled() || input.has_user_marked_surface || input.model.objects.size() != 1 ||
        input.config.opt_bool("belt_printer")) return false;
    auto* object = input.model.objects.front();
    if (object->instances.size() != 1 || object->has_custom_layering()) return false;
    if (std::any_of(input.intent_constraints.records.begin(), input.intent_constraints.records.end(),
        [](const auto& record) {
            if (!record.is_hard_constraint()) return false;
            using Type = IntentConstraintType;
            return record.type == Type::PlatePlacementLock || record.type == Type::ObjectPlacementLock ||
                record.type == Type::InstancePlacementLock || record.type == Type::SupportPainting ||
                record.type == Type::SeamPainting || record.type == Type::ModifierVolume;
        })) return false;

    auto* instance = object->instances.front();
    auto mesh = object->mesh();
    if (mesh.facets_count() == 0) return false;
    // Reuse Orca's normal/hull direction search. Keep the candidate in a
    // modest tilt range to preserve the model's front, base and build height.
    orientation::OrientMesh orientable;
    orientable.overhang_angle = input.config.opt_int("support_threshold_angle");
    orientable.mesh = mesh;
    orientation::OrientMeshs items;
    items.push_back(std::move(orientable));
    orientation::OrientParams params;
    params.min_volume = true;
    params.stopcondition = canceled;
    params.progressind = [](unsigned, std::string) {};
    orientation::orient(items, {}, params);
    if (canceled()) return false;
    Matrix3d rotation = items.front().rotation_matrix;
    Eigen::AngleAxisd angle(rotation);
    const auto small_tilt = [](double radians) {
        return std::isfinite(radians) && radians >= 0.035 && radians <= 1.05;
    };
    if (!small_tilt(angle.angle())) {
        // Native orientation can favor a completely different face of a
        // symmetric model. Try a nearby downward plane within the rotation
        // limit, then run the same contact and native slicing checks.
        std::map<std::array<int, 3>, std::pair<double, Vec3d>> planes;
        for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
            if ((i & 4095) == 0 && canceled()) return false;
            const auto& face = mesh.its.indices[i];
            const auto& a = mesh.its.vertices[face.x()];
            const auto& b = mesh.its.vertices[face.y()];
            const auto& c = mesh.its.vertices[face.z()];
            const Vec3d normal = (b - a).cross(c - a).cast<double>();
            const double area2 = normal.norm();
            if (area2 <= 1e-12) continue;
            const Vec3d unit = normal / area2;
            if (!small_tilt(std::acos(std::clamp(-unit.z(), -1., 1.)))) continue;
            const std::array<int, 3> key{int(std::lround(unit.x() * 1000)),
                int(std::lround(unit.y() * 1000)), int(std::lround(unit.z() * 1000))};
            auto& plane = planes[key];
            plane.first += area2;
            plane.second = unit;
        }
        if (planes.empty()) return false;
        const auto best = std::max_element(planes.begin(), planes.end(),
            [](const auto& a, const auto& b) { return a.second.first < b.second.first; });
        rotation = Eigen::Quaterniond::FromTwoVectors(best->second.second, -Vec3d::UnitZ()).toRotationMatrix();
        angle = Eigen::AngleAxisd(rotation);
        if (!small_tilt(angle.angle())) return false;
    }
    const auto original = instance->get_matrix();
    instance->rotate(rotation);
    object->invalidate_bounding_box();
    object->ensure_on_bed();
    const auto box = object->instance_bounding_box(*instance);
    if (!box.defined || std::abs(box.min.z()) > 0.02 ||
        box.max.z() > input.config.opt_float("printable_height")) return false;
    const auto* bed = input.config.option<ConfigOptionPoints>("printable_area");
    if (!bed || bed->values.size() < 3) return false;
    auto hull = object->convex_hull_2d(instance->get_matrix());
    hull.translate(Point::new_scale(-input.plate_origin.x(), -input.plate_origin.y()));
    if (hull.empty() || !diff(Polygons{hull}, Polygons{Polygon::new_scale(bed->values)}).empty()) return false;
    // Conservatively exclude a tilt whose XY envelope crosses a keep-out
    // volume at a height the reoriented object occupies.
    for (const auto& region : get_bed_excluded_regions(input.config)) {
        if (region.z_max <= box.min.z() || (region.has_z_range && region.z_min >= box.max.z()))
            continue;
        if (!intersection(Polygons{hull}, Polygons{region.polygon}).empty()) return false;
    }
    // A point or edge contact is not an eligible alternative for the default
    // workbench. Assess the first-layer band without changing the mesh.
    double contact_area = 0.;
    const auto transformed = instance->get_matrix();
    const auto original_inv = original.inverse();
    const Transform3d relative = transformed * original_inv;
    const double first_layer = input.config.opt_float("initial_layer_print_height");
    const size_t stride = std::max<size_t>(1, mesh.its.indices.size() / 2500);
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        if (i % 4096 == 0 && canceled()) return false;
        const auto& face = mesh.its.indices[i];
        std::array<Vec3f, 3> before, after;
        for (size_t corner = 0; corner < 3; ++corner) {
            before[corner] = mesh.its.vertices[face[corner]];
            after[corner] = (relative * before[corner].cast<double>()).cast<float>();
        }
        if (std::max({after[0].z(), after[1].z(), after[2].z()}) <= box.min.z() + first_layer)
            contact_area += 0.5 * std::abs((after[1] - after[0]).cross(after[2] - after[0]).z());
        if (i % stride == 0) {
            preview.original_preview.push_back(before);
            preview.oriented_preview.push_back(after);
        }
    }
    if (contact_area < std::max(4., box.size().x() * box.size().y() * 0.01)) return false;
    ObjectTransform transform;
    transform.object_id = object->id().id;
    transform.instance_id = instance->id().id;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            transform.matrix[size_t(row * 4 + col)] = transformed.matrix()(row, col);
    preview.placement.transforms.push_back(std::move(transform));
    preview.orientation_degrees = angle.angle() * 180. / PI;
    return true;
}

// Normalize with the same intent constraints before comparing reusable native
// inputs and before creating a trial. The comparison never starts a Print.
static bool prepare_recommendation_config(const OrcaTrialSliceInput& input,
    const SliceRecommendation& recommendation, DynamicPrintConfig& effective_config,
    bool& adaptive, std::string& diagnostic, OrcaTrialSliceDetails* details)
{
    if (!SmartSliceApplier::BuildConfig(input.config, recommendation, effective_config, &diagnostic))
        return false;
    bool preserve_layers = input.plate_overrides.has("layer_height");
    const auto plate_keys = input.plate_overrides.keys();
    bool preserve_support = std::any_of(plate_keys.begin(), plate_keys.end(), [](const auto& key) {
        return key == "enable_support" || key.find("support_") == 0;
    });
    for (const auto& record : input.intent_constraints.records) {
        if (!record.is_hard_constraint()) continue;
        preserve_layers |= record.type == IntentConstraintType::LayerHeightRange ||
            record.type == IntentConstraintType::VariableLayerHeight;
        preserve_support |= record.type == IntentConstraintType::SupportPainting;
        preserve_support |= std::any_of(record.parameter_keys.begin(), record.parameter_keys.end(),
            [](const auto& key) { return key == "enable_support" || key.find("support_") == 0; });
    }
    // Organic tree support cannot use an existing variable layer profile.
    // If support is being recommended, choose a compatible style before the
    // single native trial. Explicit user support overrides remain authoritative.
    if (!preserve_support && !recommendation.keep_current_process && effective_config.opt_bool("enable_support")) {
        const auto* type = effective_config.option<ConfigOptionEnum<SupportType>>("support_type");
        const auto* style = effective_config.option("support_style");
        const bool organic = type && is_tree(type->value) &&
            (!style || style->getInt() == smsDefault || style->getInt() == smsTreeOrganic);
        if (organic && std::any_of(input.model.objects.begin(), input.model.objects.end(),
                [](const ModelObject* object) { return object->has_custom_layering(); })) {
            effective_config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
        }
    }
    if (details) details->preserves_manual_settings = preserve_layers || preserve_support;
    for (const auto& key : input.config.diff(effective_config)) {
        if (input.plate_overrides.has(key) || (preserve_layers && key == "layer_height") ||
            (preserve_support && (key == "enable_support" || key.find("support_") == 0))) {
            effective_config.set_key_value(key, input.config.option(key)->clone());
        } else if (details) {
            if (const auto* value = effective_config.option(key)) details->plate_patch.set_key_value(key, value->clone());
        }
    }
    adaptive = recommendation.adaptive_layer_height && !preserve_layers;
    return true;
}

bool OrcaTrialSliceExecutor::same_native_recommendation_input(const OrcaTrialSliceInput& input,
    const SliceRecommendation& lhs, const SliceRecommendation& rhs)
{
    DynamicPrintConfig first, second;
    std::string diagnostic;
    bool first_adaptive = false, second_adaptive = false;
    return prepare_recommendation_config(input, lhs, first, first_adaptive, diagnostic, nullptr) &&
           prepare_recommendation_config(input, rhs, second, second_adaptive, diagnostic, nullptr) &&
           first_adaptive == second_adaptive && first.diff(second).empty() && second.diff(first).empty();
}

TrialSliceResult OrcaTrialSliceExecutor::execute_recommendation_slice(
    OrcaTrialSliceInput input, const SliceRecommendation& recommendation,
    const WorkspaceRevision& base_revision, OrcaTrialSliceDetails& details,
    const OrcaTrialSliceControl& control)
{
    details = {};
    TrialSliceResult result;
    result.candidate_id = "smart-slice-preview-" + std::string(slice_mode_id(recommendation.mode));
    result.base_revision = base_revision;

    DynamicPrintConfig effective_config;
    std::string diagnostic;
    bool adaptive = false;
    if (!prepare_recommendation_config(input, recommendation, effective_config, adaptive, diagnostic, &details)) {
        result.diagnostic_code = diagnostic.empty() ? "smart_slice_preview_config_failed" : std::move(diagnostic);
        return result;
    }
    SliceCandidate candidate;
    candidate.id = result.candidate_id;
    candidate.base_revision = base_revision;
    return execute_trial_slice_impl(candidate, nullptr, {}, nullptr, nullptr,
                                    std::move(input), std::move(effective_config), &details,
                                    adaptive, control);
}

TrialSliceResult OrcaTrialSliceExecutor::execute_trial_slice_impl(
    const SliceCandidate& candidate,
    const TrialSliceTask* versioned_task,
    const std::shared_ptr<const RecommendationCancellationToken>& cancellation_token,
    TrialMetrics* trial_metrics,
    TrialEvaluationFacts* evaluation_facts,
    std::optional<OrcaTrialSliceInput> supplied_input,
    std::optional<DynamicPrintConfig> override_config,
    OrcaTrialSliceDetails* details, bool request_adaptive_layers, const OrcaTrialSliceControl& control)
{
    TrialSliceResult result;
    result.candidate_id  = candidate.id;
    result.base_revision = candidate.base_revision;
    try {
        OrcaTrialSliceInput input;
        bool prepared_session = false;
        std::optional<PreparedParameters> prepared_parameters;
        if (supplied_input) {
            input = std::move(*supplied_input);
        } else {
            std::lock_guard<std::mutex> lock(m_session_mutex);
            if (m_session_input) {
                input = *m_session_input;
                prepared_session = true;
                if (versioned_task == nullptr) {
                    for (const auto& prepared : m_prepared_parameters) {
                        if (prepared.candidate.id != candidate.id ||
                            prepared.candidate.base_revision != candidate.base_revision ||
                            prepared.candidate.goal != candidate.goal ||
                            prepared.candidate.parameters.goal != candidate.parameters.goal ||
                            prepared.candidate.parameters.policy_version != candidate.parameters.policy_version ||
                            prepared.candidate.parameters.entries.size() != candidate.parameters.entries.size())
                            continue;
                        bool matches = true;
                        for (size_t i = 0; i < candidate.parameters.entries.size(); ++i) {
                            const auto& a = prepared.candidate.parameters.entries[i];
                            const auto& b = candidate.parameters.entries[i];
                            if (a.scope != b.scope || a.owner != b.owner || a.target_id != b.target_id ||
                                a.key != b.key || a.expected_value != b.expected_value || a.new_value != b.new_value)
                                matches = false;
                        }
                        if (matches)
                            prepared_parameters = prepared;
                    }
                }
            } else if (versioned_task != nullptr) {
                result.diagnostic_code = "versioned_trial_session_input_unavailable";
                return result;
            }
        }
        if (supplied_input || !prepared_session) {
            m_cancel_requested.store(false, std::memory_order_release);
            if (!supplied_input)
                input = m_input_provider();
        }
        m_timed_out.store(false, std::memory_order_release);
        const auto local_deadline = std::chrono::steady_clock::now() + m_maximum_duration;
        auto effective_deadline = std::min(local_deadline, control.deadline);
        if (versioned_task)
            effective_deadline = std::min(effective_deadline, versioned_task->deadline);
        const auto cancellation_requested = [&] {
            return (cancellation_token && cancellation_token->cancellation_requested()) ||
                   (control.canceled && control.canceled());
        };
        if (cancellation_requested() || m_cancel_requested.load(std::memory_order_acquire)) {
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
            effective_deadline, cancellation_requested,
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
        if (override_config) {
            input.config = std::move(*override_config);
        } else if (!candidate.parameters.entries.empty()) {
            if (versioned_task != nullptr) {
                DynamicPrintConfig patched_config;
                const OrcaParameterApplyResult parameter_result = OrcaParameterProposalAdapter().validate_and_apply(
                    candidate.parameters, input.plate_id, input.config, input.intent_constraints,
                    input.profile_bounds, patched_config);
                if (!parameter_result.accepted) {
                    result.diagnostic_code = parameter_result.diagnostic_code;
                    return result;
                }
                input.config = std::move(patched_config);
            } else {
                const auto domain = ParameterProposalValidator().validate(candidate.parameters);
                if (!domain.accepted()) {
                    result.diagnostic_code = parameter_rejection_code_name(domain.rejections.front().code);
                    return result;
                }
                if (!prepared_parameters) {
                    result.diagnostic_code = "parameter_trial_input_not_prepared";
                    return result;
                }
                if (!prepared_parameters->result.accepted) {
                    result.diagnostic_code = prepared_parameters->result.diagnostic_code;
                    return result;
                }
                const auto applied = OrcaParameterProposalAdapter().apply_object_patches(
                    input.model, prepared_parameters->patches);
                if (!applied.accepted) {
                    result.diagnostic_code = applied.diagnostic_code;
                    return result;
                }
            }
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
        DynamicPrintConfig effective_config = input.config;

        Print trial_print;
        ActivePrintGuard active_print(*this, trial_print);
        std::atomic<bool> missing_support{false};
        trial_print.set_status_callback([&](const PrintBase::SlicingStatus& status) {
            if (status.message_type == PrintStateBase::SlicingNeedSupportOn)
                missing_support.store(true, std::memory_order_relaxed);
        });
        trial_print.set_plate_index(input.plate_index);
        trial_print.set_plate_name(input.plate_name);
        trial_print.set_plate_origin(input.plate_origin);
        trial_print.set_check_multi_filaments_compatibility(input.check_filament_compatibility);
        trial_print.set_extruder_filament_info(input.extruder_filament_info);
        const auto* filament_mapping = input.config.option<ConfigOptionInts>("filament_map");
        const std::vector<int> expected_filament_mapping = filament_mapping != nullptr ? filament_mapping->values :
                                                                                    std::vector<int>{};
        const bool prime_tower_enabled = input.config.opt_bool("enable_prime_tower");
        trial_print.apply(input.model, std::move(input.config));
        // The first apply initializes print regions. When the configured
        // tower depends on different region tools, resolve it once more now
        // that the same tool assignments as the background Print are known.
        if (details && prime_tower_enabled && !trial_print.has_wipe_tower())
            trial_print.apply(input.model, effective_config);
        if (details && request_adaptive_layers && trial_print.objects().size() == 1 &&
            input.model.objects.size() == 1 && input.model.objects.front()->instances.size() == 1) {
            auto* target = input.model.objects.front();
            const auto* printable = trial_print.objects().front();
            const auto& object_config = printable->config();
            const bool organic_support = printable->has_support_material() &&
                is_tree(object_config.support_type.value) &&
                (object_config.support_style.value == smsDefault || object_config.support_style.value == smsTreeOrganic);
            if (!target->has_custom_layering() && !organic_support && !trial_print.config().belt_printer.value) {
                const auto& params = printable->slicing_parameters();
                auto profile = layer_height_profile_adaptive(params, *target, 0.32f);
                if (profile.size() >= 4 && !check_object_layers_fixed(params, profile)) {
                    target->layer_height_profile.set(profile);
                    details->layer_profiles.emplace_back(target->id().id, std::move(profile));
                    trial_print.apply(input.model, effective_config);
                }
            }
        }

        std::vector<StringObjectException> validation_warnings;
        auto validation_error = trial_print.validate(&validation_warnings);
        bool no_estimated_tower_position = false;
        if (details && input.automatic_tower && control.allow_estimated_tower_relocation &&
            !validation_error.string.empty() && validation_error.opt_key == "wipe_tower_x") {
            // This correction only repeats preflight on the trial copy. The
            // full retry stays available for the tower built by Print::process.
            const auto position = trial_print.find_wipe_tower_position();
            no_estimated_tower_position = !position;
            if (position) {
                if (trial_print.canceled()) throw CanceledException();
                const ConfigOptionFloat x(position->x()), y(position->y());
                effective_config.option<ConfigOptionFloats>("wipe_tower_x")->set_at(&x, input.plate_index, 0);
                effective_config.option<ConfigOptionFloats>("wipe_tower_y")->set_at(&y, input.plate_index, 0);
                BOOST_LOG_TRIVIAL(info) << "[SmartSlicingTower] estimated preflight relocation plate="
                    << input.plate_index << " position=" << position->transpose();
                trial_print.apply(input.model, effective_config);
                validation_warnings.clear();
                validation_error = trial_print.validate(&validation_warnings);
            }
        }
        if (!validation_error.string.empty()) {
            result.diagnostic_code = "trial_validation_failed";
            result.diagnostic_message = validation_error.string;
            if (details && validation_error.opt_key == "wipe_tower_x")
                result.diagnostic_code = no_estimated_tower_position ?
                    "trial_tower_no_clear_position" : "trial_tower_position";
            return result;
        }

        try {
            trial_print.process();
        } catch (const WipeTowerPlacementError& error) {
            if (!details) throw;
            if (input.automatic_tower) details->tower_retry_position = trial_print.find_wipe_tower_position();
            result.diagnostic_code = input.automatic_tower && !details->tower_retry_position ?
                "trial_tower_no_clear_position" : "trial_tower_position";
            result.diagnostic_message = error.what();
            return result;
        }
        if (details && missing_support.load(std::memory_order_relaxed)) {
            result.diagnostic_code = "trial_support_needed";
            return result;
        }
        if (details) {
            if (trial_print.wipe_tower_data().wipe_tower_mesh_data) {
                const auto& config = trial_print.config();
                const Vec2d current(config.wipe_tower_x.get_at(input.plate_index),
                                    config.wipe_tower_y.get_at(input.plate_index));
                auto geometry = trial_print.wipe_tower_position_geometry();
                const auto status = geometry.check(current);
                if (status != WipeTowerPositionStatus::Valid) {
                    if (input.automatic_tower) details->tower_retry_position = trial_print.find_wipe_tower_position();
                    result.diagnostic_code = input.automatic_tower && !details->tower_retry_position ?
                        "trial_tower_no_clear_position" : tower_position_diagnostic(status);
                    return result;
                }
                details->tower_position = current;
                details->tower_geometry = std::move(geometry);
            }
            double minimum = std::numeric_limits<double>::max();
            double maximum = 0.;
            double total = 0.;
            for (const auto* object : trial_print.objects()) {
                const auto& config = object->config();
                const auto* model = object->model_object();
                if (details->effective_layer_height == 0.) details->effective_layer_height = config.layer_height.value;
                else if (!is_approx(details->effective_layer_height, config.layer_height.value))
                    details->mixed_nominal_layer_heights = true;
                const std::string support = !config.enable_support.value ? "none" :
                    is_tree(config.support_type.value) ? "tree" : "normal";
                if (details->support_type.empty()) details->support_type = support;
                else if (details->support_type != support) details->support_type = "mixed";
                if (trial_print.objects().size() == 1 && config.enable_support.value) {
                    details->support_angle = config.support_threshold_angle.value;
                    details->plate_only_support = config.support_on_build_plate_only.value;
                }
                const auto profile = model->layer_height_profile.get();
                details->variable_layers |= profile.size() >= 4 && profile.size() % 2 == 0 &&
                    profile.front() == 0. && !check_object_layers_fixed(object->slicing_parameters(), profile);
                for (const auto* layer : object->layers()) {
                    minimum = std::min(minimum, double(layer->height));
                    maximum = std::max(maximum, double(layer->height));
                    total += layer->height;
                    ++details->layer_count;
                }
            }
            if (trial_print.objects().size() == 1 && details->support_type == "normal")
                details->support_interface_signature = support_interface_signature(trial_print);
            if (maximum > 0.) details->layer_range = std::make_pair(minimum, maximum);
            if (details->layer_count) details->mean_layer_height = total / details->layer_count;
        }
        ScopedTrialGCode temporary_gcode;
        GCodeProcessorResult gcode_result;
        temporary_gcode.set_actual_path(trial_print.export_gcode(temporary_gcode.requested_path(), &gcode_result, nullptr));
        if (temporary_gcode.exceeds(m_maximum_temporary_disk_bytes)) {
            result.diagnostic_code = "workflow_disk_budget_exceeded";
            return result;
        }
        result.metrics = extract_metrics(gcode_result, expected_filament_mapping, prime_tower_enabled);
        if (details && gcode_result.toolpath_outside) {
            result.metrics.reset();
            result.diagnostic_code = "trial_toolpath_outside";
            return result;
        }
        const auto& stats = gcode_result.print_statistics;
        result.metrics->material_grams = gcode_result.material_weight_grams(stats.total_volumes_per_extruder);
        if (details) {
            details->support_grams = gcode_result.material_weight_grams(stats.support_volumes_per_extruder);
            details->flush_grams = gcode_result.material_weight_grams(stats.flush_per_filament);
            details->tower_grams = gcode_result.material_weight_grams(stats.wipe_tower_volumes_per_extruder);
        }
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
    } catch (const SlicingError& error) {
        result.status             = TrialSliceStatus::Failed;
        result.diagnostic_code    = "trial_validation_failed";
        result.diagnostic_message = error.what();
    } catch (const std::exception& error) {
        result.status          = TrialSliceStatus::Failed;
        result.diagnostic_code = "trial_slice_exception";
        result.diagnostic_message = error.what();
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
