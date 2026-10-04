#include <catch2/catch_all.hpp>

#include "slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "../test_utils.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <future>
#include <thread>
#include <cstdlib>
#include <limits>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#undef small
#endif

using namespace Slic3r;

namespace {
// Test-only access to compare every adjacency row without widening the editor API.
template<class Tag, typename Tag::Type Member> struct RegionProbeAccess {
    friend typename Tag::Type region_probe_member(Tag) { return Member; }
};
struct RegionNeighbors {
    using Type = std::vector<std::vector<uint32_t>> AI::VertexColorRegionEditor::*;
    friend Type region_probe_member(RegionNeighbors);
};
template struct RegionProbeAccess<RegionNeighbors, &AI::VertexColorRegionEditor::m_face_neighbors>;
struct RegionPickFaceOrder {
    using Type = std::vector<uint32_t> AI::VertexColorRegionEditor::*;
    friend Type region_probe_member(RegionPickFaceOrder);
};
template struct RegionProbeAccess<RegionPickFaceOrder, &AI::VertexColorRegionEditor::m_pick_face_order>;
}

TEST_CASE("Historical models expose repeatable region preparation timings", "[.][RegionPreparationProbe]")
{
    const char* source = std::getenv("ORCA_SELECTION_SOURCE");
    const char* output = std::getenv("ORCA_SELECTION_OUTPUT");
    REQUIRE(source != nullptr);
    REQUIRE(output != nullptr);
    REQUIRE_FALSE(boost::filesystem::exists(output));
    struct RestoreLogging {
        unsigned level = get_logging_level();
        ~RestoreLogging() { set_logging_level(level); }
    } restore_logging;
    set_logging_level(3);
    TriangleMesh mesh;
    ObjInfo colors;
    std::string error;
    REQUIRE(AI::load_model_artifact(source, mesh, colors, error));
    const auto hash = AI::model_artifact_sha256(source);
    REQUIRE_FALSE(hash.empty());
    Vec3f minimum = mesh.its.vertices.front();
    Vec3f maximum = minimum;
    for (const Vec3f& vertex : mesh.its.vertices) {
        minimum = minimum.cwiseMin(vertex);
        maximum = maximum.cwiseMax(vertex);
    }
    const double ray_padding = std::max(1.0, double((maximum - minimum).norm()));
    nlohmann::json report = {{"source_sha256", hash}, {"faces", mesh.its.indices.size()},
                             {"samples", nlohmann::json::array()}};
#ifdef _WIN32
    // The hidden performance probe can pin its calling thread to one allowed
    // logical processor. This changes neither application scheduling nor tests.
    struct RestoreThreadAffinity {
        DWORD_PTR mask {0};
        ~RestoreThreadAffinity() { if (mask) SetThreadAffinityMask(GetCurrentThread(), mask); }
    } restore_affinity;
    if (std::getenv("ORCA_SELECTION_PIN_THREAD")) {
        DWORD_PTR process_mask = 0, system_mask = 0;
        REQUIRE(GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask));
        REQUIRE(process_mask != 0);
        const DWORD_PTR pinned_mask = process_mask & (~process_mask + 1);
        restore_affinity.mask = SetThreadAffinityMask(GetCurrentThread(), pinned_mask);
        REQUIRE(restore_affinity.mask != 0);
        report["thread_affinity_mask"] = uint64_t(pinned_mask);
    }
    const auto thread_cpu_ticks = [] {
        FILETIME created, exited, kernel, user;
        REQUIRE(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user));
        const auto ticks = [](const FILETIME& value) {
            return (uint64_t(value.dwHighDateTime) << 32) | uint64_t(value.dwLowDateTime);
        };
        return ticks(kernel) + ticks(user);
    };
#endif
    for (int sample = 0; sample < 7; ++sample) {
        AI::VertexColorRegionEditor editor;
#ifdef _WIN32
        const uint64_t cpu_before = thread_cpu_ticks();
#endif
        const auto started = std::chrono::steady_clock::now();
        std::atomic<bool> canceled {false};
        const bool cooperative = std::getenv("ORCA_SELECTION_COOPERATIVE_CANCEL") != nullptr;
        const char* configured_mode = std::getenv("ORCA_SELECTION_PREPARATION_MODE");
        const std::string mode = configured_mode ? configured_mode : "eager";
        REQUIRE((mode == "eager" || mode == "picking" || mode == "upgrade"));
        const bool success = mode != "eager"
            ? editor.initialize_for_picking(mesh.its, colors.vertex_colors, error, [&] { return canceled.load(); })
            : cooperative ? editor.initialize(mesh.its, colors.vertex_colors, error, [&] { return canceled.load(); })
                          : editor.initialize(mesh.its, colors.vertex_colors, error);
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
#ifdef _WIN32
        const uint64_t cpu_after = thread_cpu_ticks();
#endif
        INFO(error);
        REQUIRE(success);
        REQUIRE(editor.mesh().vertices == mesh.its.vertices);
        REQUIRE(editor.mesh().indices == mesh.its.indices);
        REQUIRE(editor.vertex_colors() == colors.vertex_colors);
        REQUIRE(editor.selected_face_count() == 0);
        nlohmann::json row = {{"sample", sample}, {"initialize_ms", elapsed}, {"mode", mode},
            {"initial_region_ready", editor.region_selection_ready()}};
#ifdef _WIN32
        row["initialize_thread_cpu_ms"] = double(cpu_after - cpu_before) / 10000.0;
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        REQUIRE(K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)));
        row["process_lifetime_peak_working_set_bytes"] = uint64_t(counters.PeakWorkingSetSize);
#endif
        if (mode == "upgrade") {
            const auto begin = std::chrono::steady_clock::now();
            auto topology = editor.prepare_region_topology(error, [&] { return canceled.load(); });
            REQUIRE(topology);
            REQUIRE(editor.install_region_topology(std::move(topology)));
            row["upgrade_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        }
        REQUIRE(editor.region_selection_ready() == (mode != "picking"));
        // Stream the exact ordered neighbor lists; no large JSON/string allocation.
        const boost::filesystem::path adjacency_path = std::string(output) + ".neighbors";
        boost::filesystem::ofstream adjacency(adjacency_path, std::ios::binary | std::ios::trunc);
        for (const auto& neighbors : editor.*region_probe_member(RegionNeighbors{})) {
            const uint64_t count = neighbors.size();
            adjacency.write(reinterpret_cast<const char*>(&count), sizeof(count));
            if (!neighbors.empty()) adjacency.write(reinterpret_cast<const char*>(neighbors.data()),
                                                    neighbors.size() * sizeof(uint32_t));
        }
        adjacency.close();
        REQUIRE(bool(adjacency));
        row["adjacency_sha256"] = AI::model_artifact_sha256(adjacency_path);
        REQUIRE_FALSE(row["adjacency_sha256"].get<std::string>().empty());
        if (sample != 0) REQUIRE(row["adjacency_sha256"] == report["samples"][0]["adjacency_sha256"]);
        const boost::filesystem::path order_path = std::string(output) + ".pick_order";
        boost::filesystem::ofstream order_stream(order_path, std::ios::binary | std::ios::trunc);
        const auto& order = editor.*region_probe_member(RegionPickFaceOrder{});
        order_stream.write(reinterpret_cast<const char*>(order.data()), order.size() * sizeof(uint32_t));
        order_stream.close();
        REQUIRE(bool(order_stream));
        row["pick_order_sha256"] = AI::model_artifact_sha256(order_path);
        REQUIRE_FALSE(row["pick_order_sha256"].get<std::string>().empty());
        if (sample != 0) REQUIRE(row["pick_order_sha256"] == report["samples"][0]["pick_order_sha256"]);

        const boost::filesystem::path ray_path = std::string(output) + ".rays";
        boost::filesystem::ofstream ray_stream(ray_path, std::ios::binary | std::ios::trunc);
        for (int axis = 0; axis < 3; ++axis) {
            const int u = (axis + 1) % 3;
            const int v = (axis + 2) % 3;
            for (int row_index = 0; row_index < 9; ++row_index) {
                for (int column_index = 0; column_index < 9; ++column_index) {
                    Vec3d origin = Vec3d::Zero();
                    Vec3d direction = Vec3d::Zero();
                    origin[axis] = double(maximum[axis]) + ray_padding;
                    origin[u] = double(minimum[u]) + (double(row_index) + 0.5) / 9.0 *
                        double(maximum[u] - minimum[u]);
                    origin[v] = double(minimum[v]) + (double(column_index) + 0.5) / 9.0 *
                        double(maximum[v] - minimum[v]);
                    direction[axis] = -1.0;
                    const auto hit = editor.pick_face(origin, direction);
                    const uint64_t face = hit ? uint64_t(*hit) : std::numeric_limits<uint64_t>::max();
                    ray_stream.write(reinterpret_cast<const char*>(&face), sizeof(face));
                }
            }
        }
        ray_stream.close();
        REQUIRE(bool(ray_stream));
        row["ray_hits_sha256"] = AI::model_artifact_sha256(ray_path);
        REQUIRE_FALSE(row["ray_hits_sha256"].get<std::string>().empty());
        if (sample != 0) REQUIRE(row["ray_hits_sha256"] == report["samples"][0]["ray_hits_sha256"]);
        report["samples"].push_back(std::move(row));
    }
    REQUIRE(AI::model_artifact_sha256(source) == hash);
    boost::filesystem::ofstream stream(output);
    stream << report.dump(2);
    stream.close();
    REQUIRE(bool(stream));
}

