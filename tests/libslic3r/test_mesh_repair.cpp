#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>
#include <array>
#include <cstring>
#include <random>
#include <type_traits>
#include "diagnostic_log_capture.hpp"
#include <tbb/task_arena.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Polygon_mesh_processing/repair_self_intersections.h>
#include <CGAL/Polygon_mesh_processing/repair_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>

#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/MeshRepair.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/PolygonSoupPreparation.hpp>
#include <libslic3r/Point.hpp>
#include <libslic3r/libslic3r.h>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleMeshSlicer.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/Utils.hpp>

using namespace Slic3r;
using Catch::Matchers::WithinRel;

namespace {

indexed_triangle_set cube(double size, const Vec3d &offset = Vec3d::Zero())
{
    TriangleMesh mesh = make_cube(size, size, size);
    mesh.translate(offset.cast<float>());
    return mesh.its;
}

indexed_triangle_set merged(const std::vector<indexed_triangle_set> &parts)
{
    indexed_triangle_set out;
    for (const indexed_triangle_set &part : parts)
        its_merge(out, part);
    return out;
}

// Every triangle with its own three vertices, as some exporters write meshes.
indexed_triangle_set soup(const indexed_triangle_set &its)
{
    indexed_triangle_set out;
    for (const stl_triangle_vertex_indices &f : its.indices) {
        const int first = int(out.vertices.size());
        for (int k = 0; k < 3; ++k)
            out.vertices.push_back(its.vertices[f[k]]);
        out.indices.emplace_back(first, first + 1, first + 2);
    }
    return out;
}

double regular_polygon_area(int sides, double radius) { return 0.5 * sides * radius * radius * std::sin(2. * PI / sides); }

indexed_triangle_set open_pipe(int sides, double outer, double inner, double height)
{
    indexed_triangle_set mesh;
    for (const double radius : {outer, inner})
        for (int k = 0; k < sides; ++k) {
            const double angle = 2. * PI * k / sides;
            mesh.vertices.emplace_back(float(radius * std::cos(angle)), float(radius * std::sin(angle)), 0.f);
            mesh.vertices.emplace_back(float(radius * std::cos(angle)), float(radius * std::sin(angle)), float(height));
        }
    for (int k = 0; k < sides; ++k) {
        const int a0 = 2 * k, b0 = a0 + 1, a1 = 2 * ((k + 1) % sides), b1 = a1 + 1;
        mesh.indices.emplace_back(a0, a1, b1);
        mesh.indices.emplace_back(a0, b1, b0);
        mesh.indices.emplace_back(2 * sides + a0, 2 * sides + b1, 2 * sides + a1);
        mesh.indices.emplace_back(2 * sides + a0, 2 * sides + b0, 2 * sides + b1);
    }
    return mesh;
}

// Closed by vertex indices, outward oriented and free of self-intersections.
void check_solid(const TriangleMesh &mesh, double volume)
{
    CHECK(its_num_open_edges(mesh.its) == 0);
    CHECK_THAT(its_volume(mesh.its), WithinRel(volume, 1e-4));
    CHECK_FALSE(MeshBoolean::cgal::does_self_intersect(mesh));
}

} // namespace

TEST_CASE("Compatible normalized polygon soups retain their exact orientation", "[MeshRepair][SoupOrientationEligibility]")
{
    using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
    using Point = Kernel::Point_3;
    using Polygon = std::array<std::size_t, 3>;
    namespace PMP = CGAL::Polygon_mesh_processing;
    const auto same_bits = [](double left, double right) {
        return std::memcmp(&left, &right, sizeof(double)) == 0;
    };
    std::size_t compatible_cases = 0, incompatible_cases = 0, empty_cases = 0;
    const auto check = [&](std::vector<Point> points, std::vector<Polygon> polygons, int expected = -1) {
        PMP::repair_polygon_soup(points, polygons);
        const bool compatible = PMP::is_polygon_soup_a_polygon_mesh(polygons);
        if (expected >= 0) REQUIRE(compatible == bool(expected));
        const auto original_points = points;
        const auto original_polygons = polygons;
        auto actual_points = points;
        auto actual_polygons = polygons;
        const auto actual = MeshBoolean::detail::orient_prepared_polygon_soup(actual_points, actual_polygons);
        const bool no_vertex_split = PMP::orient_polygon_soup(points, polygons);
        REQUIRE(actual.valid == (!polygons.empty() && PMP::is_polygon_soup_a_polygon_mesh(polygons)));
        REQUIRE(actual_polygons == polygons);
        REQUIRE(actual_points.size() == points.size());
        for (std::size_t i = 0; i < points.size(); ++i) {
            CHECK(same_bits(actual_points[i].x(), points[i].x()));
            CHECK(same_bits(actual_points[i].y(), points[i].y()));
            CHECK(same_bits(actual_points[i].z(), points[i].z()));
        }
#if CGAL_VERSION_NR == 1060211000
        CHECK(actual.reused_existing == (compatible && !original_polygons.empty()));
#else
        CHECK_FALSE(actual.reused_existing);
#endif
        if (compatible) {
            if (original_polygons.empty()) ++empty_cases;
            else ++compatible_cases;
            REQUIRE(no_vertex_split);
            REQUIRE(points.size() == original_points.size());
            REQUIRE(polygons == original_polygons);
            for (std::size_t i = 0; i < points.size(); ++i) {
                CHECK(same_bits(points[i].x(), original_points[i].x()));
                CHECK(same_bits(points[i].y(), original_points[i].y()));
                CHECK(same_bits(points[i].z(), original_points[i].z()));
            }
            CHECK(PMP::is_polygon_soup_a_polygon_mesh(polygons));
        } else {
            ++incompatible_cases;
        }
    };
    std::mt19937 random(610219);
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        auto source = scenario == 0 ? cube(20.) : make_sphere(10., PI / 12.).its;
        if (scenario == 2) source.indices.pop_back();
        if (scenario == 3) source = merged({source, cube(3., {40., 7., 3.})});
        if (scenario == 4) {
            auto other = source;
            for (auto& vertex : other.vertices) vertex += Vec3f(40.f, 7.f, 3.f);
            other.vertices[0] = source.vertices[0];
            // Two otherwise separate shells have only one vertex in common.
            const int offset = int(source.vertices.size()) - 1;
            source.vertices.insert(source.vertices.end(), other.vertices.begin() + 1, other.vertices.end());
            for (auto face : other.indices) {
                for (int corner = 0; corner < 3; ++corner)
                    if (face[corner] != 0) face[corner] += offset;
                source.indices.push_back(face);
            }
        }
        for (unsigned order = 0; order < 4; ++order) {
            if (order) std::shuffle(source.indices.begin(), source.indices.end(), random);
            if (order == 2) its_flip_triangles(source);
            std::vector<Point> points;
            for (const auto& vertex : source.vertices)
                points.emplace_back(vertex.x() == 0.f ? -0. : double(vertex.x()), vertex.y(), vertex.z());
            std::vector<Polygon> polygons;
            for (const auto& face : source.indices)
                polygons.push_back({std::size_t(face[0]), std::size_t(face[1]), std::size_t(face[2])});
            check(std::move(points), std::move(polygons), scenario == 4 ? 0 : 1);
        }
    }
    // Independent soups exercise same-direction edges, repeated/reversed
    // faces, disconnected fans, geometric coincidences and open boundaries.
    for (unsigned trial = 0; trial < 3000; ++trial) {
        std::vector<Point> points = {{-0.,0.,0.}, {1.,0.,0.}, {0.,1.,0.}, {0.,0.,1.},
            {1.,1.,1.}, {1.,-1.,0.}, {0.,1.,0.}};
        std::vector<Polygon> polygons;
        const unsigned faces = 1 + random() % 10;
        for (unsigned i = 0; i < faces; ++i)
            polygons.push_back({random() % points.size(), random() % points.size(), random() % points.size()});
        check(std::move(points), std::move(polygons));
    }
    CHECK(compatible_cases > 100);
    CHECK(incompatible_cases > 100);
    CHECK(empty_cases > 0);
}

