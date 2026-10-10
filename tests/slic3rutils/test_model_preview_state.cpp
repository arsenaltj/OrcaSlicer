#include "slic3r/GUI/AI/ModelGeneration/ModelPreview3D.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationPresentation.hpp"
#include "../test_utils.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <cstring>
#include <future>
#include <string_view>

using Slic3r::GUI::ModelPreview3D;
using Slic3r::Vec3d;
namespace selection = Slic3r::AI::SurfaceSelectionPersistence;
namespace trial = Slic3r::AI::ColorTrialPersistence;

TEST_CASE("Texture upload rejects an uninitialized OpenGL loader without calling it", "[ModelPreviewState]")
{
    // A cold start has a canvas/context before GLAD has loaded its entry points.
    // Keep this independent of other tests that may have initialized OpenGL.
    struct RestoreEntryPoint {
        PFNGLGETINTEGERVPROC saved {glad_glGetIntegerv};
        ~RestoreEntryPoint() { glad_glGetIntegerv = saved; }
    } restore;
    glad_glGetIntegerv = nullptr;
    Slic3r::AI::ModelArtifactTextureSurface surface;
    surface.faces.resize(1);
    indexed_triangle_set mesh;
    mesh.vertices = {{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}};
    mesh.indices.emplace_back(0, 1, 2);
    const std::vector<Slic3r::Vec3f> normals(3, Slic3r::Vec3f(0.f, 0.f, 1.f));
    REQUIRE_THROWS_WITH(Slic3r::GUI::ModelPreviewTexture(surface, mesh, normals),
                        "OpenGL texture preview is not initialized.");
}

namespace {
struct SavedPreview {
    ScopedTemporaryDir directory {"orca-preview-state"};
    boost::filesystem::path model = directory.path() / "models" / "saved.glb";
    boost::filesystem::path record = directory.path() / "downloads" / "saved.json";
    boost::filesystem::path adjacent = directory.path() / "models" / "saved.json";
    ModelPreview3D::PreparedModel original;
    selection::SelectionState masks;
    selection::FaceColorOverrides intent {{0, {0.2f, 0.7f, 0.1f}}};
    trial::State colors;

    SavedPreview()
    {
        boost::filesystem::create_directories(model.parent_path());
        boost::filesystem::create_directories(record.parent_path());
        boost::filesystem::copy_file(boost::filesystem::path(std::string(TEST_DATA_DIR)) /
            "model_artifact" / "baseline.glb", model);
        std::string error;
        REQUIRE(ModelPreview3D::prepare_model(model, original, error));
        REQUIRE(original.triangles >= 2);
        masks.selected.assign(original.triangles, 0);
        masks.protected_faces.assign(original.triangles, 0);
        masks.foreground.assign(original.triangles, 0);
        masks.domain.assign(original.triangles, 0);
        masks.selected[0] = masks.foreground[0] = masks.domain[0] = 1;
        masks.protected_faces[1] = 1;
        colors.mapping_colors = {{0.1f, 0.2f, 0.3f}};
        colors.colors = {{0.8f, 0.6f, 0.4f}};
        colors.source = 2;
        colors.count = 1;
        colors.enabled = true;
        colors.locks[0] = true;
    }

    nlohmann::json metadata(const std::string& geometry) const
    {
        return {
            {"local_selection", selection::encode(masks, original.triangles, geometry)},
            {"face_color_intent", selection::encode_colors(intent, original.triangles, geometry)},
            {"color_trial", trial::encode(colors, original.triangles, geometry)}
        };
    }

    void write(const boost::filesystem::path& path, const nlohmann::json& value) const
    {
        boost::filesystem::ofstream output(path);
        REQUIRE(output.good());
        output << value.dump();
        output.close();
        REQUIRE(output.good());
    }

    void check_restored(const ModelPreview3D::PreparedModel& prepared) const
    {
        REQUIRE(prepared.selection.has_value());
        CHECK(prepared.selection->selected == masks.selected);
        CHECK(prepared.selection->protected_faces == masks.protected_faces);
        CHECK(prepared.selection->foreground == masks.foreground);
        CHECK(prepared.selection->domain == masks.domain);
        CHECK(prepared.face_color_overrides == intent);
        REQUIRE(prepared.color_trial.has_value());
        CHECK(prepared.color_trial->mapping_colors == colors.mapping_colors);
        CHECK(prepared.color_trial->colors == colors.colors);
        CHECK(prepared.color_trial->locks == colors.locks);
        CHECK(prepared.color_trial->enabled == colors.enabled);
        CHECK(prepared.color_trial->source == 2);
    }
};
}

TEST_CASE("History preview restores editing state stored outside the model directory", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.record, saved.metadata(saved.original.geometry_id));
    REQUIRE_FALSE(boost::filesystem::exists(saved.adjacent));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    saved.check_restored(prepared);
}

