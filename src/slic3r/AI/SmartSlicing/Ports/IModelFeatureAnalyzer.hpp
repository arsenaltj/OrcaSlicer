#pragma once

#include "slic3r/AI/SmartSlicing/Domain/ModelFeatureSnapshot.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace Slic3r::AI::SmartSlicing {

enum class ModelFeatureAnalysisStatus { Completed, Canceled, ResourceLimitExceeded, InvalidInput };

struct ModelFeatureAnalysisLimits
{
    size_t maximum_working_memory_bytes{512ull * 1024ull * 1024ull};
    std::function<bool()> cancellation_requested;
};

struct ModelFeatureAnalysisResult
{
    ModelFeatureAnalysisStatus status{ModelFeatureAnalysisStatus::InvalidInput};
    std::optional<ModelFeatureSnapshot> snapshot;
    std::string diagnostic_code;
    size_t peak_accounted_memory_bytes{0};

    bool completed() const
    {
        return status == ModelFeatureAnalysisStatus::Completed && snapshot.has_value();
    }
};

class IModelFeatureAnalyzer
{
public:
    virtual ~IModelFeatureAnalyzer() = default;
    virtual ModelFeatureAnalysisResult analyze(const TriangleMeshSnapshot& mesh,
                                               const ModelFeatureAnalysisLimits& limits) const = 0;
};

const char* model_feature_analysis_status_name(ModelFeatureAnalysisStatus status);

} // namespace Slic3r::AI::SmartSlicing
