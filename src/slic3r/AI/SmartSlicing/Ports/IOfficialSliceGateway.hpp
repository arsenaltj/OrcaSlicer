#pragma once

#include "slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp"

namespace Slic3r::AI::SmartSlicing {

class IOfficialSliceGateway
{
public:
    virtual ~IOfficialSliceGateway()                              = default;
    virtual bool apply_and_slice(const SliceCandidate& candidate) = 0;
};

} // namespace Slic3r::AI::SmartSlicing