TEST_CASE("Repeated preview RGB keeps each vertex alpha and local face overrides", "[ModelPreviewState]")
{
    ScopedTemporaryDir directory {"orca-preview-rgb-alpha"};
    const auto path = directory.path() / "colors.obj";
    {
        boost::filesystem::ofstream output(path);
        output << "v 0 0 0 1 0 0 .25\nv 1 0 0 1 0 0 .5\nv 0 1 0 1 0 0 .75\n"
               << "v 2 0 0 0 1 0 .2\nv 3 0 0 0 1 0 .4\nv 2 1 0 0 1 0 .8\n"
               << "v 4 0 0 1 0 0 .1\nv 5 0 0 1 0 0 .3\nv 4 1 0 1 0 0 .6\n"
               << "f 1 2 3\nf 4 5 6\nf 7 8 9\n";
        REQUIRE(output.good());
    }
    for (bool override_face : {false, true}) {
        ModelPreview3D::PreparedModel prepared;
        std::string error;
        ModelPreview3D::FaceColorOverrides overrides;
        if (override_face) overrides = {{0, {0.f, 0.f, 1.f}}};
        REQUIRE(ModelPreview3D::prepare_model(path, prepared, error, overrides));
        REQUIRE(prepared.geometry.vertices.size() == 9 * 8);
        REQUIRE(prepared.vertex_colors.size() == 9);
        CHECK(prepared.colors == 2);
        bool colors_match = true, alpha_matches = true;
        for (size_t vertex = 0; vertex < 9; ++vertex) {
            const uint32_t expected = override_face && vertex < 3 ? 255u
                : vertex >= 3 && vertex < 6 ? 65280u : 16711680u;
            const float expected_rgb = float(expected);
            const float expected_alpha = override_face && vertex < 3 ? -1.f : prepared.vertex_colors[vertex][3];
            // Packed shader attributes must retain their exact bytes, including alpha.
            colors_match &= std::memcmp(&prepared.geometry.vertices[vertex * 8 + 6], &expected_rgb, sizeof(float)) == 0;
            alpha_matches &= std::memcmp(&prepared.geometry.vertices[vertex * 8 + 7], &expected_alpha, sizeof(float)) == 0;
        }
        CHECK(colors_match);
        CHECK(alpha_matches);
    }
}

TEST_CASE("The chosen history record takes precedence over adjacent preview state", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.record, saved.metadata(saved.original.geometry_id));
    saved.write(saved.adjacent, nlohmann::json::object());
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    saved.check_restored(prepared);
}

TEST_CASE("Saved preview state is rejected when geometry differs despite matching face counts", "[ModelPreviewState]")
{
    SavedPreview saved;
    const std::string other_geometry(64, '0');
    REQUIRE(other_geometry != saved.original.geometry_id);
    saved.write(saved.record, saved.metadata(other_geometry));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    CHECK_FALSE(prepared.selection.has_value());
    CHECK_FALSE(prepared.color_trial.has_value());
    CHECK(prepared.face_color_overrides.empty());
    CHECK(prepared.triangles == saved.original.triangles);
}

TEST_CASE("Preview state beside a model remains available without an explicit history record", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.adjacent, saved.metadata(saved.original.geometry_id));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error));
    saved.check_restored(prepared);
}

TEST_CASE("A missing chosen history record does not borrow another adjacent snapshot", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.adjacent, saved.metadata(saved.original.geometry_id));
    REQUIRE_FALSE(boost::filesystem::exists(saved.record));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    CHECK_FALSE(prepared.selection.has_value());
    CHECK_FALSE(prepared.color_trial.has_value());
    CHECK(prepared.face_color_overrides.empty());
}

TEST_CASE("original color matching ignores saved trial edits and preserves native mesh connectivity", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.adjacent, saved.metadata(saved.original.geometry_id));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, {}, false));
    CHECK_FALSE(prepared.selection.has_value());
    CHECK_FALSE(prepared.color_trial.has_value());
    CHECK(prepared.face_color_overrides.empty());
    CHECK(prepared.geometry.vertices == saved.original.geometry.vertices);
    CHECK(prepared.mesh.vertices == saved.original.mesh.vertices);
    CHECK(prepared.mesh.indices == saved.original.mesh.indices);
}

TEST_CASE("original preparation retains indexed geometry even without vertex colors", "[ModelPreviewState]")
{
    ScopedTemporaryDir directory {"orca-color-native-mesh"};
    const auto path = directory.path() / "plain.obj";
    {
        boost::filesystem::ofstream file(path);
        file << "v 0 0 0\nv 20 0 0\nv 0 20 0\nv 0 0 20\n"
                "f 1 3 2\nf 1 2 4\nf 1 4 3\nf 2 3 4\n";
    }
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(path, prepared, error, {}, {}, false));
    CHECK(prepared.mesh.vertices.size() == 4);
    CHECK(prepared.mesh.indices.size() == 4);
    CHECK(prepared.geometry.vertices_count() == 12);
    CHECK(selection::geometry_fingerprint(prepared.mesh) == prepared.geometry_id);
}

TEST_CASE("Canceled preview preparation never reads a model or retains an earlier result", "[ModelPreviewState]")
{
    SavedPreview saved;
    auto prepared=std::move(saved.original);
    prepared.prepare_render_geometry();
    REQUIRE(prepared.render_geometry.has_value());
    std::string error;
    REQUIRE_FALSE(ModelPreview3D::prepare_model(saved.directory.path()/"missing.glb",prepared,error,
        {},{},true,[]{return true;}));
    CHECK(error=="Model preview loading canceled.");
    CHECK_FALSE(prepared.render_geometry.has_value());
    CHECK(prepared.geometry.is_empty());
    CHECK(prepared.mesh.indices.empty());
    CHECK(prepared.path.empty());
    CHECK(prepared.geometry_id.empty());
    CHECK(prepared.triangles==0);
    CHECK_FALSE(prepared.semantic_source);
}

