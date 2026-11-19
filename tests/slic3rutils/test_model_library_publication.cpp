#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryApplication.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryPublication.hpp"
#include "slic3r/Utils/PlatformLibrary/LibraryClient.hpp"
#include "../test_utils.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace Slic3r::GUI;
namespace {
ModelLibraryCommit published(const boost::filesystem::path& root, const std::string& job) {
    ModelLibraryCommit result; result.entry.job_id = job; result.entry.model_path = root / (job + ".obj");
    result.entry.title = wxString::FromUTF8(job); result.record_sha256 = job; return result;
}
}

TEST_CASE("A saved model appears in an empty library before a rescan or thumbnail completes", "[ModelLibraryPublication][UiLifecycle]")
{
    ModelLibraryApplication library("root");
    library.replace({});
    const auto epoch = library.commit_revision();
    const auto event = library.commit(published("root", "new"));
    REQUIRE(event);
    REQUIRE(library.entries().size() == 1);
    CHECK(library.entries().front().job_id == "new");
    library.replace({}, library.removal_revision(), epoch);
    REQUIRE(library.entries().size() == 1);
    CHECK(library.entries().front().job_id == "new");
}

TEST_CASE("Duplicate publication and late scans cannot overwrite a newer saved version", "[ModelLibraryPublication]")
{
    ModelLibraryApplication library("root");
    auto original = published("root", "one");
    REQUIRE(library.commit(original));
    const auto scan_epoch = library.commit_revision();
    CHECK_FALSE(library.commit(original));
    CHECK(library.commit_revision() == scan_epoch);
    auto updated = original; updated.entry.title = "new version"; updated.record_sha256 = "second";
    REQUIRE(library.commit(updated));
    library.replace({original.entry}, library.removal_revision(), scan_epoch);
    REQUIRE(library.entries().size() == 1);
    CHECK(library.entries().front().title == "new version");
}

TEST_CASE("Deletion projection wins over a publication or a scan from an older epoch", "[ModelLibraryPublication][ModelLibraryRemoval]")
{
    ModelLibraryApplication library("root");
    auto removals = std::make_shared<ModelLibraryRemovalState>("root"); library.set_removal_state(removals);
    const auto model = published("root", "removed"); REQUIRE(library.commit(model));
    const auto scan_removal = library.removal_revision();
    const auto removal = removals->begin(model.entry); library.apply_removals();
    CHECK(library.entries().empty());
    CHECK_FALSE(library.commit(model));
    REQUIRE(removals->commit(removal.request)); REQUIRE(removals->finish(removal.request, true));
    library.replace({model.entry}, scan_removal, 0);
    CHECK(library.entries().empty());
}

TEST_CASE("Hidden consumers retain their browsing order while merging newer commits", "[ModelLibraryPublication]")
{
    ModelLibraryApplication library("root");
    auto old = published("root", "old"); old.entry.generated_at = 1;
    library.replace({old.entry});
    auto fresh = published("root", "new"); fresh.entry.generated_at = 2;
    const auto event = library.commit(fresh); REQUIRE(event); CHECK_FALSE(event->foreground);
    std::vector<ModelLibraryEntry> hidden {old.entry}; library.merge_commits(hidden, 0);
    REQUIRE(hidden.size() == 2); CHECK(hidden.front().job_id == "new"); CHECK(hidden.back().job_id == "old");
    CHECK(library.filter(ModelLibraryCategory::All, "old").size() == 1);
}

TEST_CASE("Atomic registration preserves original generation and import observations", "[ModelLibraryPublication][ModelLibraryMetadata]")
{
    ScopedTemporaryDir temporary("orca-library-publication"); const auto path = temporary.path() / "record.json";
    REQUIRE(publish_model_library_record(path, {{"schema_version",6},{"job_id","a"},{"generated_at",12},{"imported_at",31},{"print_feedback","success"}}));
    REQUIRE(publish_model_library_record(path, {{"schema_version",6},{"job_id","a"},{"generated_at",99},{"triangle_count",3}}));
    boost::filesystem::ifstream input(path); const auto result = nlohmann::json::parse(input);
    CHECK(result.at("generated_at") == 12); CHECK(result.at("imported_at") == 31); CHECK(result.at("print_feedback") == "success");
    CHECK(result.at("triangle_count") == 3);
    size_t count = 0; for (auto it = boost::filesystem::directory_iterator(temporary.path()); it != boost::filesystem::directory_iterator(); ++it) ++count;
    CHECK(count == 1);
}