namespace {

indexed_triangle_set square_mesh()
{
    indexed_triangle_set mesh;
    mesh.vertices = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {1.0f, 1.0f, 0.0f}
    };
    mesh.indices = {{0, 1, 2}, {1, 3, 2}};
    return mesh;
}

std::vector<RGBA> red_colors()
{
    return {
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f}
    };
}

indexed_triangle_set seamed_square_mesh(float gap = 0.0f)
{
    indexed_triangle_set mesh;
    mesh.vertices = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {1.0f + gap, 0.0f, 0.0f},
        {1.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}
    };
    mesh.indices = {{0, 1, 2}, {3, 4, 5}};
    return mesh;
}

std::vector<RGBA> solid_colors(size_t count, const RGBA& color)
{
    return std::vector<RGBA>(count, color);
}

indexed_triangle_set stacked_triangle_mesh(size_t layers)
{
    indexed_triangle_set mesh;
    mesh.vertices.reserve(layers * 3);
    mesh.indices.reserve(layers);
    for (size_t layer = 0; layer < layers; ++layer) {
        const float z = float(layer) * 0.01f;
        const uint32_t first = uint32_t(mesh.vertices.size());
        mesh.vertices.emplace_back(0.0f, 0.0f, z);
        mesh.vertices.emplace_back(1.0f, 0.0f, z);
        mesh.vertices.emplace_back(0.0f, 1.0f, z);
        mesh.indices.emplace_back(first, first + 1, first + 2);
    }
    return mesh;
}

indexed_triangle_set separated_triangle_mesh(size_t count)
{
    indexed_triangle_set mesh;
    mesh.vertices.reserve(count * 3);
    mesh.indices.reserve(count);
    for (size_t item = 0; item < count; ++item) {
        const float x = float(item) * 2.0f;
        const uint32_t first = uint32_t(mesh.vertices.size());
        mesh.vertices.emplace_back(x, 0.0f, 0.0f);
        mesh.vertices.emplace_back(x + 1.0f, 0.0f, 0.0f);
        mesh.vertices.emplace_back(x, 1.0f, 0.0f);
        mesh.indices.emplace_back(first, first + 1, first + 2);
    }
    return mesh;
}

indexed_triangle_set overhang_region_mesh()
{
    indexed_triangle_set mesh;
    mesh.vertices = {
        {20.0f, 0.0f, 0.0f}, {20.0f, 10.0f, 0.0f}, {30.0f, 0.0f, 0.0f}, {30.0f, 10.0f, 0.0f},
        {0.0f, 0.0f, 5.0f}, {0.0f, 10.0f, 5.0f}, {10.0f, 0.0f, 5.0f}, {10.0f, 10.0f, 5.0f},
        {40.0f, 0.0f, 3.0f}, {40.0f, 0.1f, 3.0f}, {40.1f, 0.0f, 3.0f},
        {50.0f, 0.0f, 4.0f}, {51.0f, 0.0f, 4.0f}, {50.0f, 1.0f, 4.0f}
    };
    mesh.indices = {
        {0, 1, 2}, {1, 3, 2},       // Downward bed-contact surface.
        {4, 5, 6}, {5, 7, 6},       // Significant elevated downward region.
        {8, 9, 10},                  // Elevated but too small to be significant.
        {11, 12, 13}                 // Upward-facing surface.
    };
    return mesh;
}

} // namespace

TEST_CASE("vertex color smart region follows connected color blocks", "[AI][VertexColorRegion]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    std::vector<RGBA> colors = red_colors();
    colors[3] = {0.0f, 0.0f, 1.0f, 1.0f};
    REQUIRE(editor.initialize(square_mesh(), colors, error));

    AI::RegionSelectionSettings settings;
    settings.color_distance = 0.12f;
    CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 1);

    colors = red_colors();
    REQUIRE(editor.initialize(square_mesh(), colors, error));
    CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 2);
}

TEST_CASE("vertex color smart region crosses duplicated vertex seams", "[AI][VertexColorRegion]")
{
    AI::RegionSelectionSettings settings;
    settings.color_distance = 0.12f;
    settings.normal_angle_degrees = 45.0f;
    for (float gap : {0.0f, 1e-7f}) {
        indexed_triangle_set mesh = seamed_square_mesh(gap);
        AI::VertexColorRegionEditor editor;
        std::string error;
        REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f}), error));
        CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 2);
    }
}

TEST_CASE("Independent corners retain geometric seam multiplicity and collapsed boundaries", "[VertexColorRegion]")
{
    const auto check_neighbors = [](const indexed_triangle_set& mesh,
                                   const std::vector<std::vector<uint32_t>>& expected) {
        AI::VertexColorRegionEditor editor;
        std::string error;
        REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1, 0, 0, 1}), error));
        CHECK(editor.mesh().vertices == mesh.vertices);
        CHECK(editor.mesh().indices == mesh.indices);
        CHECK(editor.*region_probe_member(RegionNeighbors{}) == expected);
    };
    SECTION("Duplicated endpoints still join the two faces") {
        auto mesh = seamed_square_mesh();
        mesh.vertices.emplace_back(100, 100, 100); // Unreferenced vertices remain intact.
        check_neighbors(mesh, {{1}, {0}});
    }
    SECTION("Three independent uses of a geometric edge stay disconnected") {
        indexed_triangle_set mesh;
        for (int i = 0; i < 3; ++i) {
            mesh.vertices.insert(mesh.vertices.end(), {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0)});
            mesh.indices.emplace_back(3 * i, 3 * i + 1, 3 * i + 2);
        }
        check_neighbors(mesh, {{}, {}, {}});
    }
    SECTION("Collapsed geometric endpoints do not create self neighbors") {
        indexed_triangle_set mesh;
        mesh.vertices = {Vec3f::Zero(), Vec3f::Zero(), Vec3f::Zero()};
        mesh.indices = {{0, 1, 2}};
        check_neighbors(mesh, {{}});
    }
}

TEST_CASE("Shared nonmanifold indices retain the indexed first-owner neighbors", "[VertexColorRegion]")
{
    indexed_triangle_set mesh;
    mesh.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}};
    mesh.indices = {{0, 1, 2}, {1, 0, 3}, {0, 1, 4}};
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1, 0, 0, 1}), error));
    const std::vector<std::vector<uint32_t>> expected {{1, 2}, {0}, {0}};
    CHECK(editor.*region_probe_member(RegionNeighbors{}) == expected);
    CHECK(editor.mesh().indices == mesh.indices);
}

