#pragma once

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/AI/SmartSlicing/Application/LocalModelFeatureAnalyzer.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

namespace GUI {

class Plater;

struct OrcaModelFeatureTarget
{
    std::optional<uint64_t> object_id;
    std::optional<uint64_t> volume_id;
    std::optional<uint64_t> instance_id;
};

// Owner-stage view. The mesh pointer is consumed synchronously and is never
// retained in captured output or passed to a worker.
struct OrcaModelFeatureCaptureSource
{
    uint64_t object_id{0};
    uint64_t volume_id{0};
    uint64_t instance_id{0};
    std::string geometry_fingerprint;
    const indexed_triangle_set* mesh{nullptr};
    Transform3d transform{Transform3d::Identity()};
};

struct OrcaModelFeatureInput
{
    uint64_t instance_id{0};
    AI::SmartSlicing::TriangleMeshSnapshot mesh;
};

enum class OrcaModelFeatureCaptureStatus {
    Completed,
    Canceled,
    ResourceLimitExceeded,
    InvalidInput,
    NoMatchingGeometry,
};

struct OrcaModelFeatureCaptureResult
{
    OrcaModelFeatureCaptureStatus status{OrcaModelFeatureCaptureStatus::InvalidInput};
    std::vector<OrcaModelFeatureInput> inputs;
    std::string diagnostic_code;
    size_t accounted_memory_bytes{0};

    bool completed() const { return status == OrcaModelFeatureCaptureStatus::Completed; }
};

struct OrcaModelFeatureOutput
{
    uint64_t instance_id{0};
    AI::SmartSlicing::ModelFeatureSnapshot features;
};

struct OrcaModelFeatureBatchResult
{
    AI::SmartSlicing::ModelFeatureAnalysisStatus status{
        AI::SmartSlicing::ModelFeatureAnalysisStatus::InvalidInput};
    std::vector<OrcaModelFeatureOutput> outputs;
    std::string diagnostic_code;
    size_t peak_accounted_memory_bytes{0};

    bool completed() const
    {
        return status == AI::SmartSlicing::ModelFeatureAnalysisStatus::Completed;
    }
};

class OrcaModelFeatureAnalyzer
{
public:
    static OrcaModelFeatureCaptureResult capture_sources(
        const std::vector<OrcaModelFeatureCaptureSource>& sources,
        const OrcaModelFeatureTarget& target,
        const AI::SmartSlicing::ModelFeatureAnalysisLimits& limits);

    static OrcaModelFeatureCaptureResult capture_current_plate(
        Plater& plater,
        const OrcaModelFeatureTarget& target,
        const AI::SmartSlicing::ModelFeatureAnalysisLimits& limits);

    OrcaModelFeatureBatchResult analyze_captured(
        const std::vector<OrcaModelFeatureInput>& inputs,
        const AI::SmartSlicing::ModelFeatureAnalysisLimits& limits) const;

private:
    AI::SmartSlicing::LocalModelFeatureAnalyzer m_analyzer;
};

} // namespace GUI
} // namespace Slic3r
