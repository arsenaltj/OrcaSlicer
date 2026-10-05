#include <iostream>
#include <fstream>
#include <random>
#include <catch2/catch_all.hpp>

#include "libslic3r/TriangleMesh.hpp"

#include "test_utils.hpp"

using namespace Slic3r;

namespace {
void require_generic_topology_stats(const TriangleMesh& mesh)
{
    // The generic adjacency implementation is unchanged and is the oracle for
    // both expanded and shared-index meshes; coincident positions do not weld.
    const auto neighbors = its_face_neighbors(mesh.its);
    CHECK(mesh.stats().number_of_parts == int(its_number_of_patches(mesh.its, neighbors)));
    CHECK(mesh.stats().open_edges == int(its_num_open_edges(neighbors)));
    CHECK(mesh.stats().number_of_facets == mesh.its.indices.size());
    CHECK_THAT(mesh.stats().volume, Catch::Matchers::WithinAbs(its_volume(mesh.its), 1e-6));
    if (!mesh.its.vertices.empty()) {
        const auto bounds = Slic3r::bounding_box(mesh.its);
        for (int axis = 0; axis < 3; ++axis) {
            CHECK_THAT(mesh.stats().min[axis], Catch::Matchers::WithinAbs(float(bounds.min[axis]), 1e-6));
            CHECK_THAT(mesh.stats().max[axis], Catch::Matchers::WithinAbs(float(bounds.max[axis]), 1e-6));
        }
    }
}
}

TEST_CASE("Expanded meshes preserve topology statistics for coincident degenerate and unused vertices", "[TriangleMeshStats][its]")
{
    indexed_triangle_set source;
    for (int face = 0; face < 320; ++face) {
        const int base = int(source.vertices.size());
        // Many geometrically coincident faces, plus isolated zero-area faces.
        source.vertices.insert(source.vertices.end(), {Vec3f(0,0,1), Vec3f(1,0,1),
            face % 3 == 0 ? Vec3f(1,0,1) : Vec3f(0,1,1)});
        source.indices.emplace_back(base, base + 1, base + 2);
    }
    source.vertices.emplace_back(-5, 8, -2);
    const TriangleMesh copied(source);
    require_generic_topology_stats(copied);
    CHECK(copied.stats().number_of_parts == 320);
    CHECK(copied.stats().open_edges == 960);
    CHECK_FALSE(copied.stats().manifold());
    RepairedMeshErrors repairs;
    repairs.edges_fixed = 7;
    repairs.facets_reversed = 3;
    indexed_triangle_set moved_source = source;
    const TriangleMesh moved(std::move(moved_source), repairs);
    require_generic_topology_stats(moved);
    CHECK(moved.stats().repaired_errors.edges_fixed == 7);
    CHECK(moved.stats().repaired_errors.facets_reversed == 3);
    const TriangleMesh vectors(source.vertices, source.indices);
    require_generic_topology_stats(vectors);
    require_generic_topology_stats(TriangleMesh(indexed_triangle_set {}));
}

TEST_CASE("Mesh initialization retains generic topology for shared reordered and nonmanifold indices", "[TriangleMeshStats][its]")
{
    std::vector<indexed_triangle_set> sources;
    indexed_triangle_set shared;
    shared.vertices = {{0,0,0},{1,0,0},{0,1,0},{1,1,0},{0,0,1},{5,5,5}};
    shared.indices = {{0,1,2},{1,0,3}};
    sources.push_back(shared);
    shared.indices.push_back({0,1,4});
    sources.push_back(shared);
    shared.indices = {{0,2,1},{3,4,5}};
    sources.push_back(shared);
    std::swap(shared.indices[0],shared.indices[1]);
    sources.push_back(shared);
    sources.push_back(its_make_cube(1,2,3));
    for (size_t i = 0; i < sources.size(); ++i) {
        DYNAMIC_SECTION("Topology case " << i) {
            const TriangleMesh mesh(sources[i]);
            require_generic_topology_stats(mesh);
        }
    }
}

