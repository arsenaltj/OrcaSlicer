#include "OrcaModelFeatureAnalyzer.hpp"

#include "libslic3r/Model.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <wx/thread.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Slic3r::GUI {
namespace {

using AI::SmartSlicing::MeshPoint3d;
using AI::SmartSlicing::MeshTriangle;
using AI::SmartSlicing::ModelFeatureAnalysisLimits;
using AI::SmartSlicing::ModelFeatureAnalysisStatus;
using AI::SmartSlicing::TriangleMeshSnapshot;

bool canceled(const ModelFeatureAnalysisLimits& limits)
{
    return limits.cancellation_requested && limits.cancellation_requested();
}

bool matches(const OrcaModelFeatureCaptureSource& source, const OrcaModelFeatureTarget& target)
{
    return (!target.object_id || *target.object_id == source.object_id) &&
           (!target.volume_id || *target.volume_id == source.volume_id) &&
           (!target.instance_id || *target.instance_id == source.instance_id);
}

bool add_size(size_t& total, size_t count, size_t item_size)
{
    if (count != 0 && item_size > std::numeric_limits<size_t>::max() / count)
        return false;
    const size_t bytes = count * item_size;
    if (bytes > std::numeric_limits<size_t>::max() - total)
        return false;
    total += bytes;
    return true;
}

OrcaModelFeatureCaptureResult capture_stopped(OrcaModelFeatureCaptureStatus status,
                                               std::string diagnostic)
{
    OrcaModelFeatureCaptureResult result;
    result.status = status;
    result.diagnostic_code = std::move(diagnostic);
    return result;
}

bool finite_transform(const Transform3d& transform)
{
    return transform.matrix().allFinite();
}

} // namespace

OrcaModelFeatureCaptureResult OrcaModelFeatureAnalyzer::capture_sources(
    const std::vector<OrcaModelFeatureCaptureSource>& sources,
    const OrcaModelFeatureTarget& target,
    const ModelFeatureAnalysisLimits& limits)
{
    if (canceled(limits))
        return capture_stopped(OrcaModelFeatureCaptureStatus::Canceled,
                               "model_feature_capture_canceled");

    OrcaModelFeatureCaptureResult result;
    result.status = OrcaModelFeatureCaptureStatus::Completed;
    for (const OrcaModelFeatureCaptureSource& source : sources) {
        if (canceled(limits))
            return capture_stopped(OrcaModelFeatureCaptureStatus::Canceled,
                                   "model_feature_capture_canceled");
        if (!matches(source, target))
            continue;
        if (source.object_id == 0 || source.volume_id == 0 || source.instance_id == 0 ||
            source.geometry_fingerprint.empty() || source.mesh == nullptr ||
            source.mesh->vertices.empty() || source.mesh->indices.empty() ||
            !finite_transform(source.transform))
            return capture_stopped(OrcaModelFeatureCaptureStatus::InvalidInput,
                                   "model_feature_capture_source_invalid");

        size_t input_bytes = source.geometry_fingerprint.size();
        if (!add_size(input_bytes, source.mesh->vertices.size(), sizeof(MeshPoint3d)) ||
            !add_size(input_bytes, source.mesh->indices.size(), sizeof(MeshTriangle)) ||
            input_bytes > limits.maximum_working_memory_bytes -
                              std::min(result.accounted_memory_bytes,
                                       limits.maximum_working_memory_bytes))
            return capture_stopped(OrcaModelFeatureCaptureStatus::ResourceLimitExceeded,
                                   "model_feature_capture_memory_budget_exceeded");

        OrcaModelFeatureInput input;
        input.instance_id = source.instance_id;
        input.mesh.object_id = source.object_id;
        input.mesh.volume_id = source.volume_id;
        input.mesh.geometry_fingerprint = source.geometry_fingerprint;
        input.mesh.vertices.reserve(source.mesh->vertices.size());
        input.mesh.triangles.reserve(source.mesh->indices.size());

        for (const Vec3f& vertex : source.mesh->vertices) {
            if (canceled(limits))
                return capture_stopped(OrcaModelFeatureCaptureStatus::Canceled,
                                       "model_feature_capture_canceled");
            const Vec3d transformed = source.transform * vertex.cast<double>();
            if (!transformed.allFinite())
                return capture_stopped(OrcaModelFeatureCaptureStatus::InvalidInput,
                                       "model_feature_capture_vertex_non_finite");
            input.mesh.vertices.push_back({transformed.x(), transformed.y(), transformed.z()});
        }
        for (const Vec3i32& triangle : source.mesh->indices) {
            if (canceled(limits))
                return capture_stopped(OrcaModelFeatureCaptureStatus::Canceled,
                                       "model_feature_capture_canceled");
            if ((triangle.array() < 0).any() ||
                static_cast<size_t>(triangle.maxCoeff()) >= source.mesh->vertices.size())
                return capture_stopped(OrcaModelFeatureCaptureStatus::InvalidInput,
                                       "model_feature_capture_triangle_invalid");
            input.mesh.triangles.push_back({{static_cast<uint32_t>(triangle.x()),
                                             static_cast<uint32_t>(triangle.y()),
                                             static_cast<uint32_t>(triangle.z())}});
        }
        result.accounted_memory_bytes += input_bytes;
        result.inputs.push_back(std::move(input));
    }

    if (result.inputs.empty())
        return capture_stopped(OrcaModelFeatureCaptureStatus::NoMatchingGeometry,
                               "model_feature_capture_no_matching_geometry");
    result.diagnostic_code = "model_feature_capture_completed";
    return result;
}

