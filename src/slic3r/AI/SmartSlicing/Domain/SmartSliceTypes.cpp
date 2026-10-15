#include "SmartSliceTypes.hpp"

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
    for (const SliceRecommendation& recommendation : recommendations)
        if (recommendation.mode == mode)
            return &recommendation;
    return nullptr;
}

SliceRecommendation* SmartAnalysisResult::find(SliceMode mode)
{
    for (SliceRecommendation& recommendation : recommendations)
        if (recommendation.mode == mode)
            return &recommendation;
    return nullptr;
}

} // namespace Slic3r::AI::SmartSlicing