TEST_CASE("Interrupted preview preparation discards partial state and can restore a fresh task", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.record,saved.metadata(saved.original.geometry_id));
    for(const int checkpoint:{2,4,8}) {
        INFO("Cancel checkpoint "<<checkpoint);
        int visits=0;
        ModelPreview3D::PreparedModel prepared;
        std::string error;
        REQUIRE_FALSE(ModelPreview3D::prepare_model(saved.model,prepared,error,{},saved.record,true,
            [&]{return ++visits>=checkpoint;}));
        CHECK(error=="Model preview loading canceled.");
        CHECK(prepared.geometry.is_empty());
        CHECK(prepared.mesh.indices.empty());
        CHECK(prepared.geometry_id.empty());
        CHECK_FALSE(prepared.semantic_source);
        CHECK_FALSE(prepared.trial_histogram);
        CHECK_FALSE(prepared.selection);
        CHECK_FALSE(prepared.color_trial);
        CHECK(prepared.face_color_overrides.empty());
        CHECK(prepared.triangles==0);
        REQUIRE(ModelPreview3D::prepare_model(saved.model,prepared,error,{},saved.record,true,[]{return false;}));
        CHECK(error.empty());
        saved.check_restored(prepared);
        REQUIRE(prepared.semantic_source);
        CHECK(prepared.semantic_source->content_id==saved.original.semantic_source->content_id);
        CHECK(prepared.mesh.indices==saved.original.mesh.indices);
        CHECK(prepared.geometry.indices==saved.original.geometry.indices);
    }
}

TEST_CASE("A worker observes an externally canceled preview before publishing its CPU result", "[ModelPreviewState]")
{
    SavedPreview saved;
    std::atomic<bool> canceled{false};
    std::promise<void> reached,release;
    auto arrived=reached.get_future();auto released=release.get_future();
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    auto worker=std::async(std::launch::async,[&]{
        int visits=0;
        return ModelPreview3D::prepare_model(saved.model,prepared,error,{}, {},true,[&]{
            if(++visits==2){reached.set_value();released.wait();}
            return canceled.load();
        });
    });
    const auto status=arrived.wait_for(std::chrono::seconds(5));
    canceled=true;release.set_value();
    const bool completed=worker.get();
    REQUIRE(status==std::future_status::ready);
    CHECK_FALSE(completed);
    CHECK(error=="Model preview loading canceled.");
    CHECK(prepared.geometry.is_empty());
    CHECK(prepared.triangles==0);
    CHECK_FALSE(prepared.semantic_source);
}

// Opt-in CPU measurement using an existing local model. Measures preparation
// and cancellation checkpoints; it never creates a window or uploads to GL.
TEST_CASE("Historical preview cancellation measures avoided preparation work", "[.ModelPreviewCancelProbe]")
{
    struct RestoreLogging {
        unsigned level {Slic3r::get_logging_level()};
        ~RestoreLogging() { Slic3r::set_logging_level(level); }
    } restore_logging;
    const auto env=[](const char* key){const auto value=boost::nowide::getenv(key);return value?std::string(value):std::string{};};
    if (env("ORCASLICER_ASSIMP_TIMING") == "1" || env("ORCASLICER_MODEL_PREPARE_TIMING") == "1")
        Slic3r::set_logging_level(3);
    const auto source=env("ORCA_PREVIEW_SOURCE"),report=env("ORCA_PREVIEW_REPORT");
    if(source.empty() || report.empty())SKIP("Set a historical model and a fresh CPU report path.");
    REQUIRE_FALSE(boost::filesystem::exists(report));
    const auto source_hash=Slic3r::AI::model_artifact_sha256(source);
    REQUIRE_FALSE(source_hash.empty());
    nlohmann::json samples=nlohmann::json::array();
    std::string geometry_id,content_id;
    uint64_t render_digest=0;
    for(int round=0;round<4;++round)for(int index=0;index<3;++index) {
        const int mode=(index+round)%3;
        ModelPreview3D::PreparedModel prepared;
        std::string error;
        const auto start=std::chrono::steady_clock::now();
        const auto requested=start+std::chrono::milliseconds(mode==1?50:250);
        std::function<bool()> cancellation;
        if(mode!=0)cancellation=[&]{
            return std::chrono::steady_clock::now()>=requested;
        };
        const bool ok=ModelPreview3D::prepare_model(source,prepared,error,{}, {},true,cancellation);
        const auto finished=std::chrono::steady_clock::now();
        uint64_t digest=14695981039346656037ull;
        if(mode==0) {
            REQUIRE(ok);REQUIRE(prepared.semantic_source);
            for(const unsigned char byte:std::string_view(reinterpret_cast<const char*>(prepared.geometry.vertices.data()),prepared.geometry.vertices.size()*sizeof(float)))
                digest=(digest^byte)*1099511628211ull;
            if(round==0){geometry_id=prepared.geometry_id;content_id=prepared.semantic_source->content_id;render_digest=digest;}
            CHECK(prepared.geometry_id==geometry_id);
            CHECK(prepared.semantic_source->content_id==content_id);
            CHECK(digest==render_digest);
        } else {
            REQUIRE_FALSE(ok);CHECK(error=="Model preview loading canceled.");
            CHECK(prepared.geometry.is_empty());CHECK(prepared.triangles==0);CHECK_FALSE(prepared.semantic_source);
        }
        samples.push_back({{"round",round},{"mode",mode==0?"full":mode==1?"cancel_after_50ms":"cancel_after_250ms"},
            {"elapsed_ms",std::chrono::duration<double,std::milli>(finished-start).count()},
            {"return_after_request_ms",mode==0?nlohmann::json{}:nlohmann::json(std::chrono::duration<double,std::milli>(finished-requested).count())}});
    }
    CHECK(Slic3r::AI::model_artifact_sha256(source)==source_hash);
    boost::filesystem::ofstream output(report);
    output<<nlohmann::json{{"source_sha256",source_hash},{"samples",samples},{"geometry_id",geometry_id},
        {"semantic_content_id",content_id},{"render_fnv64",render_digest},
        {"scope","CPU preparation versus cooperative cancellation, same binary; no GUI or GPU"}}.dump(2);
    output.close();REQUIRE(output.good());
}


