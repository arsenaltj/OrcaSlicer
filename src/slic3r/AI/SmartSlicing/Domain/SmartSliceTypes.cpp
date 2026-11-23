#include "SmartSliceTypes.hpp"
#include <utility>

namespace Slic3r::AI::SmartSlicing {

const char* slice_mode_id(SliceMode mode)
{
    switch (mode) {
    case SliceMode::BALANCED: return "balanced";
    case SliceMode::SPEED: return "speed";
    case SliceMode::QUALITY: return "quality";
    }
    return "unknown";
}

const SliceRecommendation* SmartAnalysisResult::find(SliceMode mode) const
{
    for (size_t i = 0; i < recommendations.size(); ++i)
        if (recommendation_available[i] && recommendations[i].mode == mode)
            return &recommendations[i];
    return nullptr;
}

SliceRecommendation* SmartAnalysisResult::find(SliceMode mode)
{
    return const_cast<SliceRecommendation*>(std::as_const(*this).find(mode));
    return nullptr;
}

} // namespace Slic3r::AI::SmartSlicing
