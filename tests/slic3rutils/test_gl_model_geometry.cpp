#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstring>
#include <limits>
#include <chrono>
#include <numeric>
#include <optional>
#include <thread>
#include <exception>
#include <boost/nowide/cstdlib.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <nlohmann/json.hpp>

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {
BoundingBoxf3 reference_bounds(const GLModel::Geometry& geometry)
{
    BoundingBoxf3 bounds;
    const auto dimensions = GLModel::Geometry::position_stride_floats(geometry.format);
    for (size_t i = 0; i < geometry.vertices_count(); ++i) {
        if (dimensions == 3)
            bounds.merge(geometry.extract_position_3(i).cast<double>());
        else if (dimensions == 2) {
            const auto point = geometry.extract_position_2(i);
            bounds.merge(Vec3f(point.x(), point.y(), 0.f).cast<double>());
        }
    }
    return bounds;
}

void check_exact_bounds(const BoundingBoxf3& actual, const BoundingBoxf3& expected)
{
    CHECK(actual.defined == expected.defined);
    CHECK(std::memcmp(actual.min.data(), expected.min.data(), 3 * sizeof(double)) == 0);
    CHECK(std::memcmp(actual.max.data(), expected.max.data(), 3 * sizeof(double)) == 0);
}

void install_geometry(GLModel& model, GLModel::Geometry&& geometry, bool background)
{
    if (!background) { model.init_from(std::move(geometry)); return; }
    std::optional<GLModel::PreparedGeometry> prepared;
    std::exception_ptr error;
    std::thread worker([&] {
        try { prepared = GLModel::prepare_geometry(std::move(geometry)); }
        catch (...) { error = std::current_exception(); }
    });
    worker.join();
    REQUIRE_FALSE(error); REQUIRE(prepared);
    model.init_from(std::move(*prepared));
}
}

TEST_CASE("Render attributes keep their exact layout when vertex storage grows", "[GLModelGeometry]")
{
    for (size_t reserve : {size_t(0), size_t(7)}) {
        DYNAMIC_SECTION("Initial vertex reserve " << reserve) {
            GLModel::Geometry geometry;
            geometry.format.vertex_layout = GLModel::Geometry::EVertexLayout::P3N3T2;
            geometry.reserve_vertices(reserve);
            std::vector<std::array<float, 8>> expected;
            for (int i = 0; i < 65; ++i) {
                // RGB8 packing, signed zero and the lock sentinel are GPU data,
                // so their exact bits matter independently of visual tolerances.
                expected.push_back({float(i), -0.0f, -float(i), 0.0f,
                    std::numeric_limits<float>::denorm_min(), 1.0f,
                    16777215.0f, i % 2 == 0 ? -1.0f : -0.0f});
                const auto& value = expected.back();
                geometry.add_vertex(Vec3f(value[0], value[1], value[2]),
                    Vec3f(value[3], value[4], value[5]), Vec2f(value[6], value[7]));
            }
            REQUIRE(geometry.vertices_count() == expected.size());
            REQUIRE(geometry.vertices_size_bytes() == expected.size() * 8 * sizeof(float));
            CHECK(std::memcmp(geometry.vertices.data(), expected.data(), geometry.vertices_size_bytes()) == 0);
            for (size_t i = 0; i < expected.size(); ++i) {
                const auto position = geometry.extract_position_3(i);
                const auto normal = geometry.extract_normal_3(i);
                const auto texture = geometry.extract_tex_coord_2(i);
                CHECK(std::memcmp(position.data(), expected[i].data(), 3 * sizeof(float)) == 0);
                CHECK(std::memcmp(normal.data(), expected[i].data() + 3, 3 * sizeof(float)) == 0);
                CHECK(std::memcmp(texture.data(), expected[i].data() + 6, 2 * sizeof(float)) == 0);
            }
        }
    }
}

