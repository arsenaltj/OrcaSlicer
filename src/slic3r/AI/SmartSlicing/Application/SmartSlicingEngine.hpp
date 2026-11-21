#pragma once

#include "slic3r/AI/SmartSlicing/Domain/SmartSliceTypes.hpp"
#include "slic3r/AI/SmartSlicing/Domain/ModelFeatureSnapshot.hpp"

namespace Slic3r {
class ModelObject;
}

namespace Slic3r::AI::SmartSlicing {

// Pure, deterministic rule engine.  Native ModelObject capture is implemented
// by the GUI adapter; the worker-facing overload consumes only immutable DTOs.
struct MeasuredSliceOption
{
    double seconds{0.};
    double grams{0.};
    double surface_error{0.};
    double support_grams{0.};
    bool baseline{false};
};

// Chooses from successfully validated trials only. The order of otherwise
// equal options is stable, with the current process preferred on ties.
std::optional<size_t> select_measured_slice_option(SliceMode goal,
    const std::vector<MeasuredSliceOption>& options);

class SmartSlicingEngine
{
public:
    ModelMetrics MeasureModel(const std::vector<TriangleMeshSnapshot>& meshes) const;
    SmartAnalysisResult AnalyzeGoal(const ModelMetrics& metrics, const MachinePreset& machine,
                                    const FilamentPreset& filament, SliceMode mode) const;

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
private:
    SmartAnalysisResult analyze(const ModelMetrics& metrics, const MachinePreset& machine,
                                const FilamentPreset& filament, std::optional<SliceMode> goal) const;
};

} // namespace Slic3r::AI::SmartSlicing