TEST_CASE("Polygon soup orientation qualification failure retains the original fallback", "[MeshRepair][SoupOrientationEligibility]")
{
    using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
    using Point = Kernel::Point_3;
    namespace PMP = CGAL::Polygon_mesh_processing;
    const auto input = cube(20.);
    std::vector<Point> points;
    std::vector<std::array<std::size_t, 3>> polygons;
    for (const auto& vertex : input.vertices) points.emplace_back(vertex.x(), vertex.y(), vertex.z());
    for (const auto& face : input.indices)
        polygons.push_back({std::size_t(face[0]), std::size_t(face[1]), std::size_t(face[2])});
    PMP::repair_polygon_soup(points, polygons);
    const auto original_points = points;
    const auto original_polygons = polygons;
    const auto result = MeshBoolean::detail::orient_prepared_polygon_soup(points, polygons,
        [](const auto&) -> bool { throw std::bad_alloc(); });
    CHECK(result.valid);
    CHECK_FALSE(result.reused_existing);
    CHECK(points == original_points);
    CHECK(polygons == original_polygons);
}

TEST_CASE("Self intersection queries retain exact decisions across bounded arenas", "[MeshRepair][StrictMeshRepair]")
{
    const int threads = GENERATE(1, 2, 4);
    const bool overlaps = GENERATE(false, true);
    tbb::task_arena arena(threads);
    for (int sample = 0; sample < 12; ++sample) {
        const auto offset = overlaps ? 0.5 + sample : 21.5 + sample;
        const auto input = merged({cube(20.), cube(20., Vec3d(offset, offset * 0.7, offset * 0.3))});
        const auto original = input;
        const TriangleMesh mesh(input);
        const auto converted = MeshBoolean::cgal::triangle_mesh_to_cgal(mesh);
        REQUIRE(converted);
        const auto result = arena.execute([&] { return MeshBoolean::cgal::does_self_intersect(mesh); });
        const auto converted_result = arena.execute([&] { return MeshBoolean::cgal::does_self_intersect(*converted); });
        CHECK(result == overlaps);
        CHECK(converted_result == overlaps);
        CHECK(mesh.its.vertices == original.vertices);
        CHECK(mesh.its.indices == original.indices);
    }
}

TEST_CASE("Intersection discovery preserves face pairs across query concurrency", "[MeshRepair][StrictMeshRepair][IntersectionQueryPairs]")
{
    using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
    using Mesh = CGAL::Surface_mesh<Kernel::Point_3>;
    using Point = Kernel::Point_3;
    std::mt19937 random(921771);
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        std::vector<std::array<Point,3>> triangles;
        // Exceed CGAL's generic broad-phase cutoff, with bounded local overlap.
        for (unsigned group = 0; group < 720; ++group) {
            const double x = double(group % 36) * 10., y = double(group / 36) * 10.;
            triangles.push_back({Point(x,y,0),Point(x+2,y,0),Point(x,y+2,0)});
            if (scenario == 0) // Separate triangles.
                triangles.push_back({Point(x+3,y,1),Point(x+5,y,1),Point(x+3,y+2,1)});
            else if (scenario == 1) // Proper crossings.
                triangles.push_back({Point(x+0.5,y+0.5,-1),Point(x+0.5,y+0.5,1),Point(x+1.5,y+0.5,1)});
            else if (scenario == 2) // Coplanar overlap and duplicate coordinates.
                triangles.push_back(triangles.back());
            else if (scenario == 3) // Point contact.
                triangles.push_back({Point(x,y,0),Point(x,y,-2),Point(x-2,y,0)});
            else if (scenario == 4) // Geometric degeneracy, with valid graph indices.
                triangles.push_back({Point(x,y,0),Point(x+1,y,0),Point(x+2,y,0)});
            else // Varied intersections independent of any real model.
                triangles.push_back({Point(x+double(random()%3),y+double(random()%3),-1),
                    Point(x+double(random()%3),y+double(random()%3),1),Point(x+1,y+1,0)});
            triangles.push_back({Point(x+6,y+6,2),Point(x+8,y+6,2),Point(x+6,y+8,2)});
        }
        for (unsigned order = 0; order < 2; ++order) {
            if (order) std::shuffle(triangles.begin(), triangles.end(), random);
            Mesh mesh;
            for (const auto& triangle : triangles) {
                const auto a = mesh.add_vertex(triangle[0]), b = mesh.add_vertex(triangle[1]), c = mesh.add_vertex(triangle[2]);
                REQUIRE(mesh.add_face(a,b,c) != Mesh::null_face());
            }
            const auto enumerate = [&](const auto& tag) {
                std::vector<std::pair<Mesh::Face_index,Mesh::Face_index>> raw;
                CGAL::Polygon_mesh_processing::self_intersections<std::decay_t<decltype(tag)>>(
                    mesh, std::back_inserter(raw));
                std::vector<std::pair<std::size_t,std::size_t>> result;
                for (const auto& pair : raw) {
                    const auto a = std::size_t(pair.first.idx()), b = std::size_t(pair.second.idx());
                    result.emplace_back(std::min(a,b), std::max(a,b));
                }
                std::sort(result.begin(), result.end());
                return result;
            };
            const auto expected = enumerate(CGAL::Sequential_tag{});
            for (const int threads : {1,2,4}) {
                CAPTURE(scenario, order, threads);
                tbb::task_arena arena(threads);
                const auto actual = arena.execute([&] { return enumerate(CGAL::Parallel_if_available_tag{}); });
                CHECK(actual == expected);
                const bool any = arena.execute([&] {
                    return CGAL::Polygon_mesh_processing::does_self_intersect<CGAL::Parallel_if_available_tag>(mesh);
                });
                CHECK(any == !expected.empty());
            }
        }
    }
}