TEST_CASE("Triangle extraction keeps vertex references and winding across storage growth", "[GLModelGeometry]")
{
    for (auto layout : {GLModel::Geometry::EVertexLayout::P3, GLModel::Geometry::EVertexLayout::P3N3T2}) {
        DYNAMIC_SECTION("Vertex layout " << int(layout)) {
            GLModel::Geometry geometry;
            geometry.format.vertex_layout = layout;
            geometry.reserve_indices(5);
            for (int i = 0; i < 195; ++i) {
                const Vec3f position(float(i), float(i % 7), -float(i));
                if (layout == GLModel::Geometry::EVertexLayout::P3)
                    geometry.add_vertex(position);
                else
                    geometry.add_vertex(position, Vec3f(0, 0, 1), Vec2f(0, 1));
            }
            for (unsigned i = 0; i < 65; ++i)
                geometry.add_triangle(3 * i + 2, 3 * i, 3 * i + 1);
            const auto mesh = geometry.get_as_indexed_triangle_set();
            REQUIRE(mesh.vertices.size() == 195);
            REQUIRE(mesh.indices.size() == 65);
            for (size_t i = 0; i < mesh.indices.size(); ++i) {
                CHECK(mesh.indices[i][0] == int(3 * i + 2));
                CHECK(mesh.indices[i][1] == int(3 * i));
                CHECK(mesh.indices[i][2] == int(3 * i + 1));
                const Vec3f expected(float(3 * i), float(3 * i % 7), -float(3 * i));
                CHECK(std::memcmp(mesh.vertices[mesh.indices[i][1]].data(), expected.data(), 3 * sizeof(float)) == 0);
            }
        }
    }
}

