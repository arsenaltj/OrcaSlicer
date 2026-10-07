#include <catch2/catch_test_macros.hpp>
#include "slic3r/AI/Placement/PlacementEngine.hpp"

using namespace Slic3r;
namespace Placement = AI::Placement;
namespace {
class RecordingPlacement final : public Placement::IPlacementEngine {
public:
    mutable bool saw_setter {false};
    mutable size_t calls {0}, fixed_count {0}, excluded_count {0};
    bool invalid_identity {false}, cancel {false};
    const char* algorithm_id() const noexcept override { return "recording-placement"; }
    const char* algorithm_version() const noexcept override { return "recording-placement-v2"; }
    Placement::Result plan(Placement::Request request) const override
    {
        ++calls;
        fixed_count = request.fixed.size(); excluded_count = request.params.excluded_regions.size();
        auto inspect = [&](auto& polygons) {
            for (auto& polygon : polygons) {
                saw_setter = saw_setter || bool(polygon.setter);
                polygon.apply(); // Must not reach a model setter.
            }
        };
        inspect(request.selected); inspect(request.fixed);
        inspect(request.params.excluded_regions); inspect(request.params.nonprefered_regions);
        Placement::Result result;
        result.canceled = cancel;
        for (const auto& polygon : request.selected)
            result.transforms.push_back({invalid_identity ? 999 : polygon.itemid, Vec2crd(123, 456), 0, 0, 2});
        return result;
    }
};
}

TEST_CASE("Placement strategies see owned constraints and cannot apply a live model setter", "[PlacementEngine]")
{
    int applied = 0;
    arrangement::ArrangePolygon polygon;
    polygon.itemid = 7;
    polygon.setter = [&](const auto&) { ++applied; };
    arrangement::ArrangePolygons selected {polygon}, fixed {polygon};
    arrangement::ArrangeParams params;
    params.excluded_regions = {polygon}; params.nonprefered_regions = {polygon};
    const auto engine = std::make_shared<RecordingPlacement>();
    const auto result = Placement::arrange(selected, fixed, {}, params, engine);
    CHECK(engine->calls == 1);
    CHECK_FALSE(engine->saw_setter);
    CHECK(engine->fixed_count == 1);
    CHECK(engine->excluded_count == 1);
    CHECK(applied == 0);
    CHECK(fixed.front().translation == Vec2crd(0, 0));
    CHECK(selected.front().translation == Vec2crd(123, 456));
    CHECK(result.algorithm_version == "recording-placement-v2");
    selected.front().apply();
    CHECK(applied == 1); // Application remains an explicit consumer operation.
}

TEST_CASE("Invalid or canceled placement proposals leave every source transform unchanged", "[PlacementEngine]")
{
    arrangement::ArrangePolygon polygon;
    polygon.itemid = 7;
    arrangement::ArrangePolygons selected {polygon};
    auto engine = std::make_shared<RecordingPlacement>();
    engine->invalid_identity = true;
    CHECK_THROWS(Placement::arrange(selected, {}, {}, {}, engine));
    CHECK(selected.front().translation == Vec2crd(0, 0));
    engine->invalid_identity = false; engine->cancel = true;
    CHECK(Placement::arrange(selected, {}, {}, {}, engine).canceled);
    CHECK(selected.front().translation == Vec2crd(0, 0));
    arrangement::ArrangeParams params;
    params.stopcondition = [] { return true; };
    const auto calls = engine->calls;
    CHECK(Placement::arrange(selected, {}, {}, params, engine).canceled);
    CHECK(engine->calls == calls);
}
