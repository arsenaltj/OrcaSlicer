#include <catch2/catch_all.hpp>
#include "test_utils.hpp"

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/MeshBoolean.hpp>

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