TEST_CASE("Render bounds preserve every stored position across layouts and reset", "[GLModelGeometry]")
{
    using Layout = GLModel::Geometry::EVertexLayout;
    for (auto layout : {Layout::P2, Layout::P2T2, Layout::P3, Layout::P3T2,
                        Layout::P3N3, Layout::P3N3T2, Layout::P4}) {
        DYNAMIC_SECTION("Vertex layout " << int(layout)) {
            for (bool special : {false, true}) {
                DYNAMIC_SECTION("Nonfinite positions " << special) {
                    GLModel::Geometry geometry;
                    geometry.format.vertex_layout = layout;
                    const size_t stride = GLModel::Geometry::vertex_stride_floats(geometry.format);
                    const size_t dimensions = GLModel::Geometry::position_stride_floats(geometry.format);
                    geometry.vertices.resize(65 * stride, 16777215.f);
                    for (size_t i = 0; i < 65; ++i) {
                        float* vertex = geometry.vertices.data() + i * stride;
                        vertex[0] = i == 0 ? -0.f : float(i % 11) - 5.f;
                        vertex[1] = i == 0 ? 0.f : float(i % 17) - 8.f;
                        if (dimensions >= 3) vertex[2] = -float(i);
                    }
                    // Bounds include unreferenced vertices, not only index targets.
                    geometry.vertices[64 * stride] = -std::numeric_limits<float>::max();
                    geometry.vertices[64 * stride + 1] = std::numeric_limits<float>::max();
                    if (special) {
                        geometry.vertices[0] = std::numeric_limits<float>::quiet_NaN();
                        geometry.vertices[stride] = std::numeric_limits<float>::infinity();
                        geometry.vertices[2 * stride] = -std::numeric_limits<float>::infinity();
                    }
                    geometry.indices = {2, 0, 1};
                    const auto expected = reference_bounds(geometry);
                    const auto vertices = geometry.vertices;
                    const auto indices = geometry.indices;
                    for (bool background : {false, true}) {
                        DYNAMIC_SECTION("Background preparation " << background) {
                            GLModel model;
                            auto data = geometry;
                            install_geometry(model, std::move(data), background);
                            check_exact_bounds(model.get_bounding_box(), expected);
                            REQUIRE(model.get_geometry().vertices.size() == vertices.size());
                            CHECK(std::memcmp(model.get_geometry().vertices.data(), vertices.data(), vertices.size() * sizeof(float)) == 0);
                            CHECK(model.get_geometry().indices == indices);
                            model.reset();
                            CHECK_FALSE(model.get_bounding_box().defined);
                            GLModel::Geometry replacement;
                            replacement.format.vertex_layout = layout;
                            replacement.vertices.resize(3 * stride, -0.f);
                            replacement.indices = {2, 0, 1};
                            const auto replacement_bounds = reference_bounds(replacement);
                            install_geometry(model, std::move(replacement), background);
                            check_exact_bounds(model.get_bounding_box(), replacement_bounds);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("Canceled render preparation preserves owned geometry and permits retry", "[GLModelGeometry]")
{
    GLModel::Geometry data;
    data.format.vertex_layout = GLModel::Geometry::EVertexLayout::P3;
    data.vertices.resize(9000 * 3, -0.f);
    data.indices = {0, 1, 2};
    const auto vertices = data.vertices;
    const auto indices = data.indices;
    size_t checkpoints = 0;
    std::exception_ptr error;
    std::thread worker([&] {
        try { GLModel::prepare_geometry(std::move(data), [&] { return ++checkpoints == 2; }); }
        catch (...) { error = std::current_exception(); }
    });
    worker.join();
    REQUIRE(error); CHECK(checkpoints == 2);
    CHECK(data.vertices == vertices); CHECK(data.indices == indices);
    const auto bounds = reference_bounds(data);
    GLModel model;
    install_geometry(model, std::move(data), true);
    check_exact_bounds(model.get_bounding_box(), bounds);
    CHECK(model.get_geometry().vertices == vertices);
    CHECK(model.get_geometry().indices == indices);
}

// Local CPU measurement of the actual GLModel initialization entry, excluding
// model loading, geometry filling/copying, verification, destruction and GPU.
TEST_CASE("Historical render initialization preserves exact bounds and buffers", "[.GLModelBoundsProbe]")
{
    const auto env = [](const char* name) {
        const auto value = boost::nowide::getenv(name);
        return value ? std::string(value) : std::string{};
    };
    const auto source = env("ORCA_GL_BOUNDS_SOURCE"), report = env("ORCA_GL_BOUNDS_REPORT");
    if (source.empty() || report.empty()) SKIP("Set a historical model and fresh report path.");
    REQUIRE_FALSE(boost::filesystem::exists(report));
    const auto source_hash = AI::model_artifact_sha256(source);
    TriangleMesh mesh; ObjInfo colors; std::string error;
    REQUIRE(AI::load_model_artifact(source, mesh, colors, error));
    nlohmann::json results = nlohmann::json::array();
    for (auto layout : {GLModel::Geometry::EVertexLayout::P3, GLModel::Geometry::EVertexLayout::P3N3T2}) {
        GLModel::Geometry input;
        input.format.vertex_layout = layout;
        const size_t stride = GLModel::Geometry::vertex_stride_floats(input.format);
        const size_t count = mesh.its.indices.size() * 3;
        input.vertices.resize(count * stride, 1.f);
        input.indices.resize(count);
        std::iota(input.indices.begin(), input.indices.end(), 0u);
        for (size_t f = 0; f < mesh.its.indices.size(); ++f)
            for (size_t c = 0; c < 3; ++c) {
                const auto& position = mesh.its.vertices[mesh.its.indices[f][c]];
                std::memcpy(input.vertices.data() + (f * 3 + c) * stride, position.data(), 3 * sizeof(float));
            }
        const auto expected = reference_bounds(input);
        nlohmann::json samples = nlohmann::json::array();
        for (int iteration = 0; iteration < 5; ++iteration) {
            auto data = input;
            GLModel model;
            const auto begin = std::chrono::steady_clock::now();
            model.init_from(std::move(data));
            const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            check_exact_bounds(model.get_bounding_box(), expected);
            REQUIRE(model.get_geometry().vertices.size() == input.vertices.size());
            CHECK(std::memcmp(model.get_geometry().vertices.data(), input.vertices.data(), input.vertices_size_bytes()) == 0);
            CHECK(model.get_geometry().indices == input.indices);
            samples.push_back({{"phase", iteration == 0 ? "first" : iteration == 1 ? "warmup" : "warm"}, {"elapsed_ms", elapsed}});
        }
        results.push_back({{"layout", int(layout)}, {"vertex_count", count}, {"samples", samples},
                           {"bounds_min", {expected.min.x(), expected.min.y(), expected.min.z()}},
                           {"bounds_max", {expected.max.x(), expected.max.y(), expected.max.z()}}});
    }
    boost::filesystem::ofstream output(report);
    output << nlohmann::json{{"source_sha256", source_hash}, {"faces", mesh.its.indices.size()}, {"results", results},
        {"scope", "CPU GLModel initialization only; no GPU/context or UI timing"}}.dump(2);
    output.close(); REQUIRE(output.good());
    REQUIRE(AI::model_artifact_sha256(source) == source_hash);
}
