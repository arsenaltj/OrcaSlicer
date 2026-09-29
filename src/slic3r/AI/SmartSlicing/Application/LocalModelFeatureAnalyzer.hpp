#pragma once

#include "slic3r/AI/SmartSlicing/Ports/IModelFeatureAnalyzer.hpp"

#include <array>
#include <string>

namespace Slic3r::AI::SmartSlicing {

struct ModelFeatureAnalysisPolicy
{
    std::string version{"model-feature-policy/v1"};
    double scalar_epsilon{1e-9};
    double degenerate_area_epsilon_mm2{1e-10};
    double bed_plane_tolerance_mm{1e-5};
    double bed_facing_minimum_cosine{0.98};
    std::array<double, 3> overhang_maximum_angle_from_down_degrees{30.0, 55.0, 75.0};
    double bridge_facing_minimum_cosine{0.98};
    double bridge_minimum_span_mm{1.0};
    double thin_wall_global_extent_maximum_mm{0.8};

    bool valid() const;
};

class LocalModelFeatureAnalyzer final : public IModelFeatureAnalyzer
{
public:
    static constexpr const char* ANALYSIS_VERSION = "local-triangle-model-features/v1";

    explicit LocalModelFeatureAnalyzer(ModelFeatureAnalysisPolicy policy = {});

    ModelFeatureAnalysisResult analyze(const TriangleMeshSnapshot& mesh,
                                       const ModelFeatureAnalysisLimits& limits) const override;
    const ModelFeatureAnalysisPolicy& policy() const { return m_policy; }

private:
    ModelFeatureAnalysisPolicy m_policy;
};

} // namespace Slic3r::AI::SmartSlicing