OrcaModelFeatureCaptureResult OrcaModelFeatureAnalyzer::capture_current_plate(
    Plater& plater,
    const OrcaModelFeatureTarget& target,
    const ModelFeatureAnalysisLimits& limits)
{
    if (!wxIsMainThread())
        return capture_stopped(OrcaModelFeatureCaptureStatus::InvalidInput,
                               "model_feature_capture_requires_gui_owner_thread");
    if ((target.object_id && *target.object_id == 0) ||
        (target.volume_id && *target.volume_id == 0) ||
        (target.instance_id && *target.instance_id == 0))
        return capture_stopped(OrcaModelFeatureCaptureStatus::InvalidInput,
                               "model_feature_capture_target_invalid");
    PartPlate* plate = plater.get_partplate_list().get_curr_plate();
    if (plate == nullptr)
        return capture_stopped(OrcaModelFeatureCaptureStatus::NoMatchingGeometry,
                               "model_feature_capture_current_plate_unavailable");

    std::vector<OrcaModelFeatureCaptureSource> sources;
    Model& model = plater.model();
    for (size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        if (canceled(limits))
            return capture_stopped(OrcaModelFeatureCaptureStatus::Canceled,
                                   "model_feature_capture_canceled");
        ModelObject* object = model.objects[object_index];
        if (object == nullptr || (target.object_id && object->id().id != *target.object_id))
            continue;
        for (size_t instance_index = 0; instance_index < object->instances.size(); ++instance_index) {
            if (canceled(limits))
                return capture_stopped(OrcaModelFeatureCaptureStatus::Canceled,
                                       "model_feature_capture_canceled");
            ModelInstance* instance = object->instances[instance_index];
            if (instance == nullptr ||
                (target.instance_id && instance->id().id != *target.instance_id) ||
                !plate->contain_instance(static_cast<int>(object_index),
                                         static_cast<int>(instance_index)))
                continue;
            for (ModelVolume* volume : object->volumes) {
                if (canceled(limits))
                    return capture_stopped(OrcaModelFeatureCaptureStatus::Canceled,
                                           "model_feature_capture_canceled");
                if (volume == nullptr || !volume->is_model_part() ||
                    (target.volume_id && volume->id().id != *target.volume_id))
                    continue;
                const indexed_triangle_set& mesh = volume->mesh().its;
                sources.push_back({object->id().id, volume->id().id, instance->id().id,
                                   AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh),
                                   &mesh, instance->get_matrix() * volume->get_matrix()});
            }
        }
    }
    return capture_sources(sources, target, limits);
}

OrcaModelFeatureBatchResult OrcaModelFeatureAnalyzer::analyze_captured(
    const std::vector<OrcaModelFeatureInput>& inputs,
    const ModelFeatureAnalysisLimits& limits) const
{
    OrcaModelFeatureBatchResult result;
    if (inputs.empty()) {
        result.diagnostic_code = "model_feature_batch_empty";
        return result;
    }

    size_t cumulative_peak = 0;
    for (const OrcaModelFeatureInput& input : inputs) {
        if (canceled(limits)) {
            result.status = ModelFeatureAnalysisStatus::Canceled;
            result.diagnostic_code = "model_feature_analysis_canceled";
            return result;
        }
        if (input.instance_id == 0) {
            result.diagnostic_code = "model_feature_instance_invalid";
            return result;
        }
        ModelFeatureAnalysisLimits item_limits = limits;
        item_limits.maximum_working_memory_bytes =
            limits.maximum_working_memory_bytes -
            std::min(cumulative_peak, limits.maximum_working_memory_bytes);
        auto analyzed = m_analyzer.analyze(input.mesh, item_limits);
        if (analyzed.peak_accounted_memory_bytes >
            limits.maximum_working_memory_bytes -
                std::min(cumulative_peak, limits.maximum_working_memory_bytes)) {
            result.status = ModelFeatureAnalysisStatus::ResourceLimitExceeded;
            result.diagnostic_code = "model_feature_batch_memory_budget_exceeded";
            return result;
        }
        cumulative_peak += analyzed.peak_accounted_memory_bytes;
        result.peak_accounted_memory_bytes = cumulative_peak;
        if (!analyzed.completed()) {
            result.status = analyzed.status;
            result.diagnostic_code = analyzed.diagnostic_code;
            result.outputs.clear();
            return result;
        }
        result.outputs.push_back({input.instance_id, std::move(*analyzed.snapshot)});
    }
    result.status = ModelFeatureAnalysisStatus::Completed;
    result.diagnostic_code = "model_feature_batch_completed";
    return result;
}

} // namespace Slic3r::GUI