TEST_CASE("Worker history preparation preserves editing state and complete metric records", "[ModelPreviewState]")
{
    SavedPreview fixture;
    auto expected = fixture.metadata(fixture.original.geometry_id);
    expected["beauty_workbench"] = {{"puzzle_base_file", "original.glb"}, {"large_runs", std::vector<int>(10000, 17)},
                                   {"unknown", {{"text", "历史\n\"quoted\""}, {"enabled", true}}}};
    expected["provider"] = "tripo";
    expected["face_limit"] = 2000000;
    expected["geometry_quality"] = "detailed";
    expected["unknown_extension"] = {{"signed", -17}, {"unsigned", uint64_t(9007199254740993ULL)}};
    fixture.write(fixture.record, expected);
    ModelPreview3D::PreparedModel baseline, candidate;
    std::string baseline_error, candidate_error;
    REQUIRE(ModelPreview3D::prepare_model(fixture.model, baseline, baseline_error, {}, fixture.record));
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    auto worker = std::async(std::launch::async, [&] {
        return ModelPreview3D::prepare_model(fixture.model, candidate, candidate_error, {},
                                            fixture.record, true, {}, &snapshot);
    });
    REQUIRE(worker.get());
    fixture.check_restored(candidate);
    REQUIRE(candidate.geometry.vertices.size() == baseline.geometry.vertices.size());
    CHECK(std::memcmp(candidate.geometry.vertices.data(), baseline.geometry.vertices.data(),
                      baseline.geometry.vertices.size() * sizeof(float)) == 0);
    CHECK(candidate.geometry.indices == baseline.geometry.indices);
    CHECK(candidate.geometry_id == baseline.geometry_id);
    CHECK(candidate.triangles == baseline.triangles);
    CHECK(candidate.colors == baseline.colors);
    nlohmann::json fields;
    std::map<std::string, std::string> beauty;
    REQUIRE(snapshot.take_if_current(fixture.record, fields, beauty));
    CHECK_FALSE(fields.contains("beauty_workbench"));
    CHECK(nlohmann::json::parse(beauty.at("beauty_workbench")) == expected["beauty_workbench"]);
    const auto options = Slic3r::GUI::AIModelGenerationClient::restore_generation_options(fields);
    CHECK(options.face_limit == 2000000);
    fields["load_seconds"] = 1.25;
    expected["load_seconds"] = 1.25;
    const auto output = fixture.directory.path() / "metrics.json";
    REQUIRE(Slic3r::GUI::ModelGenerationPresentation::write_json_with_preencoded_fields(
        output, fields, beauty));
    CHECK(Slic3r::GUI::ModelGenerationPresentation::read_json(output) == expected);
    boost::filesystem::ifstream actual(output);
    const std::string bytes((std::istreambuf_iterator<char>(actual)), {});
    CHECK(bytes == expected.dump());
    CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, beauty));
}

TEST_CASE("History handoff detects same size changes even when timestamps are preserved", "[ModelPreviewState]")
{
    SavedPreview fixture;
    const nlohmann::json before {{"prompt", "old"}, {"beauty_workbench", {{"base", "first"}}}};
    const nlohmann::json after {{"prompt", "new"}, {"beauty_workbench", {{"base", "other"}}}};
    REQUIRE(before.dump().size() == after.dump().size());
    fixture.write(fixture.record, before);
    const auto modified = boost::filesystem::last_write_time(fixture.record);
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    fixture.write(fixture.record, after);
    boost::filesystem::last_write_time(fixture.record, modified);
    nlohmann::json fields = {{"sentinel", true}};
    std::map<std::string, std::string> beauty {{"sentinel", "unchanged"}};
    CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, beauty));
    CHECK(fields == nlohmann::json{{"sentinel", true}});
    CHECK(beauty == (std::map<std::string, std::string>{{"sentinel", "unchanged"}}));
    CHECK(Slic3r::GUI::ModelGenerationPresentation::read_json(fixture.record) == after);
    CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, beauty));
}

TEST_CASE("History handoff rejects missing and different records without modifying outputs", "[ModelPreviewState]")
{
    SavedPreview fixture;
    fixture.write(fixture.record, fixture.metadata(fixture.original.geometry_id));
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    nlohmann::json fields = {{"sentinel", true}};
    std::map<std::string, std::string> beauty {{"sentinel", "unchanged"}};
    SECTION("Different path")
    {
        fixture.write(fixture.adjacent, fixture.metadata(fixture.original.geometry_id));
        CHECK_FALSE(snapshot.take_if_current(fixture.adjacent, fields, beauty));
    }
    SECTION("Deleted record")
    {
        REQUIRE(boost::filesystem::remove(fixture.record));
        CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, beauty));
    }
    CHECK(fields == nlohmann::json{{"sentinel", true}});
    CHECK(beauty == (std::map<std::string, std::string>{{"sentinel", "unchanged"}}));
    CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, beauty));
}

TEST_CASE("Legacy history fields and arbitrary beauty values survive the existing ordered writer", "[ModelPreviewState]")
{
    SavedPreview fixture;
    const std::vector<nlohmann::json> documents {
        {{"prompt", "legacy"}, {"unknown", {{"nested", {1, 2, 3}}}}},
        {{"prompt", "legacy"}, {"beauty_workbench", nullptr}},
        {{"prompt", "legacy"}, {"beauty_workbench", "future-format"}}
    };
    for (const auto& document : documents) {
        DYNAMIC_SECTION(document.dump())
        {
            fixture.write(fixture.record, document);
            Slic3r::GUI::ModelHistoryMetadata snapshot;
            REQUIRE(snapshot.read(fixture.record));
            nlohmann::json fields;
            std::map<std::string, std::string> beauty;
            REQUIRE(snapshot.take_if_current(fixture.record, fields, beauty));
            const auto output = fixture.directory.path() / "legacy-output.json";
            const bool saved = beauty.empty()
                ? Slic3r::GUI::ModelGenerationPresentation::write_json(output, fields)
                : Slic3r::GUI::ModelGenerationPresentation::write_json_with_preencoded_fields(
                    output, fields, beauty);
            REQUIRE(saved);
            CHECK(Slic3r::GUI::ModelGenerationPresentation::read_json(output) == document);

}
}
}

