#include <catch2/catch_all.hpp>

#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Surface.hpp"

using namespace Slic3r;

namespace {
Polylines support_paths(double factor)
{
    Polygon boundary;
    for (const Vec2d &p : std::vector<Vec2d>{{0,0},{24,0},{24,18},{21,20},{18,14},
            {15,21},{12,15},{9,22},{6,14},{3,20},{0,18}})
        boundary.points.emplace_back(scale_(p.x() * factor), scale_(p.y() * factor));
    auto filler = std::unique_ptr<Fill>(Fill::new_from_type(ipSupportBase));
    filler->set_bounding_box(boundary.bounding_box());
    filler->spacing = .4 * factor;
    filler->angle = 0.;
    FillParams params;
    params.density = .2f;
    params.dont_adjust = true;
    Surface surface(stInternal, ExPolygon(boundary));
    return filler->fill_surface(&surface, params);
}
}

TEST_CASE("Support connections use the current line spacing", "[FillBase][Regression]")
{
    const Polylines reference = support_paths(1.);
    REQUIRE_FALSE(reference.empty());
    for (double factor : {.5, 2., 4.}) {
        CAPTURE(factor);
        const Polylines scaled = support_paths(factor);
        REQUIRE(scaled.size() == reference.size());
        for (size_t i = 0; i < reference.size(); ++i) {
            REQUIRE(scaled[i].points.size() == reference[i].points.size());
            for (size_t j = 0; j < reference[i].points.size(); ++j)
                REQUIRE_THAT((scaled[i].points[j].cast<double>() / factor -
                              reference[i].points[j].cast<double>()).norm(),
                             Catch::Matchers::WithinAbs(0., scale_(.001)));
        }
    }
}