TEST_CASE("Split empty mesh", "[its_split][its]") {
    using namespace Slic3r;

    indexed_triangle_set its;

    std::vector<indexed_triangle_set> res = its_split(its);

    REQUIRE(res.empty());
}

TEST_CASE("Split simple mesh consisting of one part", "[its_split][its]") {
    using namespace Slic3r;

    auto cube = its_make_cube(10., 10., 10.);

    std::vector<indexed_triangle_set> res = its_split(cube);

    REQUIRE(res.size() == 1);
    REQUIRE(res.front().indices.size() == cube.indices.size());
    REQUIRE(res.front().vertices.size() == cube.vertices.size());
}

// Dump each split part as its own OBJ for eyeballing; no-op in release.
void debug_write_obj(const std::vector<indexed_triangle_set> &res, const std::string &name)
{
#ifndef NDEBUG
    size_t part_idx = 0;
    for (const auto &part_its : res)
        write_debug_obj("indexed_triangle_set/" + name + std::to_string(part_idx++) + ".obj", part_its);
#else
    (void) res; (void) name;
#endif
}

TEST_CASE("Split two non-watertight mesh", "[its_split][its]") {
    using namespace Slic3r;

    auto cube1 = its_make_cube(10., 10., 10.);
    cube1.indices.pop_back();
    auto cube2 = cube1;

    its_transform(cube1, identity3f().translate(Vec3f{-5.f, 0.f, 0.f}));
    its_transform(cube2, identity3f().translate(Vec3f{5.f, 0.f, 0.f}));

    its_merge(cube1, cube2);

    std::vector<indexed_triangle_set> res = its_split(cube1);

    REQUIRE(res.size() == 2);
    REQUIRE(res[0].indices.size() == res[1].indices.size());
    REQUIRE(res[0].indices.size() == cube2.indices.size());
    REQUIRE(res[0].vertices.size() == res[1].vertices.size());
    REQUIRE(res[0].vertices.size() == cube2.vertices.size());

    debug_write_obj(res, "parts_non_watertight");
}

TEST_CASE("Split non-manifold mesh", "[its_split][its]") {
    using namespace Slic3r;

    auto cube = its_make_cube(10., 10., 10.), cube_low = cube;

    its_transform(cube_low, identity3f().translate(Vec3f{10.f, 10.f, 10.f}));
    its_merge(cube, cube_low);
    its_merge_vertices(cube);

    std::vector<indexed_triangle_set> res = its_split(cube);

    REQUIRE(res.size() == 2);
    REQUIRE(res[0].indices.size() == res[1].indices.size());
    REQUIRE(res[0].indices.size() == cube_low.indices.size());
    REQUIRE(res[0].vertices.size() == res[1].vertices.size());
    REQUIRE(res[0].vertices.size() == cube_low.vertices.size());

    debug_write_obj(res, "cubes_non_manifold");
}

TEST_CASE("Split two watertight meshes", "[its_split][its]") {
    using namespace Slic3r;

    auto sphere1 = its_make_sphere(10., 2 * PI / 200.), sphere2 = sphere1;

    its_transform(sphere1, identity3f().translate(Vec3f{-5.f, 0.f, 0.f}));
    its_transform(sphere2, identity3f().translate(Vec3f{5.f, 0.f, 0.f}));

    its_merge(sphere1, sphere2);

    std::vector<indexed_triangle_set> res = its_split(sphere1);

    REQUIRE(res.size() == 2);
    REQUIRE(res[0].indices.size() == res[1].indices.size());
    REQUIRE(res[0].indices.size() == sphere2.indices.size());
    REQUIRE(res[0].vertices.size() == res[1].vertices.size());
    REQUIRE(res[0].vertices.size() == sphere2.vertices.size());

    debug_write_obj(res, "parts_watertight");
}