TEST_CASE("Nearby seam endpoints choose the earliest representative in an occupied cell", "[VertexColorRegion]")
{
    const float shift = GENERATE(0.0f, -1e-7f);
    indexed_triangle_set mesh;
    mesh.vertices = {
        {shift + .1e-7f, shift + .1e-7f, 0},
        {shift + .9e-7f, shift + .9e-7f, 0}, // Distinct earlier canonical in the same cell.
        {shift + .5e-7f, shift + .5e-7f, 0}, // Within tolerance of both; must choose index 0.
        {0, 1e-6f, 0}, {1e-6f, 0, 0}, {0, 1e-6f, 0}, {-1e-6f, 0, 0}
    };
    mesh.indices = {{0, 3, 4}, {2, 5, 6}};
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1, 0, 0, 1}), error));
    const std::vector<std::vector<uint32_t>> expected {{1}, {0}};
    CHECK(editor.*region_probe_member(RegionNeighbors{}) == expected);
    CHECK(editor.mesh().vertices == mesh.vertices);
    CHECK(editor.mesh().indices == mesh.indices);
}

TEST_CASE("vertex color geometric seams preserve region boundaries", "[AI][VertexColorRegion]")
{
    AI::RegionSelectionSettings settings;
    settings.color_distance = 0.12f;
    settings.normal_angle_degrees = 45.0f;

    SECTION("positional gap") {
        indexed_triangle_set mesh = seamed_square_mesh(0.001f);
        AI::VertexColorRegionEditor editor;
        std::string error;
        REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f}), error));
        CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 1);
    }

    SECTION("different color") {
        indexed_triangle_set mesh = seamed_square_mesh();
        std::vector<RGBA> colors = solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f});
        for (size_t index = 3; index < colors.size(); ++index)
            colors[index] = {0.0f, 0.0f, 1.0f, 1.0f};
        AI::VertexColorRegionEditor editor;
        std::string error;
        REQUIRE(editor.initialize(std::move(mesh), std::move(colors), error));
        CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 1);
    }

    SECTION("sharp normal") {
        indexed_triangle_set mesh;
        mesh.vertices = {
            {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}
        };
        mesh.indices = {{0, 1, 2}, {3, 4, 5}};
        AI::VertexColorRegionEditor editor;
        std::string error;
        REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f}), error));
        CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 1);
    }

    SECTION("ambiguous coincident edge") {
        indexed_triangle_set mesh;
        mesh.vertices = {
            {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.2f, 0.2f, 0.0f}
        };
        mesh.indices = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}};
        AI::VertexColorRegionEditor editor;
        std::string error;
        REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f}), error));
        CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 1);
    }
}

TEST_CASE("Similar color clicks accumulate connected patches without selecting disconnected colors", "[VertexColorRegion]")
{
    auto mesh = seamed_square_mesh();
    // A disconnected triangle has the same blue as the second patch.
    mesh.vertices.insert(mesh.vertices.end(), {{10, 0, 0}, {11, 0, 0}, {10, 1, 0}});
    mesh.indices.emplace_back(6, 7, 8);
    auto colors = solid_colors(mesh.vertices.size(), {0, 0, 1, 1});
    for (size_t i = 0; i < 3; ++i) colors[i] = {1, 0, 0, 1};
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(std::move(mesh), std::move(colors), error));
    AI::RegionSelectionSettings settings;
    REQUIRE(editor.update_selection(0, AI::RegionSelectionOperation::AddSimilar, settings) == 1);
    const auto first = editor.selected_faces();
    REQUIRE(editor.update_selection(1, AI::RegionSelectionOperation::AddSimilar, settings) == 2);
    CHECK(editor.selected_faces() == std::vector<uint8_t>{1, 1, 0});
    CHECK(editor.update_selection(1, AI::RegionSelectionOperation::AddSimilar, settings) == 2);
    REQUIRE(editor.restore_selection(first));
    CHECK(editor.selected_faces() == std::vector<uint8_t>{1, 0, 0});
    CHECK(editor.update_selection(1, AI::RegionSelectionOperation::Replace, settings) == 1);
    CHECK(editor.selected_faces() == std::vector<uint8_t>{0, 1, 0});
}

TEST_CASE("vertex color local patches support add and remove", "[AI][VertexColorRegion]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));

    AI::RegionSelectionSettings settings;
    settings.local_radius_ratio = 1.0f;
    CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Add, settings) == 2);
    CHECK(editor.update_selection(1, AI::RegionSelectionOperation::Remove, settings) == 0);
}

TEST_CASE("vertex color selection snapshots can be restored safely", "[AI][VertexColorRegion]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));

    AI::RegionSelectionSettings settings;
    settings.local_radius_ratio = 0.01f;
    REQUIRE(editor.update_selection(0, AI::RegionSelectionOperation::Add, settings) == 1);
    const std::vector<uint8_t> snapshot = editor.selected_faces();
    REQUIRE(editor.update_selection(1, AI::RegionSelectionOperation::Add, settings) == 2);

    REQUIRE(editor.restore_selection(snapshot));
    CHECK(editor.selected_face_count() == 1);
    CHECK(editor.selected_faces() == snapshot);
    CHECK_FALSE(editor.restore_selection({1}));
    CHECK(editor.selected_faces() == snapshot);
}

TEST_CASE("Independent triangles with unique corners preserve selection and picking", "[VertexColorRegion][Regression]")
{
    indexed_triangle_set mesh;
    constexpr int faces = 1001;
    for (int face = 0; face < faces; ++face) {
        const float x = float(face % 31) * 10.0f;
        const float y = float(face / 31) * 10.0f;
        mesh.vertices.emplace_back(x, y, 0.0f);
        mesh.vertices.emplace_back(x + 1.0f, y, 0.0f);
        mesh.vertices.emplace_back(x, y + 1.0f, 0.0f);
        mesh.indices.emplace_back(face * 3, face * 3 + 1, face * 3 + 2);
    }
    std::vector<RGBA> colors(mesh.vertices.size(), RGBA{1, 0, 0, 1});
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(mesh, colors, error));
    REQUIRE(editor.mesh().vertices == mesh.vertices);
    REQUIRE(editor.mesh().indices == mesh.indices);
    REQUIRE(editor.vertex_colors() == colors);
    const auto& neighbors = editor.*region_probe_member(RegionNeighbors{});
    REQUIRE(neighbors.size() == faces);
    AI::RegionSelectionSettings settings;
    for (int face = 0; face < faces; ++face) {
        INFO(face);
        CHECK(neighbors[face].empty());
        CHECK(editor.update_selection(face, AI::RegionSelectionOperation::Replace, settings) == 1);
        const auto hit = editor.pick_face({double(face % 31) * 10 + 0.2,
                                          double(face / 31) * 10 + 0.2, 1.0}, {0, 0, -1});
        REQUIRE(hit.has_value());
        CHECK(*hit == size_t(face));
    }
}

TEST_CASE("vertex color material selection spans disconnected matching faces", "[AI][VertexColorRegion]")
{
    indexed_triangle_set mesh = separated_triangle_mesh(3);
    std::vector<RGBA> colors = solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f});
    for (size_t vertex = 3; vertex < 6; ++vertex)
        colors[vertex] = {0.0f, 0.0f, 1.0f, 1.0f};

    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(std::move(mesh), std::move(colors), error));
    const std::vector<RGBA> palette {
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f}
    };

    CHECK(editor.select_palette_material(palette, 0) == 2);
    CHECK(editor.selected_faces() == std::vector<uint8_t> {1, 0, 1});
    CHECK(editor.select_palette_material(palette, 1) == 1);
    CHECK(editor.selected_faces() == std::vector<uint8_t> {0, 1, 0});
}

