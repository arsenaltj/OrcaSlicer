#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryMetadata.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitPrintFlow.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryObservations.hpp"
#include "test_utils.hpp"

#include <boost/nowide/cstdlib.hpp>
#include <boost/filesystem.hpp>
#include <chrono>
#include <iostream>
#include <sstream>

using Slic3r::GUI::ModelLibraryMetadata;
using Json = nlohmann::json;

TEST_CASE("Saved versions and drafts protect every referenced model", "[ModelLibraryMetadata]")
{
    std::istringstream saved(R"({"model_path":"downloads/version.obj","source_model":"models/original.glb",
        "original_appearance":{"carrier_model":"models/version.source-appearance.obj","origin_model":"models/original.glb"},
        "beauty_workbench":{"puzzle_base_file":"version-base.obj","piece_runs":[[1,2]]}})");
    const auto dependencies = ModelLibraryMetadata::model_dependencies(saved);
    CHECK(dependencies.count("models/original.glb") == 1);
    CHECK(dependencies.count("models/version.source-appearance.obj") == 1);
    CHECK(dependencies.count("downloads/version-base.obj") == 1);

    std::istringstream draft(R"({"beauty_puzzle_draft":{"puzzle_base_file":"version-base.obj"}})");
    CHECK(ModelLibraryMetadata::references_model(draft, "downloads/version-base.obj",
        "downloads/version.obj"));

    std::istringstream unrelated(R"({"prompt":"original.glb","model_path":"downloads/version.obj",
        "beauty_workbench":{"puzzle_base_file":"other.glb"}})");
    CHECK_FALSE(ModelLibraryMetadata::references_model(unrelated, "models/original.glb"));
    std::istringstream malformed("{\"beauty_workbench\":[1,]}");
    CHECK_THROWS(ModelLibraryMetadata::references_model(malformed, "models/original.glb"));

    std::istringstream ownerless(R"({"beauty_puzzle_draft":{"puzzle_base_file":"original.glb"}})");
    CHECK_THROWS(ModelLibraryMetadata::references_model(ownerless, "original.glb"));
}

TEST_CASE("Model dependency paths are normalized and cannot escape the library", "[ModelLibraryMetadata]")
{
    std::istringstream normalized(R"({"source_model":"models/portraits/../original.glb"})");
    CHECK(ModelLibraryMetadata::references_model(normalized, "models/original.glb"));
    for (const std::string value : {"../outside.glb", "/absolute.glb"}) {
        std::istringstream invalid("{\"source_model\":" + Json(value).dump() + "}");
        CHECK_THROWS(ModelLibraryMetadata::model_dependencies(invalid));
    }
}

TEST_CASE("Only an owned original appearance companion is eligible for cleanup", "[ModelLibraryMetadata]")
{
    ScopedTemporaryDir temporary("orca-original-appearance-companion");
    const auto root = temporary.path();
    const auto models = root / "models";
    boost::filesystem::create_directories(models);
    const auto companion = models / "version.source-appearance.obj";
    {
        boost::filesystem::ofstream output(companion, std::ios::binary);
        output << "appearance";
    }
    const Json metadata {{"original_appearance", {
        {"carrier_model", "models/version.source-appearance.obj"}}}};
    const auto owned = ModelLibraryMetadata::owned_original_appearance_companion(metadata, root);
    REQUIRE(owned);
    CHECK(*owned == boost::filesystem::canonical(companion));

    for (const std::string carrier : {
            "models/original.obj", "../outside.source-appearance.obj",
            "models/missing.source-appearance.obj"}) {
        const Json rejected {{"original_appearance", {{"carrier_model", carrier}}}};
        CHECK_FALSE(ModelLibraryMetadata::owned_original_appearance_companion(rejected, root));
    }
}

TEST_CASE("History summaries skip full editing arrays without borrowing nested fields", "[ModelLibraryMetadata]")
{
    std::istringstream input(R"({"beauty_workbench":{"prompt":"wrong", "palette":["wrong"],"piece_runs":[[1,2],[2,3]]},
        "source":"local_finishing","job_id":"finish-example","model_path":"downloads/model.glb",
        "prompt":"Eye edits","palette":["#ffffff"],"palette_roles":{"face":"#ffffff"},
        "generated_at":7,"triangle_count":5,"load_seconds":0.5,"use_printable_colors":true,
        "face_color_intent":{"colors":[[1,2,3],[3,2,1]]},"future_field":{"prompt":"wrong"}})");
    const auto summary = ModelLibraryMetadata::parse(input);
    CHECK(summary.at("prompt") == "Eye edits");
    CHECK(summary.at("palette") == Json::array({"#ffffff"}));
    CHECK(summary.at("palette_roles").at("face") == "#ffffff");
    CHECK(summary.at("triangle_count") == 5);
    CHECK_FALSE(summary.contains("beauty_workbench"));
    CHECK_FALSE(summary.contains("face_color_intent"));
    CHECK_FALSE(summary.contains("future_field"));
}