TEST_CASE("Initial view shows planar assets instead of collapsing their faces", "[ModelPreviewState]")
{
    // Different planar orientations and units exercise the production initial
    // camera without constructing a window or an OpenGL context.
    for (double scale : {0.001, 1.0, 1000.0}) {
        for (int normal_axis = 0; normal_axis < 3; ++normal_axis) {
            Vec3d first = Vec3d::Zero(), second = Vec3d::Zero();
            first[(normal_axis + 1) % 3] = 90.0 * scale;
            second[(normal_axis + 2) % 3] = 30.0 * scale;
            const auto angles = ModelPreview3D::initial_view_angles(first + second);
            const auto rotation =
                Slic3r::Geometry::rotation_transform(angles.second * Vec3d::UnitX()) *
                Slic3r::Geometry::rotation_transform(angles.first * Vec3d::UnitZ());
            const Vec3d a = rotation * first, b = rotation * second;
            const double projected_area = std::abs(a.x() * b.y() - a.y() * b.x());
            CHECK(projected_area > first.norm() * second.norm() * 0.25);
        }
    }
}

TEST_CASE("Failed and canceled history reads discard the prior snapshot", "[ModelPreviewState]")
{
    SavedPreview fixture;
    const auto expected = fixture.metadata(fixture.original.geometry_id);
    fixture.write(fixture.record, expected);
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    SECTION("Malformed record")
    {
        boost::filesystem::ofstream output(fixture.adjacent);
        output << "{\"unfinished\":";
        output.close();
        CHECK_FALSE(snapshot.read(fixture.adjacent));
    }
    SECTION("Non-object record")
    {
        fixture.write(fixture.adjacent, nlohmann::json::array({1, 2, 3}));
        CHECK_FALSE(snapshot.read(fixture.adjacent));
    }
    SECTION("Missing record") { CHECK_FALSE(snapshot.read(fixture.adjacent)); }
    SECTION("Oversized record")
    {
        fixture.write(fixture.adjacent, expected);
        boost::filesystem::resize_file(fixture.adjacent, 128ULL * 1024 * 1024 + 1);
        CHECK_FALSE(snapshot.read(fixture.adjacent));
    }
    SECTION("Cancellation after parse")
    {
        unsigned checks = 0;
        CHECK_FALSE(snapshot.read(fixture.record, [&] { return ++checks >= 3; }));
        CHECK(checks >= 3);
    }
    SECTION("Preview cancellation")
    {
        ModelPreview3D::PreparedModel canceled;
        std::string error;
        CHECK_FALSE(ModelPreview3D::prepare_model(fixture.model, canceled, error, {}, fixture.record,
                                                 true, [] { return true; }, &snapshot));
        CHECK(canceled.geometry.is_empty());
        CHECK_FALSE(canceled.semantic_source);
    }
    nlohmann::json fields = {{"sentinel", true}};
    std::map<std::string, std::string> beauty {{"sentinel", "unchanged"}};
    CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, beauty));
    CHECK(fields == nlohmann::json{{"sentinel", true}});
    CHECK(beauty == (std::map<std::string, std::string>{{"sentinel", "unchanged"}}));
    CHECK(Slic3r::GUI::ModelGenerationPresentation::read_json(fixture.record) == expected);
}


TEST_CASE("Frozen historical render data keeps geometry, bounds and saved editing state", "[ModelPreviewState][GLModelGeometry]")
{
    SavedPreview saved;
    saved.write(saved.record, saved.metadata(saved.original.geometry_id));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    const auto vertices = prepared.geometry.vertices;
    const auto indices = prepared.geometry.indices;
    const auto id = prepared.geometry_id;
    Slic3r::GUI::GLModel reference;
    auto reference_geometry = prepared.geometry;
    reference.init_from(std::move(reference_geometry));
    SECTION("Successful preparation transfers storage without changing rendered data") {
        prepared.prepare_render_geometry();
        REQUIRE(prepared.render_geometry.has_value());
        CHECK(prepared.render_data().vertices == vertices);
        CHECK(prepared.render_data().indices == indices);
        CHECK(prepared.geometry_id == id);
        saved.check_restored(prepared);
        Slic3r::GUI::GLModel adopted;
        adopted.init_from(std::move(*prepared.render_geometry));
        CHECK(adopted.get_geometry().vertices == vertices);
        CHECK(adopted.get_geometry().indices == indices);
        CHECK(adopted.get_bounding_box().min == reference.get_bounding_box().min);
        CHECK(adopted.get_bounding_box().max == reference.get_bounding_box().max);
    }
    SECTION("Canceled preparation keeps owned data intact and can be retried") {
        REQUIRE_THROWS(prepared.prepare_render_geometry([] { return true; }));
        CHECK_FALSE(prepared.render_geometry.has_value());
        CHECK(prepared.geometry.vertices == vertices);
        CHECK(prepared.geometry.indices == indices);
        prepared.prepare_render_geometry();
        REQUIRE(prepared.render_geometry.has_value());
        CHECK(prepared.render_data().vertices == vertices);
        CHECK(prepared.render_data().indices == indices);
        saved.check_restored(prepared);
    }
}

