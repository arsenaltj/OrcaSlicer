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


TEST_CASE("History link I/O failure retracts committed metadata without changing prior assets",
    "[BeautyWorkbench][BeautyPersistence][PortraitSave][HistoryIndexFailure][P0]")
{
    struct Fixture {
        boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("beauty-index-io-%%%%-%%%%");
        Fixture() { boost::filesystem::create_directory(directory); }
        ~Fixture() { boost::system::error_code error; boost::filesystem::remove_all(directory, error); }
    } fixture;
    const bool directory_collision = GENERATE(false, true);
    const auto write = [](const auto& path, const std::string& bytes) {
        boost::filesystem::ofstream output(path, std::ios::binary);
        output << bytes; output.close();
        REQUIRE(output.good());
    };
    const auto read = [](const auto& path) {
        boost::filesystem::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto source = fixture.directory / "original.glb";
    const auto candidate = fixture.directory / "candidate.glb";
    const auto metadata_path = fixture.directory / "accepted.json";
    const auto index_path = fixture.directory / "accepted.history.json";
    const auto old_record = fixture.directory / "prior.json";
    const auto old_index = fixture.directory / "prior.history.json";
    const auto draft = fixture.directory / "original.json.draft";
    const auto sample = boost::filesystem::path(std::string(TEST_DATA_DIR)) / "model_artifact" / "textured.glb";
    boost::filesystem::copy_file(sample, source);
    boost::filesystem::copy_file(sample, candidate);
    const std::string old_record_bytes = "{\n  \"prior_version\": true\n}\n";
    const std::string old_index_bytes = "{ \"prior_index\": true }\n";
    const std::string draft_bytes = "unsaved user colors\n";
    const std::string collision_bytes = "existing concurrent owner\n";
    write(old_record, old_record_bytes); write(old_index, old_index_bytes); write(draft, draft_bytes);
    const auto source_hash = model_artifact_sha256(source);
    const auto candidate_hash = model_artifact_sha256(candidate);
    const nlohmann::json metadata {{"source_sha256", source_hash}, {"model_sha256", candidate_hash}};
    const nlohmann::json index {{"model_sha256", candidate_hash}, {"model_path", "candidate.glb"}};
    int checkpoints = 0;
    bool metadata_committed = false, index_temporary_written = false;
    CHECK_THROWS_AS(publish_beauty_version_record(metadata_path, candidate, source, metadata, nullptr,
        [&] {
            if (++checkpoints != 3) return false;
            // This is the real helper's final verify before the index link.
            // Its first metadata link has already been committed, and the
            // serialized index temporary has been written and closed.
            metadata_committed = boost::filesystem::is_regular_file(metadata_path) &&
                nlohmann::json::parse(read(metadata_path)) == metadata;
            size_t index_temporaries = 0;
            for (boost::filesystem::directory_iterator it(fixture.directory), end; it != end; ++it) {
                const auto name = it->path().filename().string();
                if (name.rfind("beauty-version-", 0) == 0 && it->path().extension() == ".tmp") {
                    ++index_temporaries;
                    index_temporary_written = nlohmann::json::parse(read(it->path())) == index;
                }
            }
            CHECK(index_temporaries == 1);
            if (directory_collision) {
                REQUIRE(boost::filesystem::create_directory(index_path));
                write(index_path / "owned.txt", collision_bytes);
            } else write(index_path, collision_bytes);
            // Do not cancel or throw: the production create_hard_link call
            // must encounter the actual filesystem collision and throw.
            return false;
        }, &index), boost::filesystem::filesystem_error);
    REQUIRE(checkpoints == 3);
    REQUIRE(metadata_committed); REQUIRE(index_temporary_written);
    CHECK_FALSE(boost::filesystem::exists(metadata_path));
    if (directory_collision) {
        REQUIRE(boost::filesystem::is_directory(index_path));
        CHECK(read(index_path / "owned.txt") == collision_bytes);
    } else CHECK(read(index_path) == collision_bytes);
    CHECK(read(old_record) == old_record_bytes); CHECK(read(old_index) == old_index_bytes);
    CHECK(read(draft) == draft_bytes);
    CHECK(model_artifact_sha256(source) == source_hash);
    CHECK(model_artifact_sha256(candidate) == candidate_hash);
    for (boost::filesystem::directory_iterator it(fixture.directory), end; it != end; ++it)
        CHECK(it->path().filename().string().rfind("beauty-version-", 0) != 0);
    // The collision belongs to this fixture, not to the failed publication.
    // Removing it makes a real retry succeed with the same untouched inputs.
    boost::filesystem::remove_all(index_path);
    REQUIRE_NOTHROW(publish_beauty_version_record(metadata_path, candidate, source,
        metadata, nullptr, {}, &index));
    CHECK(nlohmann::json::parse(read(metadata_path)) == metadata);
    CHECK(nlohmann::json::parse(read(index_path)) == index);
    CHECK(read(old_record) == old_record_bytes); CHECK(read(old_index) == old_index_bytes);
    CHECK(read(draft) == draft_bytes);
    CHECK(model_artifact_sha256(source) == source_hash);
    CHECK(model_artifact_sha256(candidate) == candidate_hash);
}