TEST_CASE("vertex color material selection uses nearest palette color with stable ties", "[AI][VertexColorRegion]")
{
    indexed_triangle_set mesh = separated_triangle_mesh(2);
    std::vector<RGBA> colors {
        {0.8f, 0.1f, 0.1f, 1.0f}, {0.8f, 0.1f, 0.1f, 1.0f}, {0.8f, 0.1f, 0.1f, 1.0f},
        {0.5f, 0.5f, 0.5f, 1.0f}, {0.5f, 0.5f, 0.5f, 1.0f}, {0.5f, 0.5f, 0.5f, 1.0f}
    };
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(std::move(mesh), std::move(colors), error));
    const std::vector<RGBA> palette {
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 1.0f}
    };

    CHECK(editor.select_palette_material(palette, 0) == 2);
    const std::vector<uint8_t> snapshot = editor.selected_faces();
    CHECK(editor.select_palette_material(palette, 1) == 0);
    CHECK(editor.restore_selection(snapshot));
    CHECK(editor.selected_face_count() == 2);
}

TEST_CASE("vertex color material selection rejects invalid inputs without changing selection", "[AI][VertexColorRegion]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));
    const std::vector<RGBA> palette {{1.0f, 0.0f, 0.0f, 1.0f}};
    REQUIRE(editor.select_palette_material(palette, 0) == 2);
    const std::vector<uint8_t> snapshot = editor.selected_faces();

    CHECK(editor.select_palette_material({}, 0) == 2);
    CHECK(editor.selected_faces() == snapshot);
    CHECK(editor.select_palette_material(palette, 1) == 2);
    CHECK(editor.selected_faces() == snapshot);
}

TEST_CASE("vertex color overhang localization selects only significant elevated regions", "[AI][VertexColorRegion]")
{
    indexed_triangle_set mesh = overhang_region_mesh();
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f}), error));

    CHECK(editor.select_elevated_overhang_regions() == 2);
    CHECK(editor.selected_faces() == std::vector<uint8_t> {0, 0, 1, 1, 0, 0});
}

TEST_CASE("vertex color evidence selection ignores duplicate and invalid face indices", "[AI][VertexColorRegion]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));

    CHECK(editor.select_faces({1, 1, 99}) == 1);
    CHECK(editor.selected_faces() == std::vector<uint8_t> {0, 1});
}

TEST_CASE("vertex color evidence selection preserves an existing selection when evidence is empty", "[AI][VertexColorRegion]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));
    REQUIRE(editor.select_faces({0}) == 1);
    const std::vector<uint8_t> snapshot = editor.selected_faces();

    CHECK(editor.select_faces({2, 100}) == 0);
    CHECK(editor.selected_faces() == snapshot);
    CHECK(editor.select_faces({}) == 0);
    CHECK(editor.selected_faces() == snapshot);
}

TEST_CASE("vertex color overhang localization preserves selection when no region qualifies", "[AI][VertexColorRegion]")
{
    indexed_triangle_set mesh = overhang_region_mesh();
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f}), error));
    REQUIRE(editor.select_palette_material({{1.0f, 0.0f, 0.0f, 1.0f}}, 0) == mesh.indices.size());
    const std::vector<uint8_t> snapshot = editor.selected_faces();

    AI::OverhangRegionSettings settings;
    settings.ground_band_mm = 6.0f;
    CHECK(editor.select_elevated_overhang_regions(settings) == 0);
    CHECK(editor.selected_faces() == snapshot);

    settings.ground_band_mm = 0.5f;
    settings.minimum_region_area_mm2 = 101.0f;
    CHECK(editor.select_elevated_overhang_regions(settings) == 0);
    CHECK(editor.selected_faces() == snapshot);
}

TEST_CASE("vertex color smart region stops at sharp geometry boundaries", "[AI][VertexColorRegion]")
{
    indexed_triangle_set mesh;
    mesh.vertices = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {1.0f, 0.0f, 1.0f}
    };
    mesh.indices = {{0, 1, 2}, {1, 0, 3}};

    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(std::move(mesh), red_colors(), error));
    AI::RegionSelectionSettings settings;
    settings.normal_angle_degrees = 45.0f;
    CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 1);
}

TEST_CASE("vertex color picking returns the nearest visible face", "[AI][VertexColorRegion]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));

    const std::optional<size_t> hit = editor.pick_face({0.2, 0.2, 1.0}, {0.0, 0.0, -1.0});
    REQUIRE(hit);
    CHECK(*hit == 0);
    CHECK_FALSE(editor.pick_face({2.0, 2.0, 1.0}, {0.0, 0.0, -1.0}));
}

TEST_CASE("vertex color picking acceleration preserves nearest and deterministic hits", "[AI][VertexColorRegion]")
{
    indexed_triangle_set mesh = stacked_triangle_mesh(512);
    mesh.indices.push_back(mesh.indices.back());
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(mesh, solid_colors(mesh.vertices.size(), {1.0f, 0.0f, 0.0f, 1.0f}), error));

    const std::optional<size_t> hit = editor.pick_face({0.2, 0.2, 10.0}, {0.0, 0.0, -4.0});
    REQUIRE(hit);
    CHECK(*hit == 511);
    CHECK_FALSE(editor.pick_face({1.2, 1.2, 10.0}, {0.0, 0.0, -1.0}));
    CHECK_FALSE(editor.pick_face({0.2, 0.2, 10.0}, {0.0, 0.0, 0.0}));
}

TEST_CASE("vertex color OBJ copy preserves groups and isolates selected faces", "[AI][VertexColorRegion]")
{
    const boost::filesystem::path root =
        boost::filesystem::current_path() / "generated_models" / "test-local-recolor";
    boost::filesystem::create_directories(root);
    const boost::filesystem::path source = root / "source.obj";
    const boost::filesystem::path destination = root / "edited.obj";
    {
        boost::filesystem::ofstream stream(source, std::ios::trunc);
        stream << "o Body\n"
               << "v 0 0 0 1 0 0 1\n"
               << "v 1 0 0 1 0 0 1\n"
               << "v 0 1 0 1 0 0 1\n"
               << "v 1 1 0 0 0 1 1\n"
               << "g Surface\n"
               << "f 1 2 3\n"
               << "f 2 4 3\n";
    }

    AI::VertexColorRegionEditor editor;
    std::string error;
    std::vector<RGBA> colors = red_colors();
    colors[3] = {0.0f, 0.0f, 1.0f, 1.0f};
    REQUIRE(editor.initialize(square_mesh(), colors, error));
    AI::RegionSelectionSettings settings;
    settings.color_distance = 0.12f;
    REQUIRE(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 1);
    REQUIRE(editor.apply_color_to_obj_copy({0.0f, 1.0f, 0.0f, 1.0f}, source, destination, error));
    CHECK(editor.vertex_colors() == colors);
    CHECK(editor.selected_face_count() == 1);
    // A subsequent failed export must not contaminate the source editor either.
    CHECK_FALSE(editor.apply_color_to_obj_copy({1.0f, 1.0f, 0.0f, 1.0f}, source / "missing.obj", destination, error));
    CHECK(editor.vertex_colors() == colors);

    boost::filesystem::ifstream stream(destination);
    const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CHECK(contents.find("o Body") != std::string::npos);
    CHECK(contents.find("g Surface") != std::string::npos);
    CHECK(contents.find("f 1 2 3") != std::string::npos);
    TriangleMesh result;
    ObjInfo info;
    REQUIRE(load_obj(destination.string().c_str(), &result, info, error));
    REQUIRE(result.its.indices.size() == 2);
    for (size_t corner = 0; corner < 3; ++corner) {
        CHECK(info.vertex_colors[result.its.indices[0][corner]] == RGBA{0, 1, 0, 1});
        CHECK(info.vertex_colors[result.its.indices[1][corner]] == colors[square_mesh().indices[1][corner]]);
    }
}