TEST_CASE("Whole mesh intersection repair returns the exact final query decision", "[MeshRepair][StrictMeshRepair][IntersectionRepairResult]")
{
    using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
    using Mesh = CGAL::Surface_mesh<Kernel::Point_3>;
    namespace PMP = CGAL::Polygon_mesh_processing;
    const int threads = GENERATE(1, 2, 4);
    const bool reverse_faces = GENERATE(false, true);
    tbb::task_arena arena(threads);
    const auto convert = [](const indexed_triangle_set& source) {
        Mesh mesh;
        for (const auto& vertex : source.vertices)
            mesh.add_vertex({vertex.x(), vertex.y(), vertex.z()});
        for (const auto& face : source.indices) {
            const auto inserted = mesh.add_face(Mesh::Vertex_index(face[0]),
                Mesh::Vertex_index(face[1]), Mesh::Vertex_index(face[2]));
            REQUIRE(inserted != Mesh::null_face());
        }
        return mesh;
    };
    auto folded = make_sphere(10., PI / 12.).its;
    const auto top = std::max_element(folded.vertices.begin(), folded.vertices.end(),
        [](const auto& a, const auto& b) { return a.z() < b.z(); });
    top->z() = -12.f;
    auto open = cube(20.);
    open.indices.pop_back();
    std::vector<indexed_triangle_set> cases = {
        cube(20.),
        merged({cube(20.), cube(20., {40., 7., 3.})}),
        merged({cube(20.), cube(20., {5., 7., 3.})}),
        merged({cube(20.), cube(20., {20., 20., 20.})}),
        folded,
        merged({folded, cube(20., {40., 7., 3.})}),
        open,
    };
    for (size_t scenario = 0; scenario < cases.size(); ++scenario) {
        DYNAMIC_SECTION("scenario " << scenario << " threads " << threads << " reverse " << reverse_faces) {
            auto input = cases[scenario];
            if (reverse_faces) std::reverse(input.indices.begin(), input.indices.end());
            auto mesh = convert(input);
            auto serial = mesh;
            const auto parameters = CGAL::parameters::number_of_iterations(7)
                .apply_per_connected_component(false).use_smoothing(true).preserve_genus(false);
            const bool returned = arena.execute([&] {
                return PMP::experimental::remove_self_intersections(mesh,
                    parameters.concurrency_tag(CGAL::Parallel_if_available_tag{}));
            });
            const bool serial_returned = arena.execute([&] {
                return PMP::experimental::remove_self_intersections(serial,
                    parameters.concurrency_tag(CGAL::Sequential_tag{}));
            });
            // Check both outcomes against the entire resulting mesh. This
            // includes deleted/replaced faces, not only the original range.
            CHECK(returned == !PMP::does_self_intersect<CGAL::Sequential_tag>(mesh));
            CHECK(serial_returned == !PMP::does_self_intersect<CGAL::Sequential_tag>(serial));
            CHECK(returned == serial_returned);
            CHECK(num_faces(mesh) == num_faces(serial));
            if (scenario < 2) CHECK(returned);
            if (scenario == 2) CHECK_FALSE(returned);
            if (scenario == 6) {
                CHECK(returned);
                CHECK_FALSE(CGAL::is_closed(mesh));
            }
        }
    }
}

TEST_CASE("A partial repair result cannot certify unvisited mesh components", "[MeshRepair][StrictMeshRepair][IntersectionRepairResult]")
{
    using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
    using Mesh = CGAL::Surface_mesh<Kernel::Point_3>;
    namespace PMP = CGAL::Polygon_mesh_processing;
    Mesh mesh;
    std::vector<Mesh::Face_index> clean_component;
    const size_t clean_faces = cube(20.).indices.size();
    const auto input = merged({cube(20.), cube(20., {50., 0., 0.}), cube(20., {55., 7., 3.})});
    for (const auto& vertex : input.vertices)
        mesh.add_vertex({vertex.x(), vertex.y(), vertex.z()});
    for (size_t i = 0; i < input.indices.size(); ++i) {
        const auto& face = input.indices[i];
        const auto inserted = mesh.add_face(Mesh::Vertex_index(face[0]),
            Mesh::Vertex_index(face[1]), Mesh::Vertex_index(face[2]));
        REQUIRE(inserted != Mesh::null_face());
        if (i < clean_faces) clean_component.push_back(inserted);
    }
    REQUIRE(PMP::does_self_intersect<CGAL::Sequential_tag>(mesh));
    const bool returned = PMP::experimental::remove_self_intersections(clean_component, mesh,
        CGAL::parameters::number_of_iterations(7).apply_per_connected_component(false)
            .use_smoothing(true).preserve_genus(false));
    CHECK(returned);
    CHECK(PMP::does_self_intersect<CGAL::Sequential_tag>(mesh));
}

TEST_CASE("A triangle soup repairs into the solid it describes", "[MeshRepair]")
{
    TriangleMesh mesh(soup(cube(20.)));
    MeshRepairStats stats;
    REQUIRE(repair_mesh(mesh, &stats));
    CHECK(stats.modified);
    check_solid(mesh, 20. * 20. * 20.);
    CHECK(mesh.its.vertices.size() == 8);
}