#include <libslic3r/QuadricEdgeCollapse.hpp>
static float triangle_area(const Vec3f &v0, const Vec3f &v1, const Vec3f &v2)
{
    Vec3f ab = v1 - v0;
    Vec3f ac = v2 - v0;
    return ab.cross(ac).norm() / 2.f;
}

static float triangle_area(const stl_triangle_vertex_indices &triangle_indices, const std::vector<Vec3f> &vertices)
{
    return triangle_area(vertices[triangle_indices[0]],
                         vertices[triangle_indices[1]],
                         vertices[triangle_indices[2]]);
}

#if 0
// clang complains about unused functions
static std::mt19937 create_random_generator() {
    std::random_device rd;
    std::mt19937 gen(rd());
    return gen;
}
#endif

std::vector<Vec3f> its_sample_surface(const indexed_triangle_set &its,
                                      double        sample_per_mm2,
                                      std::mt19937 random_generator) // = create_random_generator())
{
    std::vector<Vec3f> samples;
    std::uniform_real_distribution<float> rand01(0.f, 1.f);
    for (const auto &triangle_indices : its.indices) {
        float area = triangle_area(triangle_indices, its.vertices);
        float countf;
        float fractional = std::modf(area * sample_per_mm2, &countf);
        int count = static_cast<int>(countf);

        float generate = rand01(random_generator);
        if (generate < fractional) ++count;
        if (count == 0) continue;

        const Vec3f &v0 = its.vertices[triangle_indices[0]];
        const Vec3f &v1 = its.vertices[triangle_indices[1]];
        const Vec3f &v2 = its.vertices[triangle_indices[2]];
        for (int c = 0; c < count; c++) {
            // barycentric coordinate
            Vec3f b;
            b[0] = rand01(random_generator);
            b[1] = rand01(random_generator);
            if ((b[0] + b[1]) > 1.f) {
                b[0] = 1.f - b[0];
                b[1] = 1.f - b[1];
            }
            b[2] = 1.f - b[0] - b[1];
            Vec3f pos;
            for (int i = 0; i < 3; i++) {
                pos[i] = b[0] * v0[i] + b[1] * v1[i] + b[2] * v2[i];
            }
            samples.push_back(pos);
        }        
    }
    return samples;
}


#include "libslic3r/AABBTreeIndirect.hpp"

struct CompareConfig
{
    float max_distance = 3.f;
    float max_average_distance = 2.f;
};

bool is_similar(const indexed_triangle_set &from,
             const indexed_triangle_set &to,
             const CompareConfig &cfg)
{
    // create ABBTree
    auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(
        from.vertices, from.indices);
    float sum_distance = 0.f;
    float max_distance = 0.f;

    auto  collect_distances = [&](const Vec3f &surface_point) {
        size_t hit_idx;
        Vec3f  hit_point;
        float  distance2 =
            AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
                from.vertices, from.indices, tree, surface_point, hit_idx, hit_point);
        float distance = sqrt(distance2);
        if (max_distance < distance) max_distance = distance;
        sum_distance += distance;
    };

    for (const Vec3f &vertex : to.vertices) { 
        collect_distances(vertex);
    }

    for (const Vec3i32 &t : to.indices) {
        Vec3f center(0,0,0);
        for (size_t i = 0; i < 3; ++i) { 
            center += to.vertices[t[i]] / 3;
        }
        collect_distances(center);
    }

    size_t count        = to.vertices.size() + to.indices.size();
    float avg_distance = sum_distance / count;
    if (avg_distance > cfg.max_average_distance || 
        max_distance > cfg.max_distance)
        return false;
    return true;
}