TEST_CASE("Failed registration leaves the last record and certified results intact", "[ModelLibraryPublication]")
{
    ScopedTemporaryDir temporary("orca-library-publication-failure"); const auto path = temporary.path() / "record.json";
    REQUIRE(publish_model_library_record(path, {{"job_id","certified"},{"workbench_check_certificate",{{"proof","unchanged"}}}}));
    REQUIRE(publish_model_library_record(path, {{"job_id","replacement"}}));
    boost::filesystem::ifstream input(path); CHECK(nlohmann::json::parse(input).at("job_id") == "certified");
    CHECK_FALSE(publish_model_library_record(temporary.path() / "missing" / "record.json", {{"job_id","failed"}}));
    CHECK_FALSE(boost::filesystem::exists(temporary.path() / "missing"));
}

TEST_CASE("An older successful completion cannot replace the latest publication", "[ModelLibraryPublication]")
{
    ModelLibraryApplication library("root");
    auto older = published("root", "same"); older.request = 4;
    auto newer = older; newer.request = 5; newer.record_sha256 = "newer"; newer.entry.title = "latest";
    REQUIRE(library.commit(newer)); CHECK_FALSE(library.commit(older));
    REQUIRE(library.entries().size() == 1); CHECK(library.entries().front().title == "latest");
}

TEST_CASE("A durable saved record supplies the exact new asset to an empty history consumer", "[ModelLibraryPublication][UnifiedWorkbench]")
{
    ScopedTemporaryDir temporary("orca-library-publication-handoff");
    const auto root = temporary.path(), model = root / "new-task" / "model-vertex-color.obj";
    boost::filesystem::create_directories(model.parent_path());
    boost::filesystem::create_directory(root / "downloads");
    { boost::filesystem::ofstream output(model); output << "v 0 0 0\nv 20 0 0\nv 0 20 0\nv 0 0 20\nf 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n"; }
    const auto record = root / "downloads" / "orcaslicer-ai-new-task.json";
    REQUIRE(publish_model_library_record(record, {{"job_id","new-task"},{"source","local_import"},
        {"model_path","new-task/model-vertex-color.obj"},{"prompt","Committed model"},{"triangle_count",4}}));
    const std::atomic<bool> canceled {false};
    auto entries = ModelLibraryRepository::read(root,canceled,false,"new-task");
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().model_path == model);
    CHECK(entries.front().triangle_count == 4);
    ModelLibraryApplication library(root);
    const auto scan = library.commit_revision();
    ModelLibraryCommit event; event.entry = entries.front();
    event.record_sha256 = Slic3r::PlatformLibrary::file_digest(record);
    REQUIRE(library.commit(event));
    library.replace({},library.removal_revision(),scan);
    REQUIRE(library.entries().size() == 1);
    CHECK(library.entries().front().model_path == model);
    CHECK_FALSE(library.commit(event));
}

TEST_CASE("A failed atomic replacement removes its pending file and preserves the old destination", "[ModelLibraryPublication]")
{
    ScopedTemporaryDir temporary("orca-library-publication-replace-failure");
    const auto blocked = temporary.path() / "record.json";
    boost::filesystem::create_directory(blocked);
    { boost::filesystem::ofstream marker(blocked / "old-record"); marker << "preserved"; }
    CHECK_FALSE(publish_model_library_record(blocked,{{"job_id","cannot-replace"}}));
    boost::filesystem::ifstream marker(blocked / "old-record"); std::string value; marker >> value;
    CHECK(value == "preserved");
    size_t entries = 0;
    for (auto it = boost::filesystem::directory_iterator(temporary.path()); it != boost::filesystem::directory_iterator(); ++it) ++entries;
    CHECK(entries == 1);
}