namespace {
struct SharedPreviewFixture {
    indexed_triangle_set mesh;
    Slic3r::GUI::GLModel::Geometry geometry;
    explicit SharedPreviewFixture(size_t faces = 32)
    {
        using Geometry = Slic3r::GUI::GLModel::Geometry;
        mesh.vertices = {{1.f, 2.f, 3.f}, {4.f, 2.f, 3.f}, {1.f, 5.f, 3.f}};
        geometry.format = {Geometry::EPrimitiveType::Triangles, Geometry::EVertexLayout::P3N3T2};
        for (size_t f = 0; f < faces; ++f) {
            mesh.indices.emplace_back(0, 1, 2);
            const unsigned int base = unsigned(geometry.vertices_count());
            for (int v = 0; v < 3; ++v)
                geometry.add_vertex(mesh.vertices[v], Slic3r::Vec3f(0.f, 0.f, 1.f),
                                    Slic3r::Vec2f(float(0x102030 + v), 1.f));
            geometry.add_triangle(base, base + 1, base + 2);
        }
    }
};
void check_ordered_render_bits(const Slic3r::GUI::GLModel::Geometry& raw,
                               const Slic3r::GUI::GLModel::Geometry& shared)
{
    REQUIRE(shared.indices.size() == raw.indices.size());
    bool same = true;
    for (size_t i = 0; i < raw.indices.size(); ++i) {
        if (shared.indices[i] >= shared.vertices_count() ||
            std::memcmp(raw.vertices.data() + raw.indices[i] * 8,
                        shared.vertices.data() + shared.indices[i] * 8, 8 * sizeof(float)) != 0) {
            same = false; break;
        }
    }
    CHECK(same);
}
}

TEST_CASE("Historical render sharing keeps complete attributes and triangle order", "[ModelPreviewState][GLModelGeometry]")
{
    SharedPreviewFixture f;
    const auto original = f.geometry;
    auto shared = Slic3r::GUI::ModelPreviewGeometry::share_exact_vertices(f.mesh, f.geometry);
    REQUIRE(shared.has_value());
    CHECK(shared->vertices_count() < f.geometry.vertices_count());
    check_ordered_render_bits(f.geometry, *shared);
    CHECK(f.geometry.vertices == original.vertices);
    CHECK(f.geometry.indices == original.indices);
    Slic3r::GUI::GLModel a, b;
    auto raw = f.geometry;
    a.init_from(Slic3r::GUI::GLModel::prepare_geometry(std::move(raw)));
    b.init_from(Slic3r::GUI::GLModel::prepare_geometry(std::move(*shared)));
    CHECK(std::memcmp(a.get_bounding_box().min.data(), b.get_bounding_box().min.data(), 3 * sizeof(double)) == 0);
    CHECK(std::memcmp(a.get_bounding_box().max.data(), b.get_bounding_box().max.data(), 3 * sizeof(double)) == 0);
}

TEST_CASE("Historical render sharing separates hard edges colors alpha and signed attribute zeros", "[ModelPreviewState][GLModelGeometry]")
{
    SharedPreviewFixture f;
    f.geometry.vertices[3 * 8 + 3] = 1.f;       // A hard-edge normal at the same source vertex.
    f.geometry.vertices[6 * 8 + 6] = 0xabcdef;  // A local display-color override.
    f.geometry.vertices[9 * 8 + 7] = .25f;     // Original transparency.
    f.geometry.vertices[12 * 8 + 7] = -1.f;    // Locked color marker.
    f.geometry.vertices[15 * 8 + 3] = -0.f;    // Same numeric normal, different bits.
    auto shared = Slic3r::GUI::ModelPreviewGeometry::share_exact_vertices(f.mesh, f.geometry);
    REQUIRE(shared.has_value());
    check_ordered_render_bits(f.geometry, *shared);
    for (size_t corner : {3, 6, 9, 12, 15}) CHECK(shared->indices[corner] != shared->indices[0]);
}

TEST_CASE("Unsupported render sharing leaves the expanded geometry intact", "[ModelPreviewState][GLModelGeometry]")
{
    using Geometry = Slic3r::GUI::GLModel::Geometry;
    SharedPreviewFixture f;
    SECTION("Nontriangle geometry") { f.geometry.format.type = Geometry::EPrimitiveType::Lines; }
    SECTION("Different layout") { f.geometry.format.vertex_layout = Geometry::EVertexLayout::P3N3; }
    SECTION("Different index storage") { f.geometry.index_type = Geometry::EIndexType::USHORT; }
    SECTION("Truncated attributes") { f.geometry.vertices.pop_back(); }
    SECTION("Different face count") { f.mesh.indices.pop_back(); }
    SECTION("Nonsequential expanded indices") { f.geometry.indices[2] = 0; }
    SECTION("Invalid source vertex") { f.mesh.indices[0][0] = -1; }
    SECTION("Unshared source mesh") { f.mesh.vertices.resize(f.geometry.vertices_count()); }
    SECTION("Nonfinite position") { f.geometry.vertices[0] = std::numeric_limits<float>::quiet_NaN(); }
    SECTION("Signed zero position") { f.geometry.vertices[0] = -0.f; }
    const auto raw = f.geometry;
    CHECK_FALSE(Slic3r::GUI::ModelPreviewGeometry::share_exact_vertices(f.mesh, f.geometry));
    REQUIRE(f.geometry.vertices.size() == raw.vertices.size());
    CHECK(std::memcmp(f.geometry.vertices.data(), raw.vertices.data(), raw.vertices.size() * sizeof(float)) == 0);
    CHECK(f.geometry.indices == raw.indices);
}

TEST_CASE("Render sharing bounds work for a vertex with many distinct attributes", "[ModelPreviewState][GLModelGeometry]")
{
    SharedPreviewFixture f;
    for (size_t face = 0; face < f.mesh.indices.size(); ++face)
        f.geometry.vertices[face * 3 * 8 + 6] = float(face);
    const auto raw = f.geometry;
    CHECK_FALSE(Slic3r::GUI::ModelPreviewGeometry::share_exact_vertices(f.mesh, f.geometry));
    CHECK(f.geometry.vertices == raw.vertices);
    CHECK(f.geometry.indices == raw.indices);
}