TEST_CASE("History summaries validate skipped JSON and tolerate malformed optional fields", "[ModelLibraryMetadata]")
{
    for (const std::string source : {"[]", "null", "{\"beauty_workbench\":[1,]}", "{\"prompt\":\"ok\"} trailing"}) {
        std::istringstream input(source);
        CHECK_FALSE(ModelLibraryMetadata::parse(input).is_object());
    }
    std::istringstream input(R"({"prompt":false,"generated_at":{},"palette":null,"load_seconds":"bad","source":"local_import"})");
    const Json expected {{"source", "local_import"}};
    CHECK(ModelLibraryMetadata::parse(input) == expected);
}

TEST_CASE("History cache detects edits deletion and recreation while preserving source bytes", "[ModelLibraryMetadata]")
{
    ScopedTemporaryDir temporary("orca-history-summary");
    const auto path = temporary.path() / "model.json";
    const auto write = [&](const std::string& title) {
        boost::filesystem::ofstream output(path);
        output << Json{{"prompt", title}, {"beauty_workbench", {{"untouched", {1,2,3}}}}}.dump();
    };
    ModelLibraryMetadata cache;
    write("first");
    const auto timestamp = std::filesystem::last_write_time(std::filesystem::path(path.native()));
    CHECK(cache.read(path).at("prompt") == "first");
    CHECK(cache.read(path).at("prompt") == "first");
    write("other"); // Equal length: modification time, not size, invalidates it.
    std::filesystem::last_write_time(std::filesystem::path(path.native()), timestamp + std::chrono::seconds(2));
    CHECK(cache.read(path).at("prompt") == "other");
    boost::filesystem::ifstream input(path);
    Json original; input >> original; input.close();
    CHECK(original.at("beauty_workbench").at("untouched") == Json::array({1,2,3}));
    boost::filesystem::remove(path);
    CHECK_FALSE(cache.read(path).is_object());
    write("new");
    CHECK(cache.read(path).at("prompt") == "new");
}

TEST_CASE("History summary reads edited portraits larger than 32 MiB", "[ModelLibraryMetadata]")
{
    ScopedTemporaryDir temporary("orca-history-summary-large");
    const auto path = temporary.path() / "finish-large.json";
    {
        boost::filesystem::ofstream output(path, std::ios::binary);
        output << R"({"source":"local_finishing","job_id":"finish-large","prompt":"Saved portrait","beauty_workbench":)";
        // Editing-only content can be tens of megabytes; it must not hide the saved model.
        const std::string padding(1024 * 1024, ' ');
        for (int i = 0; i < 33; ++i) output << padding;
        output << "{} }";
    }
    REQUIRE(boost::filesystem::file_size(path) > 32ULL * 1024 * 1024);
    ModelLibraryMetadata cache;
    const auto summary = cache.read(path);
    REQUIRE(summary.is_object());
    CHECK(summary.at("job_id") == "finish-large");
    CHECK(summary.at("prompt") == "Saved portrait");
    CHECK_FALSE(summary.contains("beauty_workbench"));
}

