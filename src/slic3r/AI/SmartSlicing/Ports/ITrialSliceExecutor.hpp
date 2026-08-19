#pragma once

#include "slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp"

namespace Slic3r::AI::SmartSlicing {

class ITrialSliceExecutor
{
public:
    virtual ~ITrialSliceExecutor()                                              = default;
    virtual SlicingMetrics execute_trial_slice(const SliceCandidate& candidate) = 0;
    virtual void cancel_trial_slice()                                           = 0;
};

} // namespace Slic3r::AI::SmartSlicing
