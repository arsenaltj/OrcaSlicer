#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryMetadata.hpp"
#include "test_utils.hpp"

#include <boost/nowide/cstdlib.hpp>
#include <boost/filesystem.hpp>
#include <chrono>
#include <iostream>
#include <sstream>

using Slic3r::GUI::ModelLibraryMetadata;
using Json = nlohmann::json;

TEST_CASE("Saved beauty versions and drafts protect their original model", "[ModelLibraryMetadata]")
{
    for (const std::string key : {"beauty_workbench", "beauty_puzzle_draft"}) {
        std::istringstream input("{\""+key+"\":{\"puzzle_base_file\":\"original.glb\",\"piece_runs\":[[1,2]]}}");
        CHECK(ModelLibraryMetadata::references_base(input,"original.glb"));
    }
    std::istringstream unrelated(R"({"prompt":"original.glb","beauty_workbench":{"puzzle_base_file":"other.glb"}})");
    CHECK_FALSE(ModelLibraryMetadata::references_base(unrelated,"original.glb"));
    std::istringstream malformed("{\"beauty_workbench\":[1,]}");
    CHECK_THROWS(ModelLibraryMetadata::references_base(malformed,"original.glb"));
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