TEST_CASE("Flipped triangles take the orientation of their neighbours", "[MeshRepair]")
{
    indexed_triangle_set its = cube(20.);
    std::swap(its.indices[1][1], its.indices[1][2]);
    std::swap(its.indices[5][1], its.indices[5][2]);
    TriangleMesh mesh(std::move(its));
    REQUIRE(repair_mesh(mesh));
    check_solid(mesh, 20. * 20. * 20.);
}

TEST_CASE("An inside-out mesh is turned outward", "[MeshRepair]")
{
    indexed_triangle_set its = cube(20.);
    its_flip_triangles(its);
    TriangleMesh mesh(std::move(its));
    REQUIRE(repair_mesh(mesh));
    check_solid(mesh, 20. * 20. * 20.);
}

TEST_CASE("Holes are filled", "[MeshRepair]")
{
    indexed_triangle_set its = cube(20.);
    its.indices.erase(its.indices.begin(), its.indices.begin() + 3);
    TriangleMesh mesh(std::move(its));
    MeshRepairStats stats;
    REQUIRE(repair_mesh(mesh, &stats));
    CHECK(stats.filled_holes > 0);
    check_solid(mesh, 20. * 20. * 20.);
}

TEST_CASE("Cracks between triangles that should share corners are closed", "[MeshRepair]")
{
    indexed_triangle_set its = soup(cube(20.));
    // Shift the corners of two triangles by far less than a micron, as float rounding in an exporter would.
    for (std::size_t v = 0; v < 6; ++v)
        its.vertices[v] += Vec3f(1e-5f, -1e-5f, 1e-5f);
    TriangleMesh mesh(std::move(its));
    MeshRepairStats stats;
    REQUIRE(repair_mesh(mesh, &stats));
    CHECK(stats.welded_vertices > 0);
    check_solid(mesh, 20. * 20. * 20.);
}

