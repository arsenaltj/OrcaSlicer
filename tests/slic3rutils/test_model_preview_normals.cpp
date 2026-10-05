#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/GUI/AI/ModelGeneration/ModelPreviewNormals.hpp"
#include <cmath>
#include <limits>

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {
indexed_triangle_set hinged(float height)
{
    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0,0,0), Vec3f(1,0,0), Vec3f(0,1,0), Vec3f(1,1,height)};
    mesh.indices = {Vec3i32(0,1,2), Vec3i32(2,1,3)};
    return mesh;
}
}

TEST_CASE("Preview normals smooth a shallow crease on both incident faces", "[ModelPreviewNormals]")
{
    auto mesh = hinged(.2f);
    const auto normals = ModelPreviewNormals::corner_normals(mesh);
    REQUIRE(normals.size() == 6);
    CHECK((normals[1] - normals[4]).norm() < 1e-5f);
    CHECK((normals[2] - normals[3]).norm() < 1e-5f);
    CHECK(normals[0].z() > .99f);
    CHECK(normals[1].z() < 1.f);
    CHECK(normals[1].z() > .98f);
}

TEST_CASE("Preview normals keep a sharp fold and inverted thin sheet separate", "[ModelPreviewNormals]")
{
    auto mesh = hinged(2.f);
    const auto sharp = ModelPreviewNormals::corner_normals(mesh);
    CHECK((sharp[1] - sharp[4]).norm() > .5f);
    mesh.indices[1] = Vec3i32(1,2,3);
    const auto inverted = ModelPreviewNormals::corner_normals(mesh);
    CHECK(inverted[1].z() > .99f);
    CHECK(inverted[3].z() < -.25f);
}

TEST_CASE("Preview normals do not cross a nonmanifold edge or vertex-only contact", "[ModelPreviewNormals]")
{
    auto mesh = hinged(.2f);
    mesh.vertices.push_back(Vec3f(1,1,-.2f));
    mesh.indices.emplace_back(1,2,4);
    const auto nonmanifold = ModelPreviewNormals::corner_normals(mesh);
    CHECK((nonmanifold[1] - nonmanifold[4]).norm() > .1f);

    mesh.indices.pop_back();
    mesh.vertices.emplace_back(2,0,0);
    mesh.vertices.emplace_back(1,-1,0);
    mesh.indices[1] = Vec3i32(1,5,6);
    const auto vertex_only = ModelPreviewNormals::corner_normals(mesh);
    CHECK(vertex_only[1].z() > .99f);
    CHECK(vertex_only[3].z() < -.9f);
}

TEST_CASE("Disconnected triangle corners retain separate normals at coincident edges", "[ModelPreviewNormals]")
{
    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0,0,0), Vec3f(1,0,0), Vec3f(0,1,0),
                     Vec3f(0,1,0), Vec3f(1,0,0), Vec3f(1,1,.2f)};
    mesh.indices = {Vec3i32(0,1,2), Vec3i32(3,4,5)};
    const auto normals = ModelPreviewNormals::corner_normals(mesh);
    REQUIRE(normals.size() == 6);
    for (size_t corner = 0; corner < 3; ++corner) {
        CHECK((normals[corner] - Vec3f::UnitZ()).norm() < 1e-6f);
        CHECK((normals[corner + 3] - normals[3]).norm() < 1e-6f);
    }
    CHECK((normals[1] - normals[4]).norm() > .1f);
}

TEST_CASE("Isolated invalid and degenerate faces retain the finite fallback normal", "[ModelPreviewNormals]")
{
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0,0,0), Vec3f(0,0,0), Vec3f(0,0,0),
                     Vec3f(0,0,0), Vec3f(1e-5f,0,0), Vec3f(0,1e-5f,0),
                     Vec3f(invalid,0,0), Vec3f(1,0,0), Vec3f(0,1,0)};
    mesh.indices = {Vec3i32(0,1,2), Vec3i32(3,4,5), Vec3i32(6,7,8)};
    const auto normals = ModelPreviewNormals::corner_normals(mesh);
    REQUIRE(normals.size() == 9);
    for (const auto& normal : normals) {
        CHECK(normal.allFinite());
        CHECK((normal - Vec3f::UnitZ()).norm() < 1e-6f);
    }
}

TEST_CASE("Sparse high vertex IDs retain crease normals across the compact range boundary", "[ModelPreviewNormals]")
{
    const size_t count = GENERATE(size_t(1) << 20, (size_t(1) << 20) + 1);
    auto source = hinged(.2f);
    const auto expected = ModelPreviewNormals::corner_normals(source);
    indexed_triangle_set sparse;
    sparse.vertices.assign(count, Vec3f::Zero());
    const int offset = int(count - source.vertices.size());
    for (size_t vertex = 0; vertex < source.vertices.size(); ++vertex)
        sparse.vertices[size_t(offset) + vertex] = source.vertices[vertex];
    for (const auto& face : source.indices)
        sparse.indices.push_back(face + Vec3i32::Constant(offset));
    const auto normals = ModelPreviewNormals::corner_normals(sparse);
    REQUIRE(normals.size() == expected.size());
    for (size_t corner = 0; corner < expected.size(); ++corner)
        CHECK((normals[corner] - expected[corner]).norm() < 1e-6f);
}
