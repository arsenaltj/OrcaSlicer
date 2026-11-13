
#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/Point.hpp>
#include <cstddef>
#include <vector>

using namespace Slic3r;

TEST_CASE("CGAL and TriangleMesh conversions", "[MeshBoolean]") {
    TriangleMesh sphere = make_sphere(1.);
    
    auto cgalmesh_ptr = MeshBoolean::cgal::triangle_mesh_to_cgal(sphere);
    
    REQUIRE(cgalmesh_ptr);
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(*cgalmesh_ptr));
    
    TriangleMesh M = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgalmesh_ptr);
    
    REQUIRE(M.its.vertices.size() == sphere.its.vertices.size());
    REQUIRE(M.its.indices.size() == sphere.its.indices.size());
    
    REQUIRE(M.volume() == Catch::Approx(sphere.volume()));
    
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(M));
}

TEST_CASE("CGAL volume checks ignore unreferenced vertices", "[MeshBoolean][Regression]") {
    indexed_triangle_set mesh = its_make_cube(10., 10., 10.);
    // Leave isolated vertices before, between, and after the vertices used by faces.
    mesh.vertices.insert(mesh.vertices.begin(), Vec3f(-1.f, -1.f, -1.f));
    mesh.vertices.insert(mesh.vertices.begin() + 4, Vec3f(5.f, 5.f, 5.f));
    mesh.vertices.emplace_back(20.f, 20.f, 20.f);
    for (Vec3i32 &face : mesh.indices)
        for (int i = 0; i < 3; ++i)
            face[i] += face[i] >= 3 ? 2 : 1;

    auto cgal_mesh = MeshBoolean::cgal::triangle_mesh_to_cgal(mesh);
    REQUIRE(cgal_mesh);
    REQUIRE(MeshBoolean::cgal::does_bound_a_volume(*cgal_mesh));
    REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(*cgal_mesh));

    TriangleMesh result = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgal_mesh);
    REQUIRE(result.its.indices.size() == mesh.indices.size());
    for (const Vec3i32 &face : result.its.indices)
        for (int i = 0; i < 3; ++i) {
            REQUIRE(face[i] >= 0);
            REQUIRE(static_cast<std::size_t>(face[i]) < result.its.vertices.size());
        }
    REQUIRE_THAT(result.volume(), Catch::Matchers::WithinRel(1000., 1e-6));
}

TEST_CASE("CGAL volume checks reject open meshes with unreferenced vertices", "[MeshBoolean][Regression]") {
    indexed_triangle_set mesh = its_make_cube(10., 10., 10.);
    mesh.indices.pop_back();
    mesh.vertices.emplace_back(20.f, 20.f, 20.f);

    auto cgal_mesh = MeshBoolean::cgal::triangle_mesh_to_cgal(mesh);
    REQUIRE(cgal_mesh);
    REQUIRE_FALSE(MeshBoolean::cgal::does_bound_a_volume(*cgal_mesh));
}

TEST_CASE("mcut difference handles source splits between cuts", "[MeshBoolean]") {
    TriangleMesh body = make_cube(30., 10., 10.);

    TriangleMesh tool;

    // First cut splits the source into two disconnected components.
    TriangleMesh slab = make_cube(2., 12., 20.);
    slab.translate(Vec3f(14.f, -1.f, -5.f));
    its_merge(tool.its, slab.its);

    // These cuts must still be applied after the source has been split.
    TriangleMesh left_hole = make_cube(4., 4., 20.);
    left_hole.translate(Vec3f(3.f, 3.f, -5.f));
    its_merge(tool.its, left_hole.its);

    TriangleMesh right_hole = make_cube(4., 4., 20.);
    right_hole.translate(Vec3f(21.f, 3.f, -5.f));
    its_merge(tool.its, right_hole.its);

    std::vector<TriangleMesh> result;
    MeshBoolean::mcut::make_boolean(body, tool, result, "A_NOT_B");

    REQUIRE(result.size() == 1);

    const std::vector<indexed_triangle_set> components =
        its_split(result.front().its);

    REQUIRE(components.size() == 2);

    // 3000 - 200 - 160 - 160 = 2480.
    REQUIRE_THAT(
        result.front().volume(),
        Catch::Matchers::WithinRel(2480., 1e-3)
    );
}


