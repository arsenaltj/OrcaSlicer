#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include "slic3r/GUI/MeshUtils.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <memory>
#include <cstring>
#include <array>

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {
bool exact_normal(const Vec3f& actual, const Vec3f& expected)
{
    return std::memcmp(actual.data(), expected.data(), 3 * sizeof(float)) == 0;
}
}

TEST_CASE("Mesh picking normals retain the batch calculation at difficult faces", "[MeshRaycaster]")
{
    auto mesh = std::make_shared<TriangleMesh>(make_cube(3., 4., 5.));
    // Include repeated, reversed, degenerate and very small faces. Normal bits,
    // including signed zero and nonfinite values, matter independently of display.
    mesh->its.vertices.insert(mesh->its.vertices.end(), {
        Vec3f(-0.f, 0.f, 0.f), Vec3f(1e-12f, 0.f, 0.f), Vec3f(0.f, 1e-12f, 0.f)});
    const int first = int(mesh->its.vertices.size()) - 3;
    mesh->its.indices.emplace_back(first, first + 1, first + 2);
    mesh->its.indices.emplace_back(first + 2, first + 1, first);
    mesh->its.indices.emplace_back(first, first, first);
    mesh->its.indices.push_back(mesh->its.indices.front());
    const auto expected = its_face_normals(mesh->its);
    MeshRaycaster raycaster(mesh);
    for (size_t i = 0; i < expected.size(); ++i) {
        INFO(i);
        CHECK(exact_normal(raycaster.get_triangle_normal(i), expected[i]));
    }
}

TEST_CASE("Copied picking meshes preserve normals when the source mesh changes", "[MeshRaycaster]")
{
    TriangleMesh mesh = make_cube(3., 4., 5.);
    const auto expected = its_face_normals(mesh.its);
    MeshRaycaster raycaster(mesh);
    mesh.its.vertices[0] = Vec3f(17.f, -9.f, 32.f);
    mesh.its.indices.clear();
    for (size_t i = 0; i < expected.size(); ++i) {
        INFO(i);
        CHECK(exact_normal(raycaster.get_triangle_normal(i), expected[i]));
    }
}

TEST_CASE("Support and brim nearest points return the corresponding face normal", "[MeshRaycaster]")
{
    auto mesh = std::make_shared<TriangleMesh>(make_cube(3., 4., 5.));
    const auto expected = its_face_normals(mesh->its);
    MeshRaycaster raycaster(mesh);
    const std::array<Vec3f, 3> points{Vec3f(.27f, .61f, -2.f), Vec3f(1.23f, 1.41f, 8.f), Vec3f(-3.f, 1.71f, 2.33f)};
    for (const auto& point : points) {
        Vec3f normal;
        const Vec3f nearest = raycaster.get_closest_point(point, &normal);
        const int facet = raycaster.get_closest_facet(point);
        REQUIRE(facet >= 0);
        REQUIRE(size_t(facet) < expected.size());
        CHECK(exact_normal(normal, expected[size_t(facet)]));
        CHECK(exact_normal(nearest, raycaster.get_closest_point(point)));
    }
}