TEST_CASE("Recoloring shared faces leaves neighboring corner colors and source topology intact", "[VertexColorRegion][Regression]")
{
    AI::VertexColorRegionEditor editor;
    std::string error;
    auto colors = red_colors();
    colors[3] = {0.1f, 0.2f, 0.3f, 1};
    const auto mesh = square_mesh();
    REQUIRE(editor.initialize(mesh, colors, error));
    REQUIRE(editor.select_faces({0}) == 1);
    const RGBA green {0, 1, 0, 1};
    const RGBA blue {0, 0, 1, 1};
    REQUIRE(editor.apply_color(green));
    REQUIRE(editor.has_color_overrides());
    CHECK(editor.mesh().indices == mesh.indices);
    CHECK(editor.mesh().vertices == mesh.vertices);
    CHECK(editor.vertex_colors() == colors);
    for (size_t corner = 0; corner < 3; ++corner) {
        CHECK(editor.corner_color(0, corner) == green);
        CHECK(editor.corner_color(1, corner) == colors[mesh.indices[1][corner]]);
    }
    REQUIRE(editor.select_palette_material({green, red_colors()[0]}, 0) == 1);
    CHECK(editor.selected_faces() == std::vector<uint8_t>{1, 0});
    REQUIRE(editor.select_faces({1}) == 1);
    REQUIRE(editor.apply_color(blue));
    for (size_t corner = 0; corner < 3; ++corner) {
        CHECK(editor.corner_color(0, corner) == green);
        CHECK(editor.corner_color(1, corner) == blue);
    }
    editor.clear();
    CHECK_FALSE(editor.has_color_overrides());
}

TEST_CASE("Face edits survive OBJ and GLB exports without bleeding across shared edges", "[VertexColorRegion][Regression]")
{
    const auto source_extension = GENERATE(std::string(".obj"), std::string(".glb"));
    const auto destination_extension = GENERATE(std::string(".obj"), std::string(".glb"));
    ScopedTemporaryFile source(source_extension);
    ScopedTemporaryFile destination(destination_extension);
    auto original_colors = red_colors();
    original_colors[3] = {0.2f, 0.3f, 0.4f, 1};
    std::string error;
    REQUIRE(AI::write_model_artifact(source.path(), square_mesh(), original_colors, error));
    TriangleMesh input;
    ObjInfo input_colors;
    REQUIRE(AI::load_model_artifact(source.path(), input, input_colors, error));
    AI::VertexColorRegionEditor editor;
    REQUIRE(editor.initialize(input.its, input_colors.vertex_colors, error));
    REQUIRE(editor.select_faces({0}) == 1);
    const RGBA blue {0, 0, 1, 1};
    REQUIRE(editor.apply_color_to_obj_copy(blue, source.path(), destination.path(), error));
    CHECK_FALSE(editor.has_color_overrides());
    TriangleMesh output;
    ObjInfo output_colors;
    REQUIRE(AI::load_model_artifact(destination.path(), output, output_colors, error));
    REQUIRE(output.its.indices.size() == input.its.indices.size());
    REQUIRE(output_colors.vertex_colors.size() == output.its.vertices.size());
    for (size_t face = 0; face < input.its.indices.size(); ++face) {
        for (size_t corner = 0; corner < 3; ++corner) {
            const int original = input.its.indices[face][corner];
            const int derived = output.its.indices[face][corner];
            const RGBA expected = face == 0 ? blue : input_colors.vertex_colors[original];
            for (size_t axis = 0; axis < 3; ++axis)
                CHECK_THAT(output.its.vertices[derived][axis], Catch::Matchers::WithinAbs(input.its.vertices[original][axis], 1e-6));
            for (size_t channel = 0; channel < 4; ++channel)
                CHECK_THAT(output_colors.vertex_colors[derived][channel], Catch::Matchers::WithinAbs(expected[channel], 1e-6));
        }
    }
}

TEST_CASE("Quad recoloring preserves per-corner UV and normal references with relative OBJ indices", "[VertexColorRegion][Regression]")
{
    ScopedTemporaryFile source(".obj");
    ScopedTemporaryFile destination(".obj");
    {
        boost::filesystem::ofstream stream(source.path());
        stream << "v 0 0 0 1 0 0\nv 1 0 0 1 0 0\nv 1 1 0 1 0 0\nv 0 1 0 1 0 0\n"
               << "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\ng Cloth\n"
               << "f -4/1/1 -3/2/1 -2/3/1 -1/4/1\n";
    }
    std::string error;
    TriangleMesh input;
    ObjInfo info;
    REQUIRE(load_obj(source.string().c_str(), &input, info, error));
    AI::VertexColorRegionEditor editor;
    REQUIRE(editor.initialize(input.its, info.vertex_colors, error));
    REQUIRE(editor.select_faces({1}) == 1);
    REQUIRE(editor.apply_color_to_obj_copy({0, 0, 1, 1}, source.path(), destination.path(), error));
    boost::filesystem::ifstream stream(destination.path());
    const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CHECK(contents.find("g Cloth") != std::string::npos);
    std::istringstream lines(contents);
    std::vector<std::string> corner_references;
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream tokens(line);
        std::string tag;
        tokens >> tag;
        if (tag != "f") continue;
        std::string token;
        while (tokens >> token) {
            REQUIRE(token.find('/') != std::string::npos);
            corner_references.push_back(token.substr(token.find('/')));
        }
    }
    CHECK(corner_references == std::vector<std::string>{"/1/1", "/2/1", "/3/1", "/1/1", "/3/1", "/4/1"});
    TriangleMesh result;
    ObjInfo result_colors;
    REQUIRE(load_obj(destination.string().c_str(), &result, result_colors, error));
    REQUIRE(result.its.indices.size() == 2);
    for (size_t corner = 0; corner < 3; ++corner) {
        CHECK(result_colors.vertex_colors[result.its.indices[0][corner]] == RGBA{1, 0, 0, 1});
        CHECK(result_colors.vertex_colors[result.its.indices[1][corner]] == RGBA{0, 0, 1, 1});
    }
}

TEST_CASE("A changed source face layout cannot overwrite an existing recolor result", "[VertexColorRegion][Regression]")
{
    ScopedTemporaryFile source(".obj");
    ScopedTemporaryFile destination(".obj");
    std::string error;
    REQUIRE(AI::write_model_artifact(source.path(), square_mesh(), red_colors(), error));
    AI::VertexColorRegionEditor editor;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));
    REQUIRE(editor.select_faces({0}) == 1);
    REQUIRE(editor.apply_color({0, 1, 0, 1}));
    {
        boost::filesystem::ofstream stream(source.path(), std::ios::app);
        stream << "f 1 2 3\n";
    }
    {
        boost::filesystem::ofstream stream(destination.path());
        stream << "previous result";
    }
    CHECK_FALSE(editor.apply_color_to_obj_copy({0, 0, 1, 1}, source.path(), destination.path(), error));
    CHECK(editor.has_color_overrides());
    CHECK(editor.corner_color(0, 0) == RGBA{0, 1, 0, 1});
    boost::filesystem::ifstream stream(destination.path());
    const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CHECK(contents == "previous result");
}

