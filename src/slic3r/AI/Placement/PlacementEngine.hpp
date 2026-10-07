#pragma once
#include "libslic3r/Arrange.hpp"
#include <memory>
#include <string>
#include <vector>

namespace Slic3r::AI::Placement {
// Owned silhouettes and constraints. The dispatcher removes every model setter
// before the engine sees this snapshot. Only progress/cancellation callbacks remain.
struct Request {
    arrangement::ArrangePolygons selected, fixed;
    Points bed;
    arrangement::ArrangeParams params;
};
struct Transform {
    int item_id {0};
    Vec2crd translation {0, 0};
    double rotation {0};
    int bed_index {arrangement::UNARRANGED}, priority {0};
};
struct Result {
    std::vector<Transform> transforms;
    bool canceled {false};
    std::string algorithm_id, algorithm_version;
};
class IPlacementEngine {
public:
    virtual ~IPlacementEngine() = default;
    virtual const char* algorithm_id() const noexcept = 0;
    virtual const char* algorithm_version() const noexcept = 0;
    // Engines must honor bed, excluded/fixed regions, spacing and rotation policy.
    // They return proposals and never apply transformations to a live model.
    virtual Result plan(Request request) const = 0;
};
std::shared_ptr<const IPlacementEngine> baseline_engine();
Result arrange(arrangement::ArrangePolygons& selected,
    const arrangement::ArrangePolygons& fixed, const Points& bed,
    const arrangement::ArrangeParams& params,
    std::shared_ptr<const IPlacementEngine> engine = {});
} // namespace Slic3r::AI::Placement