TEST_CASE("History summary cache survives a restart and rejects stale or corrupt entries", "[ModelLibraryMetadata]")
{
    ScopedTemporaryDir temporary("orca-history-summary-persistent");
    const auto path = temporary.path() / "model.json";
    const auto cache_path = temporary.path() / "library-summary-cache-v1.json";
    auto write = [&](const std::string& prompt) {
        boost::filesystem::ofstream output(path, std::ios::binary);
        output << Json{{"prompt", prompt}, {"generated_at", 42}}.dump();
    };
    write("persisted");
    const auto original_size = boost::filesystem::file_size(path);
    std::filesystem::file_time_type initial_stamp;
    {
        ModelLibraryMetadata first;
        CHECK(first.read(path).at("prompt") == "persisted");
        initial_stamp = std::filesystem::last_write_time(std::filesystem::path(path.native()));
    }
    REQUIRE(boost::filesystem::is_regular_file(cache_path));

    // A new object can serve the old summary without parsing the source again.
    boost::filesystem::ofstream corrupt_source(path, std::ios::binary);
    corrupt_source << std::string(original_size, 'x');
    corrupt_source.close();
    std::filesystem::last_write_time(std::filesystem::path(path.native()), initial_stamp);
    {
        ModelLibraryMetadata restarted;
        CHECK(restarted.read(path).at("prompt") == "persisted");
    }

    // A changed stamp invalidates the persisted entry and exposes the bad
    // source instead of silently retaining a stale card.
    std::filesystem::last_write_time(std::filesystem::path(path.native()), initial_stamp + std::chrono::seconds(2));
    {
        ModelLibraryMetadata changed;
        CHECK_FALSE(changed.read(path).is_object());
    }

    // A malformed cache is ignored and the normal source parse remains usable.
    write("fresh");
    boost::filesystem::ofstream broken_cache(cache_path, std::ios::binary);
    broken_cache << "not-json";
    broken_cache.close();
    ModelLibraryMetadata recovered;
    CHECK(recovered.read(path).at("prompt") == "fresh");
}

// Optional read-only measurement against retained local history; no fixture or
// synthetic provider can establish the real main-window latency.
TEST_CASE("Existing history summaries match full metadata", "[.][ModelLibraryMetadataBenchmark]")
{
    const char* directory = boost::nowide::getenv("ORCA_HISTORY_BENCHMARK_DIR");
    REQUIRE(directory != nullptr);
    ModelLibraryMetadata cache;
    std::vector<boost::filesystem::path> paths;
    for (const auto& entry : boost::filesystem::directory_iterator(directory))
        if (entry.path().extension() == ".json" && entry.path().filename().string().find("orcaslicer-ai-") == 0)
            paths.push_back(entry.path());
    REQUIRE_FALSE(paths.empty());
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    std::vector<Json> summaries;
    for (const auto& path : paths) summaries.push_back(cache.read(path));
    const auto cold = Clock::now();
    for (size_t i = 0; i < paths.size(); ++i) CHECK(cache.read(paths[i]) == summaries[i]);
    const auto warm = Clock::now();
    for (size_t i = 0; i < paths.size(); ++i) {
        boost::filesystem::ifstream input(paths[i]);
        const auto full = Json::parse(input, nullptr, false);
        if (full.is_object()) for (auto it = summaries[i].begin(); it != summaries[i].end(); ++it)
            CHECK(full.at(it.key()) == it.value());
    }
    std::cout << "history_files=" << paths.size()
              << " cold_seconds=" << std::chrono::duration<double>(cold-start).count()
              << " warm_seconds=" << std::chrono::duration<double>(warm-cold).count()
              << " full_parse_once_seconds=" << std::chrono::duration<double>(Clock::now()-warm).count() << '\n';
}

namespace {
struct ObservedLibraryCache {
    ModelLibraryMetadata summaries;
    Json read(const boost::filesystem::path& path) {
        return Slic3r::GUI::ModelLibraryObservations::overlay(summaries.read(path), path);
    }
};
Json certified_library_record()
{
    namespace PP = Slic3r::GUI::PortraitPrint;
    Slic3r::GUI::WorkbenchCheckResult check;
    check.status = Slic3r::GUI::WorkbenchCheckStatus::Normal;
    check.purpose = Slic3r::GUI::WorkbenchCheckPurpose::PortraitPostColorReadOnly;
    check.model_sha256 = std::string(64, '1');
    check.before_faces = 789; check.before_vertices = 1200;
    check.self_intersections_checked = true;
    Json metadata {{"schema_version", 4}, {"model_sha256", check.model_sha256},
        {"source", "local_finishing"}, {"job_id", "finish-portrait"},
        {"triangle_count", 789}, {"dimensions", {1.0, 2.0, 3.0}}};
    metadata["workbench_check_certificate"] = PP::encode_workbench_check_certificate(check,
        check.purpose, "geometry", "appearance", "portrait.json", metadata,
        std::string(64, '2'), std::string(64, '3'));
    return metadata;
}
void write_library_record(const boost::filesystem::path& path, const Json& metadata)
{
    boost::filesystem::ofstream output(path, std::ios::binary);
    output << metadata.dump(); output.close();
    REQUIRE(output.good());
}
}