TEST_CASE("vertex color OBJ round trip preserves RGB channel order", "[AI][VertexColorRegion]")
{
    const boost::filesystem::path root =
        boost::filesystem::current_path() / "generated_models" / "test-local-recolor-rgb";
    boost::filesystem::create_directories(root);
    const boost::filesystem::path source = root / "source.obj";
    {
        boost::filesystem::ofstream stream(source, std::ios::trunc);
        stream << "v 0 0 0 1 1 1 1\n"
               << "v 1 0 0 1 1 1 1\n"
               << "v 0 1 0 1 1 1 1\n"
               << "v 1 1 0 1 1 1 1\n"
               << "f 1 2 3\n"
               << "f 2 4 3\n";
    }

    const std::array<std::pair<const char*, RGBA>, 3> cases {{
        {"red", {1.0f, 0.0f, 0.0f, 1.0f}},
        {"green", {0.0f, 1.0f, 0.0f, 1.0f}},
        {"blue", {0.0f, 0.0f, 1.0f, 1.0f}}
    }};
    for (const auto& [name, expected] : cases) {
        AI::VertexColorRegionEditor editor;
        std::string error;
        REQUIRE(editor.initialize(square_mesh(), red_colors(), error));
        AI::RegionSelectionSettings settings;
        settings.color_distance = 1.0f;
        REQUIRE(editor.update_selection(0, AI::RegionSelectionOperation::Replace, settings) == 2);

        const boost::filesystem::path destination = root / (std::string(name) + ".obj");
        REQUIRE(editor.apply_color_to_obj_copy(expected, source, destination, error));

        TriangleMesh mesh;
        ObjInfo obj_info;
        std::string message;
        REQUIRE(load_obj(destination.string().c_str(), &mesh, obj_info, message));
        REQUIRE(obj_info.vertex_colors.size() == 4);
        for (const RGBA& actual : obj_info.vertex_colors) {
            CHECK(actual[0] == Catch::Approx(expected[0]));
            CHECK(actual[1] == Catch::Approx(expected[1]));
            CHECK(actual[2] == Catch::Approx(expected[2]));
            CHECK(actual[3] == Catch::Approx(expected[3]));
        }
    }
}

TEST_CASE("uncolored OBJ loads safely without enabling local recoloring", "[AI][ModelPreview]")
{
    const boost::filesystem::path root =
        boost::filesystem::current_path() / "generated_models" / "test-uncolored-preview";
    boost::filesystem::create_directories(root);
    const boost::filesystem::path source = root / "uncolored.obj";
    {
        boost::filesystem::ofstream stream(source, std::ios::trunc);
        stream << "v 0 0 0\n"
               << "v 10 0 0\n"
               << "v 0 10 0\n"
               << "v 0 0 10\n"
               << "f 1 3 2\n"
               << "f 1 2 4\n"
               << "f 2 3 4\n"
               << "f 3 1 4\n";
    }

    TriangleMesh mesh;
    ObjInfo obj_info;
    std::string message;
    REQUIRE(load_obj(source.string().c_str(), &mesh, obj_info, message));
    CHECK(mesh.its.indices.size() == 4);
    CHECK(obj_info.vertex_colors.empty());

    AI::VertexColorRegionEditor editor;
    CHECK_FALSE(editor.initialize(std::move(mesh.its), std::move(obj_info.vertex_colors), message));
    CHECK_FALSE(editor.ready());
    CHECK(message == "Local recoloring requires OBJ vertex colors.");
}


TEST_CASE("Large geometric seam groups preserve paired and nonmanifold boundaries", "[VertexColorRegion][Regression]")
{
    const bool reversed_groups = GENERATE(false, true);
    indexed_triangle_set mesh;
    std::vector<std::vector<uint32_t>> expected;
    constexpr int groups = 512;
    const auto add_face = [&](const Vec3f& a, const Vec3f& b, const Vec3f& c) {
        const int first = int(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(), {a, b, c});
        mesh.indices.emplace_back(first, first + 1, first + 2);
        expected.emplace_back();
    };
    for (int group = 0; group < groups; ++group) {
        const float x = float(reversed_groups ? groups - 1 - group : group) * 16.0f;
        const uint32_t first = uint32_t(mesh.indices.size());
        add_face({x, 0, 0}, {x + 1, 0, 0}, {x + 1, 1, 0});
        add_face({x, 0, 0}, {x + 1, 1, 0}, {x, 1, 0});
        expected[first] = {first + 1};
        expected[first + 1] = {first};
        for (int repeated = 0; repeated < 3; ++repeated)
            add_face({x + 4, 0, 0}, {x + 5, 0, 0}, {x + 4, 1, 0});
        add_face({x + 8, 0, 0}, {x + 8, 0, 0}, {x + 8, 0, 0});
    }
    const auto colors = solid_colors(mesh.vertices.size(), {1, 0, 0, 1});
    AI::VertexColorRegionEditor editor;
    std::string error;
    REQUIRE(editor.initialize(mesh, colors, error));
    CHECK(editor.mesh().vertices == mesh.vertices);
    CHECK(editor.mesh().indices == mesh.indices);
    CHECK(editor.vertex_colors() == colors);
    REQUIRE(editor.*region_probe_member(RegionNeighbors{}) == expected);
    AI::RegionSelectionSettings settings;
    for (int group = 0; group < groups; ++group) {
        INFO(group);
        const uint32_t first = uint32_t(group * 6);
        CHECK(editor.update_selection(first, AI::RegionSelectionOperation::Replace, settings) == 2);
        CHECK(editor.update_selection(first + 2, AI::RegionSelectionOperation::Replace, settings) == 1);
        const float x = float(reversed_groups ? groups - 1 - group : group) * 16.0f;
        const auto hit = editor.pick_face({double(x) + 0.75, 0.25, 1.0}, {0, 0, -1});
        REQUIRE(hit.has_value());
        CHECK(*hit == first);
    }
}


namespace {
indexed_triangle_set cancellation_grid(size_t side) {
    indexed_triangle_set mesh;
    for (size_t row = 0; row <= side; ++row)
        for (size_t column = 0; column <= side; ++column)
            mesh.vertices.emplace_back(float(column), float(row), 0.f);
    for (size_t row = 0; row < side; ++row) {
        for (size_t column = 0; column < side; ++column) {
            const int a = int(row * (side + 1) + column), b = a + 1;
            const int c = a + int(side + 1), d = c + 1;
            mesh.indices.emplace_back(a, b, c);mesh.indices.emplace_back(b, d, c);
        }
    }
    return mesh;
}
}

TEST_CASE("Abandoned selection preparation clears partial state and can be retried", "[AI][VertexColorRegion]") {
    const bool independent = GENERATE(false, true);
    const unsigned fraction = GENERATE(0u, 1u, 25u, 50u, 99u, 100u);
    const auto mesh = independent ? separated_triangle_mesh(4096) : cancellation_grid(48);
    const auto colors = solid_colors(mesh.vertices.size(), {1.f, 0.f, 0.f, 1.f});
    const auto original_vertices = mesh.vertices;const auto original_faces = mesh.indices;
    AI::VertexColorRegionEditor reference;std::string error;size_t total = 0;
    REQUIRE(reference.initialize(mesh, colors, error, [&] { ++total;return false; }));
    REQUIRE(total > 4);
    const size_t threshold = std::max(size_t(1), total * fraction / 100);
    AI::VertexColorRegionEditor editor;
    REQUIRE(editor.initialize(square_mesh(), red_colors(), error));
    REQUIRE(editor.select_faces({0}) == 1);REQUIRE(editor.apply_color({0.f, 0.f, 1.f, 1.f}));
    size_t checkpoints = 0;
    CHECK_FALSE(editor.initialize(mesh, colors, error, [&] { return ++checkpoints >= threshold; }));
    CHECK(error == "Local selection preparation canceled.");
    CHECK_FALSE(editor.ready());CHECK(editor.mesh().vertices.empty());CHECK(editor.mesh().indices.empty());
    CHECK(editor.vertex_colors().empty());CHECK(editor.selected_faces().empty());CHECK(editor.selected_face_count() == 0);
    CHECK_FALSE(editor.has_color_overrides());
    CHECK((editor.*region_probe_member(RegionNeighbors{})).empty());
    CHECK((editor.*region_probe_member(RegionPickFaceOrder{})).empty());
    CHECK_FALSE(editor.pick_face({.25, .25, 1.}, {0., 0., -1.}));
    CHECK(mesh.vertices == original_vertices);CHECK(mesh.indices == original_faces);
    error.clear();REQUIRE(editor.initialize(mesh, colors, error));
    CHECK(editor.mesh().vertices == original_vertices);CHECK(editor.mesh().indices == original_faces);
    CHECK(editor.vertex_colors() == colors);
    CHECK((editor.*region_probe_member(RegionNeighbors{})) == (reference.*region_probe_member(RegionNeighbors{})));
    CHECK((editor.*region_probe_member(RegionPickFaceOrder{})) == (reference.*region_probe_member(RegionPickFaceOrder{})));
    CHECK(editor.pick_face({.25, .25, 1.}, {0., 0., -1.}) == reference.pick_face({.25, .25, 1.}, {0., 0., -1.}));
}