TEST_CASE("Canceled render sharing and bounds preparation preserve data for retry", "[ModelPreviewState][GLModelGeometry]")
{
    SharedPreviewFixture f(4096);
    ModelPreview3D::PreparedModel prepared;
    prepared.mesh = f.mesh;
    prepared.geometry = f.geometry;
    unsigned calls = 0;
    SECTION("Cancel before allocation") {
        REQUIRE_THROWS(prepared.prepare_render_geometry([] { return true; }, true));
    }
    SECTION("Cancel during sharing") {
        REQUIRE_THROWS(prepared.prepare_render_geometry([&] { return ++calls == 4; }, true));
        CHECK(calls == 4);
    }
    SECTION("Cancel after sharing during bounds") {
        REQUIRE_THROWS(prepared.prepare_render_geometry([&] { return ++calls == 6; }, true));
        CHECK(calls == 6);
    }
    CHECK_FALSE(prepared.render_geometry);
    CHECK(prepared.geometry.vertices == f.geometry.vertices);
    CHECK(prepared.geometry.indices == f.geometry.indices);
    prepared.prepare_render_geometry({}, true);
    REQUIRE(prepared.render_geometry);
    check_ordered_render_bits(f.geometry, prepared.render_data());
    CHECK(prepared.geometry.is_empty());
}

TEST_CASE("Historical render sharing preserves CPU mesh and saved editing state", "[ModelPreviewState][GLModelGeometry]")
{
    SavedPreview saved;
    saved.write(saved.record, saved.metadata(saved.original.geometry_id));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    const auto raw = prepared.geometry;
    const auto mesh = prepared.mesh;
    const auto id = prepared.geometry_id;
    prepared.prepare_render_geometry({}, true);
    check_ordered_render_bits(raw, prepared.render_data());
    CHECK(prepared.mesh.vertices == mesh.vertices);
    CHECK(prepared.mesh.indices == mesh.indices);
    CHECK(prepared.geometry_id == id);
    saved.check_restored(prepared);
}

TEST_CASE("Accepted and draft history fields hand off together without losing future values", "[ModelPreviewState]")
{
    SavedPreview fixture;
    const std::vector<nlohmann::json> documents {
        {{"beauty_puzzle_draft", {{"runs", {1, 2, 3}}, {"future", "历史\n\"draft\""}}}},
        {{"beauty_workbench", {{"runs", {4, 5}}}}, {"beauty_puzzle_draft", {{"unsigned", uint64_t(9007199254740993ULL)}, {"negative_zero", -0.0}}}},
        {{"beauty_workbench", nullptr}, {"beauty_puzzle_draft", "future-format"}}
    };
    for (const auto& document : documents) {
        DYNAMIC_SECTION(document.dump()) {
            auto expected = document;
            expected["unknown"] = {{"preserved", true}};
            fixture.write(fixture.record, expected);
            Slic3r::GUI::ModelHistoryMetadata snapshot;
            REQUIRE(snapshot.read(fixture.record));
            CHECK_FALSE(snapshot.restoration_fields().contains("beauty_workbench"));
            CHECK_FALSE(snapshot.restoration_fields().contains("beauty_puzzle_draft"));
            nlohmann::json fields;
            std::map<std::string, std::string> encoded;
            REQUIRE(snapshot.take_if_current(fixture.record, fields, encoded));
            fields["load_seconds"] = 1.25;
            expected["load_seconds"] = 1.25;
            const auto output = fixture.directory.path() / "draft-metrics.json";
            REQUIRE(Slic3r::GUI::ModelGenerationPresentation::write_json_with_preencoded_fields(output, fields, encoded));
            boost::filesystem::ifstream stream(output, std::ios::binary);
            const std::string actual((std::istreambuf_iterator<char>(stream)), {});
            CHECK(actual == expected.dump());
            CHECK(Slic3r::GUI::ModelGenerationPresentation::read_json(output) == expected);
        }
    }
}