TEST_CASE("CGAL conversion compacts unreferenced vertices without changing the source", "[MeshBoolean][WorkbenchTopology][Regression]") {
    TriangleMesh mesh = make_cube(10., 10., 10.);
    mesh.its.vertices.emplace_back(100.f, 100.f, 100.f);
    const auto source = mesh.its;

    auto cgal_mesh = MeshBoolean::cgal::triangle_mesh_to_cgal(mesh);

    REQUIRE(cgal_mesh);
    // Official b3f8d0af15 compacts the conversion copy before CGAL's volume
    // predicate, which dereferences an incident halfedge for every vertex.
    CHECK(MeshBoolean::cgal::does_bound_a_volume(*cgal_mesh));
    TriangleMesh converted = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgal_mesh);
    CHECK(converted.its.vertices.size() + 1 == source.vertices.size());
    CHECK(converted.its.indices.size() == source.indices.size());
    CHECK_THAT(converted.volume(), Catch::Matchers::WithinRel(1000., 1e-6));
    CHECK(mesh.its.vertices == source.vertices);
    CHECK(mesh.its.indices == source.indices);
}


TEST_CASE("Canceled CGAL repair leaves source geometry and diagnostics untouched", "[MeshBoolean][MeshRepairCancel][Regression]") {
    // First check cancels before conversion; fifth check cancels after soup cleanup.
    const int cancel_at = GENERATE(1, 5);
    TriangleMesh mesh = make_cube(10., 10., 10.);
    const auto original = mesh.its;
    RepairedMeshErrors diagnostics;
    diagnostics.edges_fixed = 7;
    diagnostics.facets_removed = 9;
    int checks = 0;
    std::string error;

    REQUIRE_FALSE(MeshBoolean::cgal::repair(mesh, &diagnostics, &error, [&] { return ++checks >= cancel_at; }));
    REQUIRE(checks == cancel_at);
    REQUIRE(error == "Repair canceled");
    REQUIRE(mesh.its.vertices == original.vertices);
    REQUIRE(mesh.its.indices == original.indices);
    REQUIRE(diagnostics.edges_fixed == 7);
    REQUIRE(diagnostics.facets_removed == 9);
}


TEST_CASE("CGAL repair still succeeds without a cancellation request", "[MeshBoolean][MeshRepairCancel]") {
    TriangleMesh mesh = make_cube(10., 10., 10.);
    std::string error;

    REQUIRE(MeshBoolean::cgal::repair(mesh, nullptr, &error));
    REQUIRE(its_num_open_edges(mesh.its) == 0);
}


TEST_CASE("CGAL repair fills an actual hole and exports a closed solid without intersections", "[MeshBoolean][WorkbenchTopology]") {
    TriangleMesh mesh = make_cube(10., 10., 10.);
    mesh.its.indices.pop_back();
    std::string error;
    REQUIRE(MeshBoolean::cgal::repair(mesh, nullptr, &error));
    CHECK(its_num_open_edges(mesh.its) == 0);
    CHECK_FALSE(MeshBoolean::cgal::does_self_intersect(mesh));
    CHECK(mesh.volume() > 0.f);
}


TEST_CASE("failed CGAL solid repair leaves a planar source and diagnostics untouched", "[MeshBoolean][WorkbenchTopology]") {
    indexed_triangle_set its;
    its.vertices = {{0,0,0}, {10,0,0}, {0,10,0}};
    its.indices = {{0,1,2}};
    TriangleMesh mesh(its);
    RepairedMeshErrors diagnostics; diagnostics.edges_fixed = 7;
    std::string error;
    CHECK_FALSE(MeshBoolean::cgal::repair(mesh, &diagnostics, &error));
    CHECK(mesh.its.vertices == its.vertices);
    CHECK(mesh.its.indices == its.indices);
    CHECK(diagnostics.edges_fixed == 7);
    CHECK_FALSE(error.empty());
}
