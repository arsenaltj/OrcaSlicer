#include "slic3r/AI/SmartSlicing/Application/SmartSlicingEngine.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r::AI::SmartSlicing {

SmartAnalysisResult SmartSlicingEngine::AnalyzeAndPrecompute(
    const ModelObject* model, const MachinePreset& machine, const FilamentPreset& filament) const
{
    if (model == nullptr)
        return {SmartAnalysisResult::CONTRACT_VERSION, false, "smart_slice_model_unavailable", {}, {}};
    const auto mesh = model->mesh();
    TriangleMeshSnapshot input;
    input.vertices.reserve(mesh.its.vertices.size());
    for (const auto& vertex : mesh.its.vertices)
        input.vertices.push_back({vertex.x(), vertex.y(), vertex.z()});
    input.triangles.reserve(mesh.its.indices.size());
    for (const auto& triangle : mesh.its.indices)
        input.triangles.push_back({{uint32_t(triangle.x()), uint32_t(triangle.y()), uint32_t(triangle.z())}});
    return AnalyzeAndPrecompute(input, machine, filament);
}

} // namespace Slic3r::AI::SmartSlicing