TEST_CASE("Large history freezing preserves canonical bytes and ordinary fields", "[ModelPreviewState]")
{
    SavedPreview fixture;
    nlohmann::json expected {
        {"beauty_workbench", {{"values", std::vector<int>(30000, 17)},
            {"unicode", u8"历史\n\"quoted\""}, {"scalars", {nullptr, true, false, -0.0, 1e-300}},
            {"large_unsigned", uint64_t(18446744073709551615ULL)}}},
        {"beauty_puzzle_draft", {{"future", {{"z", 1}, {"a", nlohmann::json::array()}}}}},
        {"unknown", {{"nested_beauty_workbench", {1, 2, 3}}, {"negative_zero", -0.0}, {"exact", uint64_t(9007199254740993ULL)}}}
    };
    REQUIRE(expected.dump().size() >= 64 * 1024);
    fixture.write(fixture.record, expected);
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    nlohmann::json fields;
    std::map<std::string, std::string> frozen;
    REQUIRE(snapshot.take_if_current(fixture.record, fields, frozen));
    CHECK(fields.at("unknown").dump() == expected.at("unknown").dump());
    CHECK(frozen.at("beauty_workbench") == expected.at("beauty_workbench").dump());
    CHECK(frozen.at("beauty_puzzle_draft") == expected.at("beauty_puzzle_draft").dump());
    const auto output = fixture.directory.path() / "streaming.json";
    REQUIRE(Slic3r::GUI::ModelGenerationPresentation::write_json_with_preencoded_fields(output, fields, frozen));
    boost::filesystem::ifstream input(output, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    CHECK(bytes == expected.dump());
}

TEST_CASE("Large frozen history keeps the last duplicate root and nested keys", "[ModelPreviewState]")
{
    SavedPreview fixture;
    const std::string text = std::string(64 * 1024, ' ') +
        "{\"beauty_workbench\":null,\"beauty_workbench\":{\"z\":1,\"a\":2,\"z\":3},"
        "\"beauty_puzzle_draft\":[],\"beauty_puzzle_draft\":\"future\","
        "\"unknown\":{\"b\":1,\"b\":2},\"unknown\":{\"b\":3}}";
    { boost::filesystem::ofstream output(fixture.record, std::ios::binary); output << text; }
    const auto expected = nlohmann::json::parse(text);
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    nlohmann::json fields;
    std::map<std::string, std::string> frozen;
    REQUIRE(snapshot.take_if_current(fixture.record, fields, frozen));
    CHECK(fields == nlohmann::json{{"unknown", expected.at("unknown")}});
    CHECK(frozen.at("beauty_workbench") == expected.at("beauty_workbench").dump());
    CHECK(frozen.at("beauty_puzzle_draft") == expected.at("beauty_puzzle_draft").dump());
}

TEST_CASE("Malformed large frozen records never publish partial history", "[ModelPreviewState]")
{
    const auto broken = GENERATE(std::string("{\"beauty_workbench\":[1,]}"),
        std::string("{\"beauty_workbench\":{\"number\":1e5000}}"),
        std::string("{\"beauty_workbench\":\"\\ud800\"}"),
        std::string("{\"beauty_workbench\":\"") + char(0xff) + "\"}",
        std::string("{\"beauty_workbench\":[]} []"), std::string("[]"));
    SavedPreview fixture;
    fixture.write(fixture.record, {{"prompt", "prior snapshot"}});
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    { boost::filesystem::ofstream output(fixture.record, std::ios::binary); output << std::string(64 * 1024, ' ') << broken; }
    CHECK_FALSE(snapshot.read(fixture.record));
    CHECK(snapshot.restoration_fields().is_null());
    nlohmann::json fields {{"sentinel", true}};
    std::map<std::string, std::string> frozen {{"sentinel", "unchanged"}};
    CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, frozen));
    CHECK(fields == nlohmann::json{{"sentinel", true}});
    CHECK(frozen == (std::map<std::string, std::string>{{"sentinel", "unchanged"}}));
}

TEST_CASE("Cancellation inside large frozen history parsing discards the snapshot and permits retry", "[ModelPreviewState]")
{
    SavedPreview fixture;
    const nlohmann::json expected {{"beauty_workbench", {{"values", std::vector<int>(30000, 17)}}}, {"prompt", "keep"}};
    REQUIRE(expected.dump().size() >= 64 * 1024);
    fixture.write(fixture.record, expected);
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    int checks = 0;
    // Initial read, completed IO and parse entry precede the first SAX checkpoint.
    CHECK_FALSE(snapshot.read(fixture.record, [&] { return ++checks >= 4; }));
    CHECK(checks == 4);
    CHECK(snapshot.restoration_fields().is_null());
    nlohmann::json fields {{"sentinel", true}};
    std::map<std::string, std::string> frozen {{"sentinel", "unchanged"}};
    CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, frozen));
    CHECK(fields == nlohmann::json{{"sentinel", true}});
    CHECK(frozen == (std::map<std::string, std::string>{{"sentinel", "unchanged"}}));
    REQUIRE(snapshot.read(fixture.record));
    REQUIRE(snapshot.take_if_current(fixture.record, fields, frozen));
    CHECK(frozen.at("beauty_workbench") == expected.at("beauty_workbench").dump());
}

TEST_CASE("Large ordinary history values keep DOM types and nested beauty fields", "[ModelPreviewState]")
{
    SavedPreview fixture;
    const nlohmann::json expected {{"unknown", {{"beauty_workbench", {{"z", 1}, {"a", 2}}},
        {"values", std::vector<uint64_t>(30000, 9007199254740993ULL)}, {"float", -0.0}}}};
    REQUIRE(expected.dump().size() >= 64 * 1024);
    fixture.write(fixture.record, expected);
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    nlohmann::json fields;
    std::map<std::string, std::string> frozen;
    REQUIRE(snapshot.take_if_current(fixture.record, fields, frozen));
    CHECK(frozen.empty());
    CHECK(fields.dump() == expected.dump());
    CHECK(fields.at("unknown").at("values").at(0).is_number_unsigned());
    CHECK(fields.at("unknown").at("float").is_number_float());

}

TEST_CASE("Initial view keeps the portrait facing the camera for volumetric models", "[ModelPreviewState]")
{
    const auto angles = ModelPreview3D::initial_view_angles(Vec3d(52.1, 54.0, 100.0));
    const auto rotation =
        Slic3r::Geometry::rotation_transform(angles.second * Vec3d::UnitX()) *
        Slic3r::Geometry::rotation_transform(angles.first * Vec3d::UnitZ());
    const Vec3d facing = rotation * Vec3d::UnitX();
    CHECK(facing.z() > 0.99);
    CHECK(std::abs(facing.x()) < 1e-8);
    CHECK(std::abs(facing.y()) < 1e-8);
}

TEST_CASE("Thin planar assets remain visible before their thickness reaches zero", "[ModelPreviewState]")
{
    const auto angles = ModelPreview3D::initial_view_angles(Vec3d(90.0, 0.01, 30.0));
    const auto rotation =
        Slic3r::Geometry::rotation_transform(angles.second * Vec3d::UnitX()) *
        Slic3r::Geometry::rotation_transform(angles.first * Vec3d::UnitZ());
    const Vec3d facing = rotation * Vec3d::UnitY();
    CHECK(std::abs(facing.z()) > 0.25);
}
