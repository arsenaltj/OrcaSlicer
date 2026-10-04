#include <catch2/catch_all.hpp>
#include "test_utils.hpp"

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/AABBTreeIndirect.hpp>

using namespace Slic3r;

TEST_CASE("Building a tree over a box, ray caster and closest query", "[AABBIndirect]")
{
    TriangleMesh tmesh = make_cube(1., 1., 1.);

    auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(tmesh.its.vertices, tmesh.its.indices);
    REQUIRE(! tree.empty());

    igl::Hit<float> hit;
	bool intersected = AABBTreeIndirect::intersect_ray_first_hit(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
		Vec3d(0.5, 0.5, -5.),
		Vec3d(0., 0., 1.),
		hit);

    REQUIRE(intersected);
    REQUIRE(hit.t == Catch::Approx(5.));

    std::vector<igl::Hit<float>> hits;
	bool intersected2 = AABBTreeIndirect::intersect_ray_all_hits(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, -5.),
		Vec3d(0., 0., 1.),
		hits);
    REQUIRE(intersected2);
    REQUIRE(hits.size() == 2);
    REQUIRE(hits.front().t == Catch::Approx(5.));
    REQUIRE(hits.back().t == Catch::Approx(6.));

    size_t hit_idx;
    Vec3d  closest_point;
    double squared_distance = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, -5.),
		hit_idx, closest_point);
    REQUIRE(squared_distance == Catch::Approx(5. * 5.));
    REQUIRE(closest_point.x() == Catch::Approx(0.3));
    REQUIRE(closest_point.y() == Catch::Approx(0.5));
    REQUIRE(closest_point.z() == Catch::Approx(0.));

    squared_distance = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, 5.),
		hit_idx, closest_point);
    REQUIRE(squared_distance == Catch::Approx(4. * 4.));
    REQUIRE(closest_point.x() == Catch::Approx(0.3));
    REQUIRE(closest_point.y() == Catch::Approx(0.5));
    REQUIRE(closest_point.z() == Catch::Approx(1.));
}

TEST_CASE("Parallel large-mesh tree preserves the sequential raycast index", "[AABBIndirect]")
{
    indexed_triangle_set mesh;
    constexpr int side = 260; // 135200 faces, above the parallel subtree threshold.
    mesh.vertices.reserve((side + 1) * (side + 1));
    mesh.indices.reserve(2 * side * side);
    for (int y = 0; y <= side; ++y)
        for (int x = 0; x <= side; ++x)
            mesh.vertices.emplace_back(float(x), float(y), 0.f);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const int lower_left = y * (side + 1) + x;
            const int upper_left = lower_left + side + 1;
            mesh.indices.emplace_back(lower_left, lower_left + 1, upper_left);
            mesh.indices.emplace_back(lower_left + 1, upper_left + 1, upper_left);
        }
    }

    const auto sequential = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.vertices, mesh.indices);
    const auto parallel = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.vertices, mesh.indices, 0.f, true);
    REQUIRE(parallel.nodes().size() == sequential.nodes().size());
    bool identical = true;
    for (size_t i = 0; i < sequential.nodes().size(); ++i) {
        const auto& old_node = sequential.node(i);
        const auto& new_node = parallel.node(i);
        if (old_node.idx != new_node.idx ||
            (old_node.is_valid() && (!old_node.bbox.min().isApprox(new_node.bbox.min()) ||
                                     !old_node.bbox.max().isApprox(new_node.bbox.max())))) {
            identical = false;
            break;
        }
    }
    CHECK(identical);

    igl::Hit<float> old_hit, new_hit;
    const Vec3d origin(13.2, 20.3, -5.);
    const Vec3d direction(0., 0., 1.);
    REQUIRE(AABBTreeIndirect::intersect_ray_first_hit(mesh.vertices, mesh.indices, sequential, origin, direction, old_hit));
    REQUIRE(AABBTreeIndirect::intersect_ray_first_hit(mesh.vertices, mesh.indices, parallel, origin, direction, new_hit));
    CHECK(old_hit.id == new_hit.id);
    CHECK(old_hit.t == new_hit.t);
}
