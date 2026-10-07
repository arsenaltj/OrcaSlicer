#include "PlacementEngine.hpp"
#include <cmath>
#include <map>
#include <stdexcept>

namespace Slic3r::AI::Placement {
namespace {
void detach_setters(arrangement::ArrangePolygons& polygons)
{
    for (auto& polygon : polygons) polygon.setter = {};
}
class NativePlacementEngine final : public IPlacementEngine {
public:
    const char* algorithm_id() const noexcept override { return "native-arrange"; }
    const char* algorithm_version() const noexcept override { return "native-arrange-v1"; }
    Result plan(Request request) const override
    {
        Result result;
        arrangement::arrange(request.selected, request.fixed, request.bed, request.params);
        result.canceled = request.params.stopcondition && request.params.stopcondition();
        if (!result.canceled) {
            result.transforms.reserve(request.selected.size());
            for (const auto& polygon : request.selected)
                result.transforms.push_back({polygon.itemid, polygon.translation, polygon.rotation,
                                             polygon.bed_idx, polygon.priority});
        }
        return result;
    }
};
}
std::shared_ptr<const IPlacementEngine> baseline_engine()
{
    static const auto engine = std::make_shared<const NativePlacementEngine>();
    return engine;
}
Result arrange(arrangement::ArrangePolygons& selected, const arrangement::ArrangePolygons& fixed,
    const Points& bed, const arrangement::ArrangeParams& params,
    std::shared_ptr<const IPlacementEngine> engine)
{
    if (!engine) engine = baseline_engine();
    Result result;
    result.algorithm_id = engine->algorithm_id();
    result.algorithm_version = engine->algorithm_version();
    if (params.stopcondition && params.stopcondition()) {
        result.canceled = true;
        return result;
    }
    Request snapshot {selected, fixed, bed, params};
    detach_setters(snapshot.selected);
    detach_setters(snapshot.fixed);
    detach_setters(snapshot.params.excluded_regions);
    detach_setters(snapshot.params.nonprefered_regions);
    auto proposal = engine->plan(std::move(snapshot));
    proposal.algorithm_id = result.algorithm_id;
    proposal.algorithm_version = result.algorithm_version;
    proposal.canceled = proposal.canceled || (params.stopcondition && params.stopcondition());
    if (proposal.canceled) return proposal;

    if (proposal.transforms.size() != selected.size())
        throw std::runtime_error("Placement result has an incomplete object identity set.");
    std::map<int, const Transform*> by_id;
    for (const auto& transform : proposal.transforms)
        if (!std::isfinite(transform.rotation) || transform.bed_index < arrangement::UNARRANGED ||
            !by_id.emplace(transform.item_id, &transform).second)
            throw std::runtime_error("Placement result contains an invalid or duplicate object identity.");
    for (const auto& polygon : selected)
        if (by_id.count(polygon.itemid) == 0)
            throw std::runtime_error("Placement result targets a different object identity set.");
    // Preserve every source silhouette, material constraint and live setter.
    // The GUI job (or the copied-model candidate adapter) still owns application.
    for (auto& polygon : selected) {
        const auto& transform = *by_id.at(polygon.itemid);
        polygon.translation = transform.translation;
        polygon.rotation = transform.rotation;
        polygon.bed_idx = transform.bed_index;
        polygon.priority = transform.priority;
    }
    return proposal;
}
} // namespace Slic3r::AI::Placement