TEST_CASE("Reduce one edge by Quadric Edge Collapse", "[its]")
{
    indexed_triangle_set its;
    its.vertices = {Vec3f(-1.f, 0.f, 0.f), Vec3f(0.f, 1.f, 0.f),
                    Vec3f(1.f, 0.f, 0.f), Vec3f(0.f, 0.f, 1.f),
                    // vertex to be removed
                    Vec3f(0.9f, .1f, -.1f)};
    its.indices  = {Vec3i32(1, 0, 3), Vec3i32(2, 1, 3), Vec3i32(0, 2, 3),
                   Vec3i32(0, 1, 4), Vec3i32(1, 2, 4), Vec3i32(2, 0, 4)};
    // edge to remove is between vertices 2 and 4 on triangles 4 and 5

    indexed_triangle_set its_ = its; // copy
    // its_write_obj(its, "tetrhedron_in.obj");
    uint32_t wanted_count = its.indices.size() - 1;
    its_quadric_edge_collapse(its, wanted_count);
    // its_write_obj(its, "tetrhedron_out.obj");
    CHECK(its.indices.size() == 4);
    CHECK(its.vertices.size() == 4);

    for (size_t i = 0; i < 3; i++) { 
        CHECK(its.indices[i] == its_.indices[i]);
    }

    for (size_t i = 0; i < 4; i++) {
        if (i == 2) continue;
        CHECK(its.vertices[i] == its_.vertices[i]);
    }

    const Vec3f &v = its.vertices[2]; // new vertex
    const Vec3f &v2 = its_.vertices[2]; // moved vertex
    const Vec3f &v4 = its_.vertices[4]; // removed vertex
    for (size_t i = 0; i < 3; i++) { 
        bool is_between = (v[i] < v4[i] && v[i] > v2[i]) ||
                          (v[i] > v4[i] && v[i] < v2[i]);
        CHECK(is_between);
    }
    CompareConfig cfg;
    cfg.max_average_distance = 0.014f;
    cfg.max_distance         = 0.75f;

    CHECK(is_similar(its, its_, cfg));
    CHECK(is_similar(its_, its, cfg));
}

TEST_CASE("Simplify mesh by Quadric edge collapse to 5%", "[its]")
{
    TriangleMesh mesh = load_model("frog_legs.obj");
    double original_volume = its_volume(mesh.its);
    uint32_t wanted_count = mesh.its.indices.size() * 0.05;
    REQUIRE_FALSE(mesh.empty());
    indexed_triangle_set its = mesh.its; // copy
    float max_error = std::numeric_limits<float>::max();
    its_quadric_edge_collapse(its, wanted_count, &max_error);
    //its_write_obj(its, "frog_legs_qec.obj");
    CHECK(its.indices.size() <= wanted_count);
    double volume = its_volume(its);
    CHECK(fabs(original_volume - volume) < 33.);

    CompareConfig cfg;
    cfg.max_average_distance = 0.043f;
    cfg.max_distance         = 0.32f;

    CHECK(is_similar(mesh.its, its, cfg));
    CHECK(is_similar(its, mesh.its, cfg));
}

bool exist_triangle_with_twice_vertices(const std::vector<stl_triangle_vertex_indices>& indices)
{
    for (const auto &face : indices)
        if (face[0] == face[1] || 
            face[0] == face[2] || 
            face[1] == face[2]) return true;
    return false;
}

TEST_CASE("Simplify trouble case", "[its]")
{
    TriangleMesh tm = load_model("simplification.obj");
    REQUIRE_FALSE(tm.empty());
    float max_error = std::numeric_limits<float>::max();
    uint32_t wanted_count = 0;
    its_quadric_edge_collapse(tm.its, wanted_count, &max_error);
    CHECK(!exist_triangle_with_twice_vertices(tm.its.indices));
}

TEST_CASE("Simplified cube should not be empty.", "[its]")
{
    auto its = its_make_cube(1, 2, 3);
    float    max_error    = std::numeric_limits<float>::max();
    uint32_t wanted_count = 0;
    its_quadric_edge_collapse(its, wanted_count, &max_error);
    CHECK(!its.indices.empty());
}