TEST_CASE("Certified portrait observations preserve the authoritative certificate bytes",
    "[ModelLibraryMetadata][PortraitPrintFlow][P0]")
{
    namespace PP = Slic3r::GUI::PortraitPrint;
    ScopedTemporaryDir temporary("orca-certified-observations");
    const auto path = temporary.path() / "portrait.json";
    const auto metadata = certified_library_record();
    write_library_record(path, metadata);
    const auto hash = Slic3r::AI::model_artifact_sha256(path);
    REQUIRE(Slic3r::GUI::ModelLibraryObservations::write_observations(path, metadata, {{"load_seconds", 2.75}}));
    REQUIRE(Slic3r::GUI::ModelLibraryObservations::write_observations(path, metadata, {{"imported_at", 42}}));
    REQUIRE(Slic3r::GUI::ModelLibraryObservations::write_observations(path, metadata,
        {{"print_feedback", "issue"}, {"print_feedback_at", 43}}));
    CHECK(Slic3r::AI::model_artifact_sha256(path) == hash);
    boost::filesystem::ifstream input(path);
    const auto reopened = Json::parse(input);
    CHECK(reopened == metadata);
    CHECK(PP::valid_workbench_check_certificate(reopened.at("workbench_check_certificate"),
        Slic3r::GUI::WorkbenchCheckPurpose::PortraitPostColorReadOnly, std::string(64, '1'),
        "geometry", "appearance", "portrait.json", reopened, std::string(64, '2'), std::string(64, '3')));
    ObservedLibraryCache cache;
    const auto summary = cache.read(path);
    CHECK_THAT(summary.at("load_seconds").get<double>(), Catch::Matchers::WithinAbs(2.75, 1e-9));
    CHECK(summary.at("imported_at") == 42);
    CHECK(summary.at("print_feedback") == "issue");
    CHECK(summary.at("print_feedback_at") == 43);
    CHECK(summary.at("triangle_count") == 789);
}

TEST_CASE("Observation refresh and deletion bypass unchanged summary cache stamps",
    "[ModelLibraryMetadata]")
{
    ScopedTemporaryDir temporary("orca-observations-cache");
    const auto path = temporary.path() / "portrait.json";
    const auto metadata = certified_library_record();
    write_library_record(path, metadata);
    const auto authority_stamp = boost::filesystem::last_write_time(path);
    {
        ObservedLibraryCache cache;
        CHECK_FALSE(cache.read(path).contains("load_seconds"));
        REQUIRE(Slic3r::GUI::ModelLibraryObservations::write_observations(path, metadata, {{"load_seconds", 1.25}}));
        CHECK_THAT(cache.read(path).at("load_seconds").get<double>(), Catch::Matchers::WithinAbs(1.25, 1e-9));
        REQUIRE(Slic3r::GUI::ModelLibraryObservations::write_observations(path, metadata, {{"load_seconds", 3.75}}));
        CHECK_THAT(cache.read(path).at("load_seconds").get<double>(), Catch::Matchers::WithinAbs(3.75, 1e-9));
        REQUIRE(boost::filesystem::remove(Slic3r::GUI::ModelLibraryObservations::observations_path(path)));
        CHECK_FALSE(cache.read(path).contains("load_seconds"));
        REQUIRE(Slic3r::GUI::ModelLibraryObservations::write_observations(path, metadata,
            {{"print_feedback", "success"}, {"print_feedback_at", 99}}));
    }
    ObservedLibraryCache restarted;
    CHECK(restarted.read(path).at("print_feedback") == "success");
    CHECK(boost::filesystem::last_write_time(path) == authority_stamp);
}

