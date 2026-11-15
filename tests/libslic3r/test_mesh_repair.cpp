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
#include <tbb/task_arena.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Polygon_mesh_processing/repair_self_intersections.h>
#include <CGAL/Polygon_mesh_processing/repair_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>

#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/MeshRepair.hpp>
#include <libslic3r/PolygonSoupPreparation.hpp>
#include <libslic3r/Point.hpp>
#include <libslic3r/libslic3r.h>
#include <libslic3r/TriangleMesh.hpp>

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
    indexed_triangle_set its;
    for (const double r : {r_out, r_in})
        for (int k = 0; k < n; ++k) {
            const double a = 2. * PI * k / n;
            its.vertices.emplace_back(float(r * std::cos(a)), float(r * std::sin(a)), 0.f);
            its.vertices.emplace_back(float(r * std::cos(a)), float(r * std::sin(a)), float(h));
        }
    for (int k = 0; k < n; ++k) {
        const int a0 = 2 * k, b0 = a0 + 1, a1 = 2 * ((k + 1) % n), b1 = a1 + 1;
        its.indices.emplace_back(a0, a1, b1);
        its.indices.emplace_back(a0, b1, b0);
        its.indices.emplace_back(2 * n + a0, 2 * n + b1, 2 * n + a1);
        its.indices.emplace_back(2 * n + a0, 2 * n + b0, 2 * n + b1);
    }
    TriangleMesh mesh(std::move(its));
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