TEST_CASE("A failing selection cancellation callback leaves no selectable partial editor", "[AI][VertexColorRegion]") {
    const auto mesh = cancellation_grid(32);const auto colors = solid_colors(mesh.vertices.size(), {1.f, 0.f, 0.f, 1.f});
    AI::VertexColorRegionEditor editor;std::string error;size_t checkpoints = 0;
    CHECK_THROWS_WITH(editor.initialize(mesh, colors, error, [&]() -> bool {
        if (++checkpoints == 4) throw std::runtime_error("Interrupted callback.");
        return false;
    }), "Interrupted callback.");
    CHECK_FALSE(editor.ready());CHECK(editor.mesh().indices.empty());CHECK(editor.vertex_colors().empty());
    REQUIRE(editor.initialize(mesh, colors, error));
    REQUIRE(editor.pick_face({.25, .25, 1.}, {0., 0., -1.}));
}

TEST_CASE("Historical selection cancellation reports avoided initialization and cleanup", "[.][RegionPreparationCancelProbe]") {
    const char* source = std::getenv("ORCA_SELECTION_SOURCE");
    const char* output = std::getenv("ORCA_SELECTION_OUTPUT");
    REQUIRE(source != nullptr);REQUIRE(output != nullptr);REQUIRE_FALSE(boost::filesystem::exists(output));
    const auto source_hash = AI::model_artifact_sha256(source);
    TriangleMesh mesh;ObjInfo colors;std::string error;
    REQUIRE(AI::load_model_artifact(source, mesh, colors, error));
    nlohmann::json samples = nlohmann::json::array();
    std::vector<int> delays {50, 250};
    if (const char* configured = std::getenv("ORCA_SELECTION_CANCEL_DELAYS_MS")) {
        delays.clear();
        std::istringstream stream(configured);
        std::string token;
        while (std::getline(stream, token, ',')) {
            const int delay = std::stoi(token);
            REQUIRE(delay > 0);
            REQUIRE(delay <= 10000);
            delays.push_back(delay);
        }
        REQUIRE(delays.size() == 2);
    }
    for (int round = 0; round < 2; ++round) {
        for (const bool cooperative : {false, true}) {
            for (const int delay_ms : delays) {
                AI::VertexColorRegionEditor editor;
                std::atomic<bool> canceled {false};
                const auto started = std::chrono::steady_clock::now();
                const auto due = started + std::chrono::milliseconds(delay_ms);
                std::chrono::steady_clock::time_point requested;
                auto request = std::async(std::launch::async, [&] {
                    std::this_thread::sleep_until(due);requested = std::chrono::steady_clock::now();canceled = true;
                });
                const bool success = cooperative
                    ? editor.initialize(mesh.its, colors.vertex_colors, error, [&] { return canceled.load(); })
                    : editor.initialize(mesh.its, colors.vertex_colors, error);
                const auto finished = std::chrono::steady_clock::now();request.get();
                const bool requested_while_running = requested < finished;
                // A request after the last checkpoint may race a completed result.
                // Task publication also checks cancellation; record that tail separately.
                if (cooperative && !success) {
                    REQUIRE_FALSE(success);CHECK(error == "Local selection preparation canceled.");
                    CHECK_FALSE(editor.ready());CHECK(editor.mesh().vertices.empty());CHECK(editor.mesh().indices.empty());
                    CHECK(editor.vertex_colors().empty());CHECK(editor.selected_faces().empty());
                    CHECK((editor.*region_probe_member(RegionNeighbors{})).empty());
                    CHECK((editor.*region_probe_member(RegionPickFaceOrder{})).empty());
                } else {
                    REQUIRE(success);CHECK(editor.mesh().vertices == mesh.its.vertices);CHECK(editor.mesh().indices == mesh.its.indices);
                    CHECK(editor.vertex_colors() == colors.vertex_colors);
                }
                samples.push_back({{"round", round}, {"cooperative", cooperative}, {"delay_ms", delay_ms},
                    {"requested_while_running", requested_while_running}, {"success", success},
                    {"initialize_ms", std::chrono::duration<double, std::milli>(finished - started).count()},
                    {"request_to_return_ms", requested_while_running
                        ? nlohmann::json(std::chrono::duration<double, std::milli>(finished - requested).count()) : nlohmann::json(nullptr)}});
            }
        }
    }
    REQUIRE(AI::model_artifact_sha256(source) == source_hash);
    boost::filesystem::ofstream report(output);
    report << nlohmann::json{{"source_sha256", source_hash}, {"faces", mesh.its.indices.size()}, {"samples", samples},
        {"scope", "Same executable, legacy/cooperative initialization with actual background atomic requests. Includes argument copies and cancellation cleanup; source load/hash excluded. No GUI, natural switch/exit, upper-bound or normal performance claim."}}.dump(2);
    report.close();REQUIRE(bool(report));
}

TEST_CASE("Picking can prepare legacy regions without changing selection or source colors", "[VertexColorRegion]") {
    const unsigned shape = GENERATE(0u, 1u, 2u, 3u, 4u, 5u, 6u);
    const auto mesh = shape == 0 ? square_mesh() : shape == 1 ? seamed_square_mesh() :
        shape == 2 ? seamed_square_mesh(1e-7f) : shape == 3 ? seamed_square_mesh(.01f) :
        shape == 4 ? separated_triangle_mesh(4096) : shape == 5 ? cancellation_grid(48) : overhang_region_mesh();
    const auto colors = solid_colors(mesh.vertices.size(), {1, 0, 0, 1});
    AI::VertexColorRegionEditor eager, editor;std::string error;
    REQUIRE(eager.initialize(mesh, colors, error));
    REQUIRE(editor.initialize_for_picking(mesh, colors, error));
    REQUIRE(editor.ready());REQUIRE_FALSE(editor.region_selection_ready());
    CHECK(editor.mesh().vertices == mesh.vertices);CHECK(editor.mesh().indices == mesh.indices);CHECK(editor.vertex_colors() == colors);
    CHECK((editor.*region_probe_member(RegionNeighbors{})).empty());
    CHECK((editor.*region_probe_member(RegionPickFaceOrder{})) == (eager.*region_probe_member(RegionPickFaceOrder{})));
    REQUIRE(editor.select_faces({0}) == 1);REQUIRE(editor.apply_color({0, 1, 0, 1}));
    REQUIRE(eager.select_faces({0}) == 1);REQUIRE(eager.apply_color({0, 1, 0, 1}));
    const auto selected = editor.selected_faces();
    const auto hit = editor.pick_face({.25, .25, 1}, {0, 0, -1});
    CHECK(hit == eager.pick_face({.25, .25, 1}, {0, 0, -1}));
    CHECK(editor.update_selection(0, AI::RegionSelectionOperation::Replace, {}) == 1);
    CHECK(editor.selected_faces() == selected);CHECK(editor.select_elevated_overhang_regions() == 0);
    auto topology = editor.prepare_region_topology(error);REQUIRE(topology);
    REQUIRE(editor.install_region_topology(std::move(topology)));REQUIRE(editor.region_selection_ready());
    CHECK(editor.selected_faces() == selected);CHECK(editor.corner_color(0, 0) == RGBA{0, 1, 0, 1});
    CHECK(editor.vertex_colors() == colors);CHECK(editor.pick_face({.25, .25, 1}, {0, 0, -1}) == hit);
    CHECK((editor.*region_probe_member(RegionNeighbors{})) == (eager.*region_probe_member(RegionNeighbors{})));
    for (const auto operation : {AI::RegionSelectionOperation::Replace, AI::RegionSelectionOperation::Add,
                                AI::RegionSelectionOperation::Remove, AI::RegionSelectionOperation::AddSimilar}) {
        CHECK(editor.update_selection(1, operation, {}) == eager.update_selection(1, operation, {}));
        CHECK(editor.selected_faces() == eager.selected_faces());
    }
    CHECK(editor.select_elevated_overhang_regions() == eager.select_elevated_overhang_regions());
    CHECK(editor.selected_faces() == eager.selected_faces());
}