TEST_CASE("Observations require exact model identity and reject geometry and coloring fields",
    "[ModelLibraryMetadata][P0]")
{
    ScopedTemporaryDir temporary("orca-observations-identity");
    const auto path = temporary.path() / "portrait.json";
    auto metadata = certified_library_record();
    write_library_record(path, metadata);
    REQUIRE(Slic3r::GUI::ModelLibraryObservations::write_observations(path, metadata,
        {{"print_feedback", "issue"}, {"print_feedback_at", 42}}));
    const auto companion = Slic3r::GUI::ModelLibraryObservations::observations_path(path);
    const auto original_hash = Slic3r::AI::model_artifact_sha256(companion);
    for (const Json fields : {Json{{"geometry_id", "foreign"}}, Json{{"color_count", 3}},
            Json{{"triangle_count", 1}}, Json{{"load_seconds", -1.0}}, Json{{"print_feedback", "other"}}}) {
        CHECK_FALSE(Slic3r::GUI::ModelLibraryObservations::write_observations(path, metadata, fields));
        CHECK(Slic3r::AI::model_artifact_sha256(companion) == original_hash);
    }
    REQUIRE(Slic3r::GUI::ModelLibraryObservations::owned_observations_companion(path, metadata));
    auto replacement = metadata;
    replacement["model_sha256"] = std::string(64, '9');
    CHECK_FALSE(Slic3r::GUI::ModelLibraryObservations::owned_observations_companion(path, replacement));
    write_library_record(path, replacement);
    const auto changed_stamp = std::filesystem::last_write_time(std::filesystem::path(path.native()));
    std::filesystem::last_write_time(std::filesystem::path(path.native()), changed_stamp + std::chrono::seconds(2));
    ObservedLibraryCache cache;
    CHECK_FALSE(cache.read(path).contains("print_feedback"));
    CHECK_FALSE(Slic3r::GUI::ModelLibraryObservations::write_observations(path, replacement, {{"load_seconds", 0.5}}));
    CHECK(Slic3r::AI::model_artifact_sha256(companion) == original_hash);
    REQUIRE(boost::filesystem::remove(companion));
    REQUIRE(Slic3r::GUI::ModelLibraryObservations::write_observations(path, replacement, {{"load_seconds", 0.5}}));
    CHECK_FALSE(cache.read(path).contains("print_feedback"));
    CHECK_THAT(cache.read(path).at("load_seconds").get<double>(), Catch::Matchers::WithinAbs(0.5, 1e-9));
    auto malformed = Json{{"schema", "orca.model-library-observations/v1"},
        {"model_sha256", std::string(64, '9')}, {"fields", {{"final_slot_binding", "foreign"}}}};
    write_library_record(companion, malformed);
    CHECK_FALSE(cache.read(path).contains("load_seconds"));
    CHECK_FALSE(cache.read(path).contains("final_slot_binding"));
    CHECK_FALSE(Slic3r::GUI::ModelLibraryObservations::owned_observations_companion(path, replacement));
    const auto malformed_hash = Slic3r::AI::model_artifact_sha256(companion);
    CHECK_FALSE(Slic3r::GUI::ModelLibraryObservations::write_observations(path, replacement, {{"imported_at", 43}}));
    CHECK(Slic3r::AI::model_artifact_sha256(companion) == malformed_hash);
}

TEST_CASE("Observation companions never replace ordinary history or certify malformed proof",
    "[ModelLibraryMetadata]")
{
    ScopedTemporaryDir temporary("orca-observations-ordinary");
    const auto path = temporary.path() / "portrait.json";
    Json ordinary {{"schema_version", 4}, {"model_sha256", std::string(64, '1')}, {"load_seconds", 0.25}};
    write_library_record(path, ordinary);
    CHECK_FALSE(Slic3r::GUI::ModelLibraryObservations::write_observations(path, ordinary, {{"load_seconds", 1.0}}));
    CHECK_FALSE(boost::filesystem::exists(Slic3r::GUI::ModelLibraryObservations::observations_path(path)));
    ObservedLibraryCache cache;
    CHECK_THAT(cache.read(path).at("load_seconds").get<double>(), Catch::Matchers::WithinAbs(0.25, 1e-9));
    ordinary["workbench_check_certificate"] = nullptr;
    write_library_record(path, ordinary);
    const auto hash = Slic3r::AI::model_artifact_sha256(path);
    REQUIRE(Slic3r::GUI::ModelLibraryObservations::has_check_certificate(ordinary));
    REQUIRE(Slic3r::GUI::ModelLibraryObservations::write_observations(path, ordinary, {{"load_seconds", 1.0}}));
    CHECK(Slic3r::AI::model_artifact_sha256(path) == hash);
    CHECK(ordinary.at("workbench_check_certificate").is_null());
    const auto owned = Slic3r::GUI::ModelLibraryObservations::owned_observations_companion(path, ordinary);
    REQUIRE(owned);
    CHECK(*owned == Slic3r::GUI::ModelLibraryObservations::observations_path(path));
    REQUIRE(boost::filesystem::remove(*owned));
    CHECK_FALSE(Slic3r::GUI::ModelLibraryObservations::owned_observations_companion(path, ordinary));
}
