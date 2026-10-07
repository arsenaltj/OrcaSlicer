#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"
#include <boost/filesystem/fstream.hpp>

using namespace Slic3r::AI;

TEST_CASE("Beauty documents retain regions only for the matching geometry", "[BeautyWorkbench]")
{
    BeautyDocument document;
    document.geometry_id = "geometry-a";
    document.face_count = 4;
    document.face_patch = {0, 0, 1, 1};
    document.add_group("eyes", {0, 1});

    const auto encoded = document.encode();
    const auto reopened = BeautyDocument::decode(encoded, "geometry-a", 4);
    CHECK(reopened.geometry_id == document.geometry_id);
    CHECK(reopened.groups.size() == 1);
    CHECK(reopened.groups.front().faces == std::vector<size_t>{0, 1});
    CHECK_THROWS(BeautyDocument::decode(encoded, "geometry-b", 4));
    CHECK_THROWS(BeautyDocument::decode(encoded, "geometry-a", 3));
}

TEST_CASE("Beauty version publication rejects changed files and preserves existing records", "[BeautyWorkbench]")
{
    struct Fixture {
        boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("beauty-publication-%%%%-%%%%");
        Fixture() { boost::filesystem::create_directory(directory); }
        ~Fixture() { boost::system::error_code error; boost::filesystem::remove_all(directory, error); }
    } fixture;
    const auto source = fixture.directory / "source.glb";
    const auto model = fixture.directory / "candidate.glb";
    const auto history = fixture.directory / "accepted.json";
    const auto sample = boost::filesystem::path(std::string(TEST_DATA_DIR)) / "model_artifact" / "textured.glb";
    boost::filesystem::copy_file(sample, source);
    boost::filesystem::copy_file(sample, model);
    const nlohmann::json metadata = {{"source_sha256", model_artifact_sha256(source)},
        {"model_sha256", model_artifact_sha256(model)}};
    auto wrong = metadata;
    wrong["model_sha256"] = std::string(64, 'a');
    CHECK_THROWS(publish_beauty_version_record(history, model, source, wrong));
    CHECK_FALSE(boost::filesystem::exists(history));
    REQUIRE_NOTHROW(publish_beauty_version_record(history, model, source, metadata));
    const auto saved = model_artifact_sha256(history);
    CHECK_THROWS(publish_beauty_version_record(history, model, source, metadata));
    CHECK(model_artifact_sha256(history) == saved);
    { boost::filesystem::ofstream output(source, std::ios::app | std::ios::binary); output << "changed"; }
    CHECK_THROWS(publish_beauty_version_record(fixture.directory / "stale.json", model, source, metadata));
    CHECK_FALSE(boost::filesystem::exists(fixture.directory / "stale.json"));
    CHECK(model_artifact_sha256(history) == saved);
}

TEST_CASE("Canceled publication removes incomplete records and retains model files", "[BeautyWorkbench]")
{
    struct Fixture {
        boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("beauty-cancel-%%%%-%%%%");
        Fixture() { boost::filesystem::create_directory(directory); }
        ~Fixture() { boost::system::error_code error; boost::filesystem::remove_all(directory, error); }
    } fixture;
    const auto model = boost::filesystem::path(std::string(TEST_DATA_DIR)) / "model_artifact" / "textured.glb";
    const auto history = fixture.directory / "accepted.json";
    const auto index_path = fixture.directory / "accepted.history.json";
    const auto hash = model_artifact_sha256(model);
    const nlohmann::json metadata = {{"source_sha256", hash}, {"model_sha256", hash}};
    const nlohmann::json index = {{"model_sha256", hash}, {"model_path", "textured.glb"}};
    int checkpoints = 0;
    CHECK_THROWS(publish_beauty_version_record(history, model, model, metadata, nullptr,
        [&] { return ++checkpoints == 4; }, &index));
    CHECK_FALSE(boost::filesystem::exists(history));
    CHECK_FALSE(boost::filesystem::exists(index_path));
    CHECK(model_artifact_sha256(model) == hash);
    REQUIRE_NOTHROW(publish_beauty_version_record(history, model, model, metadata, nullptr, {}, &index));
    const auto saved_record = model_artifact_sha256(history);
    const auto saved_index = model_artifact_sha256(index_path);
    CHECK_THROWS(publish_beauty_version_record(history, model, model, metadata, nullptr, {}, &index));
    CHECK(model_artifact_sha256(history) == saved_record);
    CHECK(model_artifact_sha256(index_path) == saved_index);
}