TEST_CASE("Canceled region upgrades preserve the ready picker and can be retried", "[VertexColorRegion]") {
    const unsigned fraction = GENERATE(0u, 25u, 50u, 99u, 100u);
    const auto mesh = cancellation_grid(48);const auto colors = solid_colors(mesh.vertices.size(), {1, 0, 0, 1});
    AI::VertexColorRegionEditor editor;std::string error;
    REQUIRE(editor.initialize_for_picking(mesh, colors, error));
    REQUIRE(editor.select_faces({0}) == 1);REQUIRE(editor.apply_color({0, 1, 0, 1}));
    const auto mask = editor.selected_faces();const auto hit = editor.pick_face({.25, .25, 1}, {0, 0, -1});
    size_t count = 0;auto reference = editor.prepare_region_topology(error, [&] { ++count;return false; });
    REQUIRE(reference);REQUIRE(count > 4);
    const size_t threshold = std::max(size_t(1), count * fraction / 100);size_t at = 0;
    CHECK_FALSE(editor.prepare_region_topology(error, [&] { return ++at >= threshold; }));
    CHECK(error == "Local selection preparation canceled.");
    REQUIRE(editor.ready());CHECK_FALSE(editor.region_selection_ready());CHECK(editor.selected_faces() == mask);
    CHECK(editor.corner_color(0, 0) == RGBA{0, 1, 0, 1});CHECK(editor.vertex_colors() == colors);
    CHECK(editor.pick_face({.25, .25, 1}, {0, 0, -1}) == hit);
    REQUIRE(editor.install_region_topology(std::move(reference)));CHECK(editor.region_selection_ready());
    CHECK(editor.selected_faces() == mask);
}

TEST_CASE("Region topology cannot be installed on another or reinitialized editor", "[VertexColorRegion]") {
    AI::VertexColorRegionEditor first, other;std::string error;
    REQUIRE(first.initialize_for_picking(square_mesh(), red_colors(), error));
    REQUIRE(other.initialize_for_picking(square_mesh(), red_colors(), error));
    auto wrong = first.prepare_region_topology(error);REQUIRE(wrong);
    CHECK_FALSE(other.install_region_topology(std::move(wrong)));CHECK_FALSE(other.region_selection_ready());
    auto stale = first.prepare_region_topology(error);REQUIRE(stale);
    REQUIRE(first.initialize_for_picking(square_mesh(), red_colors(), error));
    REQUIRE(first.select_faces({1}) == 1);
    CHECK_FALSE(first.install_region_topology(std::move(stale)));CHECK_FALSE(first.region_selection_ready());
    CHECK(first.selected_faces() == std::vector<uint8_t>{0, 1});
    auto fresh = first.prepare_region_topology(error);REQUIRE(fresh);
    REQUIRE(first.install_region_topology(std::move(fresh)));CHECK(first.selected_face_count() == 1);
    CHECK_FALSE(first.install_region_topology({}));
}

TEST_CASE("A topology callback failure preserves existing edits and geometry", "[VertexColorRegion]") {
    const auto mesh = cancellation_grid(32);const auto colors = solid_colors(mesh.vertices.size(), {1, 0, 0, 1});
    AI::VertexColorRegionEditor editor;std::string error;size_t at = 0;
    REQUIRE(editor.initialize_for_picking(mesh, colors, error));REQUIRE(editor.select_faces({0}) == 1);
    REQUIRE(editor.apply_color({0, 1, 0, 1}));
    CHECK_THROWS_WITH(editor.prepare_region_topology(error, [&]() -> bool {
        if (++at == 4) throw std::runtime_error("Interrupted upgrade.");return false;
    }), "Interrupted upgrade.");
    CHECK(editor.ready());CHECK_FALSE(editor.region_selection_ready());CHECK(editor.selected_face_count() == 1);
    CHECK(editor.mesh().vertices == mesh.vertices);CHECK(editor.mesh().indices == mesh.indices);CHECK(editor.vertex_colors() == colors);
    CHECK(editor.corner_color(0, 0) == RGBA{0, 1, 0, 1});
    auto topology = editor.prepare_region_topology(error);REQUIRE(topology);REQUIRE(editor.install_region_topology(std::move(topology)));
}

TEST_CASE("Edits made during background topology preparation survive installation and OBJ export", "[VertexColorRegion]") {
    const auto mesh = square_mesh();const auto colors = red_colors();
    AI::VertexColorRegionEditor editor, eager;std::string error;
    REQUIRE(editor.initialize_for_picking(mesh, colors, error));REQUIRE(eager.initialize(mesh, colors, error));
    std::atomic<bool> reading {false}, resume {false};std::string worker_error;
    auto task = std::async(std::launch::async, [&] {
        return editor.prepare_region_topology(worker_error, [&] {
            reading = true;while (!resume.load()) std::this_thread::yield();return false;
        });
    });
    while (!reading.load()) std::this_thread::yield();
    // No Catch assertion before release: an assertion exception cannot deadlock the worker.
    const auto selected = editor.select_faces({1});const bool painted = editor.apply_color({0, 1, 0, 1});
    resume = true;auto topology = task.get();
    REQUIRE(selected == 1);REQUIRE(painted);REQUIRE(topology);
    REQUIRE(editor.install_region_topology(std::move(topology)));CHECK(editor.selected_faces() == std::vector<uint8_t>{0, 1});
    REQUIRE(eager.select_faces({1}) == 1);REQUIRE(eager.apply_color({0, 1, 0, 1}));
    ScopedTemporaryFile source(".obj"), output(".obj"), reference(".obj");
    {boost::filesystem::ofstream stream(source.path());
        stream << "o Body\nv 0 0 0 1 0 0 1\nv 1 0 0 1 0 0 1\nv 0 1 0 1 0 0 1\nv 1 1 0 1 0 0 1\nf 1 2 3\nf 2 4 3\n";}
    const auto hash = AI::model_artifact_sha256(source.path());
    REQUIRE(editor.write_obj_copy(source.path(), output.path(), error));
    REQUIRE(eager.write_obj_copy(source.path(), reference.path(), error));
    CHECK(AI::model_artifact_sha256(output.path()) == AI::model_artifact_sha256(reference.path()));
    CHECK(AI::model_artifact_sha256(source.path()) == hash);
}

TEST_CASE("Canceled picker preparation exposes no partial geometry and retries successfully", "[VertexColorRegion]") {
    const unsigned fraction = GENERATE(0u, 25u, 50u, 99u, 100u);
    const auto mesh = separated_triangle_mesh(4096);const auto colors = solid_colors(mesh.vertices.size(), {1, 0, 0, 1});
    AI::VertexColorRegionEditor editor;std::string error;size_t count = 0;
    REQUIRE(editor.initialize_for_picking(mesh, colors, error, [&] { ++count;return false; }));
    REQUIRE(count > 4);const size_t threshold = std::max(size_t(1), count * fraction / 100);size_t at = 0;
    CHECK_FALSE(editor.initialize_for_picking(mesh, colors, error, [&] { return ++at >= threshold; }));
    CHECK_FALSE(editor.ready());CHECK(editor.mesh().vertices.empty());CHECK(editor.mesh().indices.empty());
    CHECK(editor.vertex_colors().empty());CHECK(editor.selected_faces().empty());
    REQUIRE(editor.initialize_for_picking(mesh, colors, error));CHECK(editor.ready());CHECK_FALSE(editor.region_selection_ready());
}
