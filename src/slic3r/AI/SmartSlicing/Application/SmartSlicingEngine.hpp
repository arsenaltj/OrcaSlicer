#pragma once

#include "slic3r/AI/SmartSlicing/Domain/SmartSliceTypes.hpp"
#include "slic3r/AI/SmartSlicing/Domain/ModelFeatureSnapshot.hpp"

namespace Slic3r {
class ModelObject;
}

namespace Slic3r::AI::SmartSlicing {

// Pure, deterministic rule engine.  Native ModelObject capture is implemented
// by the GUI adapter; the worker-facing overload consumes only immutable DTOs.
class SmartSlicingEngine
{
public:
    SmartAnalysisResult AnalyzeAndPrecompute(const ModelMetrics& metrics,
                                             const MachinePreset& machine,
                                             const FilamentPreset& filament) const;

    // Worker-safe overload for a captured immutable mesh.  The GUI adapter is
    // responsible for taking the snapshot on the owner thread; this overload
    // performs all geometry reads on the worker and never touches ModelObject.
    SmartAnalysisResult AnalyzeAndPrecompute(const TriangleMeshSnapshot& mesh,
                                             const MachinePreset& machine,
                                             const FilamentPreset& filament) const;

    // Aggregates every printable object on the current plate. A single input
    // delegates to the original overload so its established result is unchanged.
    SmartAnalysisResult AnalyzeAndPrecompute(const std::vector<TriangleMeshSnapshot>& meshes,
                                             const MachinePreset& machine,
                                             const FilamentPreset& filament) const;

    // Owner-thread convenience overload.  The implementation lives in the
    // Orca GUI target so the AI static library remains free of native GUI code.
    SmartAnalysisResult AnalyzeAndPrecompute(const ModelObject* model,
                                             const MachinePreset& machine,
                                             const FilamentPreset& filament) const;
};

} // namespace Slic3r::AI::SmartSlicing
