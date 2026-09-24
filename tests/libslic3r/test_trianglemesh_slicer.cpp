#include <catch2/catch_all.hpp>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include <algorithm>
#include <cmath>
#include <random>

using namespace Slic3r;

TEST_CASE("Mesh contour ordering is independent of facet processing order", "[TriangleMeshSlicer][Regression]")
{
    TriangleMesh mesh = make_cube(20., 20., 20.);
    TriangleMesh other = make_cube(10., 12., 20.);
    other.translate(Vec3f(40.123f, -0.217f, 0.f));
    mesh.merge(other);
    mesh.rotate_z(0.137f);
    mesh.translate(Vec3f(0.12345f, 0.67891f, 0.f));
    const std::vector<float> heights{0.3f, 3.5f, 9.7f, 17.4f};
    const MeshSlicingParams params;
    const auto expected = slice_mesh(mesh.its, heights, params);
    REQUIRE(expected.size() == heights.size());
    for (const auto &layer : expected) {
        REQUIRE(layer.size() == 2);
        double area_mm2 = 0.;
        for (const auto &polygon : layer)
            area_mm2 += polygon.area() * SCALING_FACTOR * SCALING_FACTOR;
        REQUIRE_THAT(area_mm2, Catch::Matchers::WithinAbs(20. * 20. + 10. * 12., .002));
    }
    std::mt19937 random(7154);
    for (int trial = 0; trial < 16; ++trial) {
        CAPTURE(trial);
        std::shuffle(mesh.its.indices.begin(), mesh.its.indices.end(), random);
        const auto actual = slice_mesh(mesh.its, heights, params);
        REQUIRE(actual.size() == expected.size());
        for (size_t layer = 0; layer < heights.size(); ++layer) {
            CAPTURE(layer);
            const auto single = slice_mesh(mesh.its, heights[layer], params);
            REQUIRE(actual[layer].size() == expected[layer].size());
            REQUIRE(single.size() == expected[layer].size());
            for (size_t polygon = 0; polygon < expected[layer].size(); ++polygon) {
                // Stable polygon order and cyclic starts are required: later
                // simplification and greedy coloring consume the point sequence.
                REQUIRE(actual[layer][polygon].points == expected[layer][polygon].points);
                REQUIRE(single[polygon].points == expected[layer][polygon].points);
            }
        }
    }
}

TEST_CASE("Slab projection ordering is independent of facet processing order", "[TriangleMeshSlicer][Regression]")
{
    TriangleMesh mesh = make_cube(20., 20., 20.);
    TriangleMesh other = make_cube(10., 12., 20.);
    other.translate(Vec3f(40.123f, -0.217f, 0.f));
    mesh.merge(other);
    mesh.rotate_y(0.23f);
    mesh.rotate_z(0.137f);
    mesh.translate(Vec3f(0.12345f, 0.67891f, 15.f));
    const std::vector<float> heights{-1.f, 5.f, 10.f, 15.f, 20.f, 25.f, 30.f, 40.f};
    const Transform3d transform = Transform3d::Identity();
    std::vector<Polygons> expected_top, expected_bottom;
    slice_mesh_slabs(mesh.its, heights, transform, &expected_top, &expected_bottom, nullptr, []{});
    REQUIRE(expected_top.size() == heights.size());
    REQUIRE(expected_bottom.size() == heights.size());
    for (const auto *projection : {&expected_top, &expected_bottom}) {
        double area_mm2 = 0.;
        for (const auto &layer : *projection)
            for (const auto &polygon : layer)
                area_mm2 += polygon.area() * SCALING_FACTOR * SCALING_FACTOR;
        // Both rotated cuboids are convex, disjoint in XY and contained between
        // the first/last planes. Each side covers its full projected footprint.
        const double footprint = (20. * std::cos(0.23) + 20. * std::sin(0.23)) * 20. +
                                 (10. * std::cos(0.23) + 20. * std::sin(0.23)) * 12.;
        REQUIRE_THAT(area_mm2, Catch::Matchers::WithinAbs(footprint, .003));
    }
    std::mt19937 random(7160);
    for (int trial = 0; trial < 16; ++trial) {
        CAPTURE(trial);
        std::shuffle(mesh.its.indices.begin(), mesh.its.indices.end(), random);
        std::vector<Polygons> top, bottom;
        slice_mesh_slabs(mesh.its, heights, transform, &top, &bottom, nullptr, []{});
        for (size_t side = 0; side < 2; ++side) {
            CAPTURE(side);
            const auto &expected = side == 0 ? expected_top : expected_bottom;
            const auto &actual = side == 0 ? top : bottom;
            REQUIRE(actual.size() == expected.size());
            for (size_t layer = 0; layer < heights.size(); ++layer) {
                CAPTURE(layer);
                REQUIRE(actual[layer].size() == expected[layer].size());
                for (size_t polygon = 0; polygon < expected[layer].size(); ++polygon)
                    // Boolean clipping and simplification consume this order.
                    REQUIRE(actual[layer][polygon].points == expected[layer][polygon].points);
            }
        }
    }
}