TEST_CASE("A T-junction is closed", "[MeshRepair]")
{
    // The top of the cube is split at the middle of its front edge, which the front side does not share.
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {20, 0, 0}, {20, 20, 0}, {0, 20, 0}, {0, 0, 20}, {20, 0, 20}, {20, 20, 20}, {0, 20, 20}, {10, 0, 20}};
    its.indices  = {{0, 2, 1}, {0, 3, 2}, {4, 8, 7}, {8, 6, 7}, {8, 5, 6}, {0, 1, 5}, {0, 5, 4},
                    {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
    TriangleMesh mesh(std::move(its));
    REQUIRE(repair_mesh(mesh));
    check_solid(mesh, 20. * 20. * 20.);
}

TEST_CASE("Crossing shells stay separate parts and are made solid", "[MeshRepair]")
{
    indexed_triangle_set inside_out = cube(20., {10., 10., 10.});
    its_flip_triangles(inside_out);
    TriangleMesh mesh(merged({cube(20.), inside_out}));
    MeshRepairStats stats;
    REQUIRE(repair_mesh(mesh, &stats));
    CHECK(stats.reversed_shells == 1);
    CHECK(its_num_open_edges(mesh.its) == 0);
    CHECK_THAT(its_volume(mesh.its), WithinRel(2 * 8000., 1e-4));
    CHECK(mesh.split().size() == 2);
}

TEST_CASE("Native acceptance preserves crossing shells accepted by the official volume repair", "[MeshRepair][NativeRepairCompatibility]")
{
    const double size = GENERATE(1., 20., 1000.);
    auto second = cube(size, Vec3d(size * .5, size * .5, size * .5));
    its_flip_triangles(second);
    const auto original = merged({cube(size), second});
    Model reference;
    auto* volume = reference.add_object()->add_volume(TriangleMesh(original));
    // add_volume centers its copy for placement; compare the same local input
    // to the official algorithm rather than comparing different transforms.
    volume->set_mesh(TriangleMesh(original));
    MeshRepairStats stats;
    std::string error;
    REQUIRE(volume->repair(true, &stats, &error));
    REQUIRE(error.empty());
    const auto& official = volume->mesh();
    REQUIRE(MeshBoolean::cgal::does_self_intersect(official));
    StrictMeshRepairOptions options;
    options.allow_native_repair_geometry_change = true;
    options.acceptance = MeshRepairAcceptance::OrcaNative;
    const auto candidate = strict_repair_mesh(original, options);
    INFO(candidate.error);
    REQUIRE(candidate.success);
    CHECK(candidate.native_repair_succeeded);
    CHECK(candidate.acceptance == MeshRepairAcceptance::OrcaNative);
    CHECK(candidate.output_topology.self_intersects);
    CHECK_FALSE(candidate.output_topology.safe());
    CHECK(candidate.repaired_mesh.indices == official.its.indices);
    REQUIRE(candidate.repaired_mesh.vertices.size() == official.its.vertices.size());
    for (size_t i = 0; i < official.its.vertices.size(); ++i)
        CHECK(candidate.repaired_mesh.vertices[i] == official.its.vertices[i]);
    CHECK(original.indices == merged({cube(size), second}).indices);
}

TEST_CASE("Official positive volume repairs retain their slicing semantics through candidate acceptance",
          "[MeshRepair][NativeRepairCompatibility][OfficialRepairMatrix]")
{
    const int scenario = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13);
    const bool reordered = GENERATE(false, true);
    CAPTURE(scenario, reordered);
    auto input = cube(20.);
    switch (scenario) {
    case 1: input.indices.erase(input.indices.begin(), input.indices.begin() + 3); break;
    case 2: std::swap(input.indices[0][1], input.indices[0][2]); break;
    case 3: its_flip_triangles(input); break;
    case 4:
        input = soup(input);
        for (size_t v = 0; v < 6; ++v) input.vertices[v] += Vec3f(1e-5f, -1e-5f, 1e-5f);
        break;
    case 5:
        input.vertices = {{0,0,0},{20,0,0},{20,20,0},{0,20,0},{0,0,20},{20,0,20},{20,20,20},{0,20,20},{10,0,20}};
        input.indices = {{0,2,1},{0,3,2},{4,8,7},{8,6,7},{8,5,6},{0,1,5},{0,5,4},
            {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
        break;
    case 6: {
        auto cavity = cube(10., {5.,5.,5.}); its_flip_triangles(cavity);
        input = merged({input, cavity}); break;
    }
    case 7: input = merged({input, cube(10., {5.,5.,5.})}); break;
    case 8: input = soup(merged({input, cube(20., {20.,0.,0.})})); break;
    case 9: {
        auto back = input; its_flip_triangles(back); input = soup(merged({input, back})); break;
    }
    case 10:
        input.vertices.insert(input.vertices.end(), {{30,0,0},{40,0,0},{30,10,0}});
        input.indices.emplace_back(8,9,10); break;
    case 11: input = merged({input, cube(5., {30.,5.,5.})}); break;
    case 12: input = open_pipe(64, 10., 7., 20.); break;
    case 13: input = soup(input); break;
    default: break;
    }
    if (reordered) {
        std::mt19937 random(610219); std::shuffle(input.indices.begin(), input.indices.end(), random);
    }
    const auto untouched = input;
    Model model;
    auto* volume = model.add_object()->add_volume(TriangleMesh(input), ModelVolumeType::MODEL_PART, false);
    MeshRepairStats stats;
    std::string error;
    REQUIRE(volume->repair(true, &stats, &error));
    REQUIRE(error.empty());
    REQUIRE_FALSE(volume->mesh().empty());
    StrictMeshRepairOptions options;
    options.acceptance = MeshRepairAcceptance::OrcaNative;
    options.allow_native_repair_geometry_change = true;
    const auto candidate = strict_repair_mesh(input, options);
    INFO(candidate.error);
    REQUIRE(candidate.accepted());
    REQUIRE_FALSE(candidate.repaired_mesh.indices.empty());
    // Slice with the same nonzero rule used by native printing. Inward shells
    // must remain cavities; same-direction internal shells add no solid area.
    const std::vector<float> heights {1.f, 6.f, 10.f, 14.f, 19.f};
    const auto official_slices = slice_mesh_ex(volume->mesh().its, heights);
    const auto actual_slices = slice_mesh_ex(candidate.repaired_mesh, heights);
    REQUIRE(actual_slices.size() == official_slices.size());
    for (size_t i = 0; i < actual_slices.size(); ++i) {
        const auto missing = diff_ex(official_slices[i], actual_slices[i]);
        const auto extra = diff_ex(actual_slices[i], official_slices[i]);
        double changed_area = 0.;
        for (const auto& polygon : missing) changed_area += polygon.area();
        for (const auto& polygon : extra) changed_area += polygon.area();
        CHECK(changed_area * SCALING_FACTOR * SCALING_FACTOR <= 1e-3);
    }
    CHECK(input.indices == untouched.indices);
    CHECK(input.vertices == untouched.vertices);
}

TEST_CASE("Native acceptance binds actual output and rejects invalid inputs and cancellation",
          "[MeshRepair][NativeRepairCompatibility]")
{
    auto original = cube(20.);
    const auto hash = mesh_repair_geometry_id(original);
    REQUIRE(hash.size() == 64);
    auto expanded = original;
    expanded.vertices.clear(); expanded.indices.clear();
    for (const auto& face : original.indices) {
        const int offset = int(expanded.vertices.size());
        for (int c = 0; c < 3; ++c) expanded.vertices.push_back(original.vertices[face[c]]);
        expanded.indices.emplace_back(offset, offset + 1, offset + 2);
    }
    CHECK(mesh_repair_geometry_id(expanded) == hash);
    expanded.vertices.front().x() += 1.f;
    CHECK(mesh_repair_geometry_id(expanded) != hash);
    StrictMeshRepairOptions options;
    options.acceptance = MeshRepairAcceptance::OrcaNative;
    const auto canceled = strict_repair_mesh(original, options, {}, [] { return true; });
    CHECK(canceled.canceled);
    CHECK_FALSE(canceled.accepted());
    original.indices.front()[0] = int(original.vertices.size());
    CHECK(mesh_repair_geometry_id(original).empty());
    const auto invalid = strict_repair_mesh(original, options);
    CHECK_FALSE(invalid.success);
    CHECK_FALSE(invalid.accepted());
    CHECK_FALSE(invalid.enhanced_repair_attempted);
}

TEST_CASE("Solids touching face to face in a soup lose the wall between them", "[MeshRepair]")
{
    // The mirrored copy shares the wall at x = 10 with exactly the same triangles, reversed.
    TriangleMesh right = make_cube(10., 10., 10.);
    right.mirror(X);
    right.translate(20.f, 0.f, 0.f);
    TriangleMesh mesh(soup(merged({cube(10.), right.its})));
    REQUIRE(repair_mesh(mesh));
    check_solid(mesh, 2 * 1000.);
    CHECK(mesh.split().size() == 1);
}

TEST_CASE("A shell inside a solid follows the nonzero winding rule of slicing", "[MeshRepair]")
{
    SECTION("Oriented like the outer shell, it adds nothing and is removed")
    {
        TriangleMesh mesh(merged({cube(20.), cube(10., {5., 5., 5.})}));
        REQUIRE(repair_mesh(mesh));
        check_solid(mesh, 8000.);
        CHECK(mesh.its.indices.size() == 12);
    }
    SECTION("Oriented inward, it is a cavity and stays")
    {
        indexed_triangle_set cavity = cube(10., {5., 5., 5.});
        its_flip_triangles(cavity);
        TriangleMesh mesh(merged({cube(20.), cavity}));
        MeshRepairStats stats;
        REQUIRE(repair_mesh(mesh, &stats));
        CHECK_FALSE(stats.modified);
        CHECK_THAT(its_volume(mesh.its), WithinRel(8000. - 1000., 1e-4));
    }
}

TEST_CASE("A pipe missing its end caps gets annular caps", "[MeshRepair]")
{
    const int    n = 64;
    const double h = 20., r_out = 10., r_in = 7.;
    TriangleMesh mesh(open_pipe(n, r_out, r_in, h));
    REQUIRE(repair_mesh(mesh));
    check_solid(mesh, h * (regular_polygon_area(n, r_out) - regular_polygon_area(n, r_in)));
}

TEST_CASE("A stray flat triangle is dropped", "[MeshRepair]")
{
    indexed_triangle_set its = cube(20.);
    its.vertices.insert(its.vertices.end(), {{30, 0, 0}, {40, 0, 0}, {30, 10, 0}});
    its.indices.emplace_back(8, 9, 10);
    TriangleMesh mesh(std::move(its));
    MeshRepairStats stats;
    REQUIRE(repair_mesh(mesh, &stats));
    CHECK(stats.removed_shells == 1);
    check_solid(mesh, 8000.);
}

TEST_CASE("A two-sided export keeps one side of each triangle", "[MeshRepair]")
{
    indexed_triangle_set back = cube(20.);
    its_flip_triangles(back);
    TriangleMesh mesh(soup(merged({cube(20.), back})));
    REQUIRE(repair_mesh(mesh));
    check_solid(mesh, 8000.);
}

TEST_CASE("A valid mesh is left untouched", "[MeshRepair]")
{
    TriangleMesh       mesh = make_sphere(10., 2. * PI / 36.);
    const TriangleMesh original = mesh;
    MeshRepairStats    stats;
    REQUIRE(repair_mesh(mesh, &stats));
    CHECK_FALSE(stats.modified);
    CHECK(mesh.its.vertices == original.its.vertices);
    CHECK(mesh.its.indices == original.its.indices);
}

TEST_CASE("A mesh enclosing no volume is reported and left untouched", "[MeshRepair]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};
    its.indices  = {{0, 1, 2}, {0, 2, 3}};
    TriangleMesh mesh(its);
    std::string  error;
    CHECK_FALSE(repair_mesh(mesh, nullptr, &error));
    CHECK_FALSE(error.empty());
    CHECK(mesh.its.indices.size() == 2);
}

TEST_CASE("Progress is reported in order and throwing from it cancels the repair", "[MeshRepair]")
{
    TriangleMesh     mesh(soup(cube(20.)));
    std::vector<int> reported;
    REQUIRE(repair_mesh(mesh, nullptr, nullptr, [&reported](int percent) { reported.push_back(percent); }));
    CHECK_FALSE(reported.empty());
    CHECK(std::is_sorted(reported.begin(), reported.end()));

    struct Canceled {};
    TriangleMesh       canceled(soup(cube(20.)));
    const TriangleMesh original = canceled;
    CHECK_THROWS_AS(repair_mesh(canceled, nullptr, nullptr, [](int) { throw Canceled(); }), Canceled);
    CHECK(canceled.its.vertices == original.its.vertices);
    CHECK(canceled.its.indices == original.its.indices);
}

TEST_CASE("Repair diagnostics batch every completed stage and flush canceled attempts independently", "[MeshRepair][RepairDiagnostics]")
{
    Slic3r::Test::DiagnosticLogCapture capture;
    auto source = cube(20.);
    source.indices.pop_back();
    for (int attempt = 0; attempt < 2; ++attempt) {
        TriangleMesh mesh(source);
        std::string error;
        REQUIRE(MeshBoolean::cgal::repair(mesh, nullptr, &error));
        CHECK(strict_repair_mesh(mesh.its).input_topology.safe());
        const auto backend = capture.sink->locked_backend();
        REQUIRE(backend->messages.size() == size_t(attempt + 1));
        const auto& message = backend->messages.back();
        CHECK(message.find("begin stage=repair_mesh_total") != std::string::npos);
        CHECK(message.find("end stage=repair_polygon_soup") != std::string::npos);
        CHECK(message.find("end stage=check_float_self_intersections") != std::string::npos);
        CHECK(message.find("end stage=repair_mesh_total") != std::string::npos);
        CHECK(message.find("elapsed_us=") != std::string::npos);
        CHECK(message.find("offset_us=") != std::string::npos);
    }
    TriangleMesh canceled(source);
    int checks = 0;
    std::string error;
    CHECK_FALSE(MeshBoolean::cgal::repair(canceled, nullptr, &error, [&] { return ++checks >= 4; }));
    CHECK(error == "Repair canceled");
    CHECK(canceled.its.vertices == source.vertices);
    CHECK(canceled.its.indices == source.indices);
    const auto backend = capture.sink->locked_backend();
    REQUIRE(backend->messages.size() == 3);
    CHECK(backend->messages.back().find("end stage=repair_mesh_total") != std::string::npos);
}

TEST_CASE("A failed diagnostic sink cannot reject a successfully repaired mesh", "[MeshRepair][RepairDiagnostics]")
{
    Slic3r::Test::DiagnosticLogCapture capture;
    capture.sink->locked_backend()->fail = true;
    auto source = cube(20.);
    source.indices.pop_back();
    TriangleMesh mesh(source);
    std::string error;
    REQUIRE(MeshBoolean::cgal::repair(mesh, nullptr, &error));
    CHECK(error.empty());
    CHECK(strict_repair_mesh(mesh.its).input_topology.safe());
}

TEST_CASE("A failed repair summary sink preserves the accepted result", "[MeshRepair][RepairDiagnostics][StageDiagnostics]")
{
    Slic3r::Test::DiagnosticLogCapture capture("strict_repair_mesh:");
    capture.sink->locked_backend()->fail = true;
    StrictMeshRepairResult result;
    REQUIRE_NOTHROW(result = strict_repair_mesh(cube(20.)));
    REQUIRE(result.success);
    CHECK(result.output_topology.safe());
}

TEST_CASE("Strict mesh repair certifies valid geometry without a second repair", "[MeshRepair][StrictMeshRepair]")
{
    const indexed_triangle_set source = cube(20.);
    const StrictMeshRepairResult result = strict_repair_mesh(source);
    REQUIRE(result.success);
    CHECK_FALSE(result.canceled);
    CHECK_FALSE(result.first_cgal_attempted);
    CHECK_FALSE(result.enhanced_repair_attempted);
    CHECK_FALSE(result.second_cgal_attempted);
    CHECK(result.input_topology.safe());
    CHECK(result.output_topology.safe());
    CHECK(result.geometry_verified);
    CHECK(result.repaired_mesh.vertices == source.vertices);
    CHECK(result.repaired_mesh.indices == source.indices);
}

TEST_CASE("Strict mesh repair starts fallbacks from the immutable input and publishes only a certificate", "[MeshRepair][StrictMeshRepair]")
{
    const indexed_triangle_set source = soup(cube(20.));
    std::vector<unsigned> progress;
    const StrictMeshRepairResult result = strict_repair_mesh(source, {},
        [&](const char*, unsigned value) { progress.push_back(value); });
    REQUIRE(result.success);
    CHECK_FALSE(result.input_topology.safe());
    CHECK(result.first_cgal_attempted);
    CHECK(result.output_topology.safe());
    CHECK(result.geometry_verified);
    CHECK_FALSE(result.repaired_mesh.empty());
    CHECK_FALSE(progress.empty());
    CHECK(progress.back() == 100);
}

TEST_CASE("Strict mesh repair rejects cancellation without publishing geometry", "[MeshRepair][StrictMeshRepair]")
{
    const indexed_triangle_set source = soup(cube(20.));
    int checks = 0;
    const StrictMeshRepairResult result = strict_repair_mesh(source, {}, {}, [&] { return ++checks >= 2; });
    CHECK_FALSE(result.success);
    CHECK(result.canceled);
    CHECK(result.repaired_mesh.empty());
}

TEST_CASE("Native compatible repair accepts necessary hole filling without changing the strict default", "[MeshRepair][StrictMeshRepair][NativeRepairCompatibility]")
{
    const double scale = GENERATE(1.0, 20.0, 1000.0);
    auto source = cube(scale);
    const bool triangle_soup = GENERATE(false, true);
    source.indices.pop_back();
    if (triangle_soup) source = soup(source);
    const auto original = source;
    TriangleMesh native(source);
    REQUIRE(repair_mesh(native));
    const auto strict = strict_repair_mesh(source);
    CHECK_FALSE(strict.success);
    CHECK_FALSE(strict.native_repair_compatible);
    StrictMeshRepairOptions options;
    options.allow_native_repair_geometry_change = true;
    options.acceptance = GENERATE(MeshRepairAcceptance::Strict, MeshRepairAcceptance::OrcaNative);
    const auto compatible = strict_repair_mesh(source, options);
    REQUIRE(compatible.success);
    CHECK(compatible.native_repair_compatible);
    CHECK_FALSE(compatible.geometry_verified);
    CHECK(compatible.strategy == "orca-native");
    CHECK(compatible.enhanced_repair_attempted);
    CHECK_FALSE(compatible.second_cgal_attempted);
    CHECK(compatible.output_topology.safe());
    CHECK(compatible.acceptance == MeshRepairAcceptance::Strict);
    CHECK_FALSE(compatible.native_repair_succeeded);
    CHECK(compatible.repaired_mesh.vertices == native.its.vertices);
    CHECK(compatible.repaired_mesh.indices == native.its.indices);
    CHECK(source.vertices == original.vertices);
    CHECK(source.indices == original.indices);
}

TEST_CASE("Enabling native compatibility adds no repair pass for an already safe mesh", "[MeshRepair][StrictMeshRepair][NativeRepairCompatibility]")
{
    const auto source = cube(20.0);
    StrictMeshRepairOptions options;
    options.allow_native_repair_geometry_change = true;
    const auto result = strict_repair_mesh(source, options);
    REQUIRE(result.success);
    CHECK(result.geometry_verified);
    CHECK(result.area_change_ratio == 0.);
    CHECK(result.sampled_deviation == 0.);
    CHECK(result.sampled_deviation_ratio == 0.);
    CHECK_FALSE(result.native_repair_compatible);
    CHECK_FALSE(result.first_cgal_attempted);
    CHECK_FALSE(result.enhanced_repair_attempted);
    CHECK(result.repaired_mesh.vertices == source.vertices);
    CHECK(result.repaired_mesh.indices == source.indices);
}

TEST_CASE("Read only deviation queries preserve repair output and geometry facts across thread counts",
          "[MeshRepair][StrictMeshRepair][NativeRepairCompatibility]")
{
    auto source = make_sphere(10., PI / 24.).its;
    source.indices.pop_back();
    StrictMeshRepairOptions options;
    options.allow_native_repair_geometry_change = true;
    const auto serial = tbb::task_arena(1).execute([&] { return strict_repair_mesh(source, options); });
    const auto parallel = tbb::task_arena(4).execute([&] { return strict_repair_mesh(source, options); });
    REQUIRE(serial.success);
    REQUIRE(parallel.success);
    CHECK(serial.repaired_mesh.vertices == parallel.repaired_mesh.vertices);
    CHECK(serial.repaired_mesh.indices == parallel.repaired_mesh.indices);
    CHECK(serial.sampled_deviation == parallel.sampled_deviation);
    CHECK(serial.sampled_deviation_ratio == parallel.sampled_deviation_ratio);
    CHECK(serial.area_change_ratio == parallel.area_change_ratio);
    CHECK(serial.geometry_verified == parallel.geometry_verified);
    CHECK(serial.native_repair_compatible == parallel.native_repair_compatible);
    CHECK(serial.strategy == parallel.strategy);
    CHECK(serial.output_topology.safe());
    CHECK(parallel.output_topology.safe());
}

TEST_CASE("Float output validation binds a completed intersection check to the actual geometry and clears on cancellation",
          "[MeshRepair][StrictMeshRepair][NativeRepairCompatibility]")
{
    const bool fill_hole = GENERATE(false, true);
    auto source = cube(20.);
    if (fill_hole) source.indices.pop_back();
    TriangleMesh checked(source), ordinary(source);
    MeshBoolean::cgal::FloatOutputValidation validation;
    CHECK_FALSE(validation.matches(source));
    std::string error;
    REQUIRE(MeshBoolean::cgal::repair(checked, nullptr, &error, {}, &validation));
    CHECK(error.empty());
    REQUIRE(MeshBoolean::cgal::repair(ordinary));
    CHECK(validation.matches(checked.its));
    CHECK(checked.its.vertices == ordinary.its.vertices);
    CHECK(checked.its.indices == ordinary.its.indices);
    CHECK_FALSE(MeshBoolean::cgal::does_self_intersect(checked));
    auto changed = checked.its;
    changed.vertices.front().x() += .5f;
    CHECK_FALSE(validation.matches(changed));
    changed = checked.its;
    std::reverse(changed.indices.begin(), changed.indices.end());
    CHECK_FALSE(validation.matches(changed));
    changed = checked.its;
    changed.indices.front()[0] = int(changed.vertices.size());
    CHECK_FALSE(validation.matches(changed));
    changed = checked.its;
    changed.vertices.front().x() = std::numeric_limits<float>::infinity();
    CHECK_FALSE(validation.matches(changed));
    TriangleMesh canceled(source);
    CHECK_FALSE(MeshBoolean::cgal::repair(canceled, nullptr, &error, [] { return true; }, &validation));
    CHECK_FALSE(validation.matches(checked.its));
    CHECK(canceled.its.vertices == source.vertices);
    CHECK(canceled.its.indices == source.indices);
}

TEST_CASE("Geometry change measurements ignore duplicate faces with signed zero corners",
          "[MeshRepair][StrictMeshRepair][NativeRepairCompatibility]")
{
    const bool negative_zero = GENERATE(false, true);
    auto source = soup(cube(20.));
    source.indices.pop_back();
    const int offset = int(source.vertices.size());
    const auto first = source.indices.front();
    for (int c = 0; c < 3; ++c) {
        auto point = source.vertices[first[c]];
        if (negative_zero) for (int axis = 0; axis < 3; ++axis) if (point[axis] == 0.f) point[axis] = -0.f;
        source.vertices.push_back(point);
    }
    source.indices.emplace_back(offset, offset + 1, offset + 2);
    StrictMeshRepairOptions options;
    options.allow_native_repair_geometry_change = true;
    const auto result = strict_repair_mesh(source, options);
    INFO(result.error);
    REQUIRE(result.success);
    CHECK(result.output_topology.safe());
    CHECK_THAT(result.area_change_ratio, WithinRel(1. / 11., 1e-12));
    CHECK(result.sampled_deviation > 0.);
}

TEST_CASE("Exact unchanged surfaces retain zero deviation after duplicate removal welding and face reordering",
          "[MeshRepair][StrictMeshRepair][NativeRepairCompatibility]")
{
    const bool triangle_soup = GENERATE(false, true);
    const bool reorder = GENERATE(false, true);
    auto source = make_sphere(10., PI / 24.).its;
    source.indices.push_back(source.indices.front());
    source.indices.push_back(source.indices[source.indices.size() / 3]);
    if (triangle_soup) source = soup(source);
    if (reorder) {
        std::mt19937 random(21937);
        std::shuffle(source.indices.begin(), source.indices.end(), random);
    }
    const auto original = source;
    StrictMeshRepairOptions options;
    options.maximum_sampled_deviation_ratio = 0.;
    options.maximum_area_change_ratio = 1e-12;
    const auto result = strict_repair_mesh(source, options);
    INFO(result.error);
    REQUIRE(result.success);
    CHECK(result.output_topology.safe());
    CHECK(result.geometry_verified);
    CHECK(result.sampled_deviation == 0.);
    CHECK(result.sampled_deviation_ratio == 0.);
    CHECK(result.area_change_ratio <= options.maximum_area_change_ratio);
    CHECK_FALSE(result.enhanced_repair_attempted);
    CHECK(source.vertices == original.vertices);
    CHECK(source.indices == original.indices);
}

TEST_CASE("Resource exhaustion never starts a second repair strategy", "[MeshRepair][NativeRepairCompatibility]")
{
    auto source = cube(20.);
    source.indices.pop_back();
    StrictMeshRepairOptions options;
    options.acceptance = MeshRepairAcceptance::OrcaNative;
    bool exhausted = false;
    const auto result = strict_repair_mesh(source, options, [&](const char* message, unsigned) {
        if (std::string(message).find("Repairing") != std::string::npos) {
            exhausted = true;
            throw std::bad_alloc();
        }
    });
    REQUIRE(exhausted);
    CHECK_FALSE(result.success);
    CHECK(result.resource_failure);
    CHECK_FALSE(result.enhanced_repair_attempted);
    CHECK_FALSE(result.second_cgal_attempted);
    CHECK(result.repaired_mesh.empty());
}

TEST_CASE("A closed repair outside geometry limits still attempts enhanced repair", "[MeshRepair][StrictMeshRepair]")
{
    auto source = cube(20.);
    source.indices.pop_back();
    const auto original = source;
    StrictMeshRepairOptions options;
    options.maximum_sampled_deviation_ratio = 0.;
    options.maximum_area_change_ratio = 0.;
    const auto result = strict_repair_mesh(source, options);
    CHECK(result.first_cgal_attempted);
    CHECK(result.enhanced_repair_attempted);
    CHECK_FALSE(result.success);
    CHECK_FALSE(result.canceled);
    CHECK(result.repaired_mesh.empty());
    CHECK(source.vertices == original.vertices);
    CHECK(source.indices == original.indices);
}

TEST_CASE("Cancellation before enhanced repair does not start another strategy", "[MeshRepair][StrictMeshRepair]")
{
    auto source = cube(20.);
    source.indices.pop_back();
    bool canceled = false;
    const auto result = strict_repair_mesh(source, {},
        [&](const char*, unsigned progress) { if (progress == 15) canceled = true; },
        [&] { return canceled; });
    CHECK(result.canceled);
    CHECK_FALSE(result.enhanced_repair_attempted);
    CHECK_FALSE(result.second_cgal_attempted);
    CHECK(result.repaired_mesh.empty());
}


TEST_CASE("Strict inspection retains duplicate and nonmanifold diagnostics before cancellation", "[MeshRepair][StrictMeshRepair]")
{
    indexed_triangle_set source;
    source.vertices = {{0,0,0},{1,0,0},{0,1,0},{0,-1,0},{0,0,1}};
    const bool duplicate = GENERATE(false, true);
    source.indices = duplicate ? std::vector<Vec3i32>{{0,1,2},{2,1,0}}
                               : std::vector<Vec3i32>{{0,1,2},{1,0,3},{0,1,4}};
    bool canceled = false;
    const auto result = strict_repair_mesh(source, {}, [&](const char* stage, unsigned) {
        if (std::string(stage).find("Repairing") == 0) canceled = true;
    }, [&] { return canceled; });
    CHECK(result.canceled);
    CHECK_FALSE(result.success);
    CHECK(result.repaired_mesh.indices.empty());
    if (duplicate) {
        CHECK(result.input_topology.duplicate_faces == 1);
    } else {
        CHECK(result.input_topology.nonmanifold_edges == 1);
        CHECK(result.input_topology.nonmanifold_vertices >= 2);
    }
}
