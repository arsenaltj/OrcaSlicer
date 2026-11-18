#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryDeletion.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryRepository.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryMetadata.hpp"
#include "slic3r/GUI/Presentation/UiRuntime.hpp"
#include "test_utils.hpp"
#include <boost/filesystem/fstream.hpp>
#include <wx/init.h>
#include <wx/imagpng.h>
#include <wx/wfstream.h>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

using namespace Slic3r::GUI;
using Json = nlohmann::json;
namespace {
struct Library {
    ScopedTemporaryDir temporary {"orca-library-removal"};
    boost::filesystem::path root {temporary.path()};
    Library() { boost::filesystem::create_directories(root / "downloads"); }
    void write(const boost::filesystem::path& path, const std::string& value) {
        boost::filesystem::create_directories(path.parent_path());
        boost::filesystem::ofstream file(path, std::ios::binary); file << value;
    }
    void preview(const boost::filesystem::path& path) {
        boost::filesystem::create_directories(path.parent_path());
        wxImage image(2, 2, true);
        wxPNGHandler png;
        wxFileOutputStream output(path.wstring());
        REQUIRE(png.SaveFile(&image, output, false));
    }
    ModelLibraryEntry entry(const std::string& id, bool metadata = true) {
        ModelLibraryEntry e;
        e.job_id = id; e.model_path = root / "downloads" / ("orcaslicer-ai-" + id + ".obj");
        write(e.model_path, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        if (metadata) write(record(e), Json {{"job_id",id}, {"model_path",e.model_path.lexically_relative(root).generic_string()}}.dump());
        return e;
    }
    boost::filesystem::path record(const ModelLibraryEntry& e) const {
        return root / "downloads" / ("orcaslicer-ai-" + e.job_id + ".json");
    }
    std::vector<ModelLibraryEntry> read() {
        const std::atomic<bool> canceled {false};
        return ModelLibraryRepository::read(root, canceled);
    }
};
}

TEST_CASE("Legacy models without metadata can be deleted and stay removed", "[ModelLibraryDeletion]")
{
    Library library;
    const auto entry = library.entry("legacy", false);
    REQUIRE(library.read().size() == 1);
    const auto result = ModelLibraryDeletion::remove(library.root, entry, {entry});
    REQUIRE(result.committed);
    CHECK(result.error.empty());
    CHECK_FALSE(result.retained_dependencies);
    CHECK_FALSE(boost::filesystem::exists(entry.model_path));
    CHECK(library.read().empty());
    CHECK(ModelLibraryDeletion::removed(library.root, entry.job_id, entry.model_path));
    const auto repeated = ModelLibraryDeletion::remove(library.root, entry, {});
    CHECK(repeated.committed);
    CHECK(repeated.removed_files == 0);
}

TEST_CASE("Removing a referenced history card preserves its exact model and provenance", "[ModelLibraryDeletion]")
{
    Library library;
    const auto original = library.entry("original");
    const auto version = library.entry("finish-version");
    library.write(library.record(version), Json {{"job_id",version.job_id}, {"source","local_finishing"},
        {"model_path",version.model_path.lexically_relative(library.root).generic_string()},
        {"source_model",original.model_path.generic_string()}}.dump());
    const auto size = boost::filesystem::file_size(original.model_path);
    const auto metadata_size = boost::filesystem::file_size(library.record(original));
    const auto result = ModelLibraryDeletion::remove(library.root, original, {original, version});
    REQUIRE(result.committed);
    CHECK(result.retained_dependencies);
    CHECK(result.removed_files == 0);
    CHECK(boost::filesystem::file_size(original.model_path) == size);
    CHECK(boost::filesystem::file_size(library.record(original)) == metadata_size);
    const auto reloaded = library.read();
    REQUIRE(reloaded.size() == 1);
    CHECK(reloaded.front().job_id == version.job_id);
    CHECK(ModelLibraryDeletion::removed(library.root, original.job_id, original.model_path));
}

TEST_CASE("Uncertain unrelated metadata retains bytes without blocking history removal", "[ModelLibraryDeletion]")
{
    Library library;
    const auto entry = library.entry("selected");
    library.write(library.root / "downloads/broken.json", "{broken");
    const auto result = ModelLibraryDeletion::remove(library.root, entry, {entry});
    REQUIRE(result.committed);
    CHECK(result.retained_dependencies);
    CHECK(boost::filesystem::exists(entry.model_path));
    CHECK(boost::filesystem::exists(library.record(entry)));
    CHECK(library.read().empty());
}

TEST_CASE("Cancellation before removal commits neither receipts nor filesystem changes", "[ModelLibraryDeletion]")
{
    Library library;
    const auto entry = library.entry("canceled");
    const auto result = ModelLibraryDeletion::remove(library.root, entry, {entry}, [] { return true; });
    CHECK(result.canceled);
    CHECK_FALSE(result.committed);
    CHECK(boost::filesystem::exists(entry.model_path));
    CHECK(boost::filesystem::exists(library.record(entry)));
    CHECK_FALSE(ModelLibraryDeletion::removed(library.root, entry.job_id, entry.model_path));
    CHECK(library.read().size() == 1);
}

TEST_CASE("Removing one model never recursively erases sibling assets", "[ModelLibraryDeletion]")
{
    Library library;
    const auto entry = library.entry("one");
    const auto sibling = library.entry("two");
    library.write(library.root / "downloads/unrelated.texture", "texture");
    const auto result = ModelLibraryDeletion::remove(library.root, entry, {entry, sibling});
    REQUIRE(result.committed);
    CHECK_FALSE(boost::filesystem::exists(entry.model_path));
    CHECK(boost::filesystem::exists(sibling.model_path));
    CHECK(boost::filesystem::exists(library.root / "downloads/unrelated.texture"));
    REQUIRE(library.read().size() == 1);
    CHECK(library.read().front().job_id == sibling.job_id);
}

TEST_CASE("Deletion rejects path escapes before committing a receipt", "[ModelLibraryDeletion]")
{
    Library library;
    auto entry = library.entry("inside");
    entry.model_path = library.root / "../outside.obj";
    auto result = ModelLibraryDeletion::remove(library.root, entry, {});
    CHECK_FALSE(result.committed);
    CHECK_FALSE(result.error.empty());
    entry.job_id = "../outside";
    result = ModelLibraryDeletion::remove(library.root, entry, {});
    CHECK_FALSE(result.committed);
}

TEST_CASE("Removal receipts match the selected resource and do not hide new identities", "[ModelLibraryDeletion]")
{
    Library library;
    const auto entry = library.entry("first");
    REQUIRE(ModelLibraryDeletion::remove(library.root, entry, {entry}).committed);
    CHECK_FALSE(ModelLibraryDeletion::removed(library.root, entry.job_id, library.root / "other.obj"));
    CHECK_FALSE(ModelLibraryDeletion::removed(library.root, "different", entry.model_path));
    CHECK_FALSE(ModelLibraryDeletion::removed(library.root, entry.job_id, entry.model_path, true));
}

TEST_CASE("Files changed during dependency inspection cannot be removed by a stale selection", "[ModelLibraryDeletion]")
{
    Library library;
    const auto entry = library.entry("changed");
    library.entry("other");
    bool changed = false;
    const auto result = ModelLibraryDeletion::remove(library.root, entry, {entry}, [&] {
        if (!changed) {
            changed = true;
            library.write(entry.model_path, "externally replaced model data");
        }
        return false;
    });
    REQUIRE(changed);
    CHECK_FALSE(result.committed);
    CHECK_FALSE(result.canceled);
    CHECK_FALSE(result.error.empty());
    CHECK(boost::filesystem::exists(entry.model_path));
    CHECK_FALSE(ModelLibraryDeletion::removed(library.root, entry.job_id, entry.model_path));
    CHECK(library.read().size() == 2);
}

TEST_CASE("A removal receipt write failure cannot remove the model", "[ModelLibraryDeletion]")
{
    Library library;
    const auto entry = library.entry("blocked");
    auto receipt = library.record(entry); receipt.replace_extension(".removed");
    boost::filesystem::create_directories(receipt);
    const auto result = ModelLibraryDeletion::remove(library.root, entry, {entry});
    CHECK_FALSE(result.committed);
    CHECK_FALSE(result.error.empty());
    CHECK(boost::filesystem::exists(entry.model_path));
    CHECK(boost::filesystem::exists(library.record(entry)));
}

TEST_CASE("Deletion preserves borrowed files in a model directory", "[ModelLibraryDeletion]")
{
    Library library;
    auto entry = library.entry("directory");
    const auto old = entry.model_path;
    entry.model_path = library.root / entry.job_id / "model.glb";
    boost::filesystem::create_directories(entry.model_path.parent_path());
    boost::filesystem::rename(old, entry.model_path);
    const auto neighbor = library.entry("neighbor");
    const auto borrowed = entry.model_path.parent_path() / "borrowed.obj";
    library.write(borrowed, "shared appearance");
    library.write(library.record(neighbor), Json {{"model_path",neighbor.model_path.generic_string()},
        {"source_model",borrowed.generic_string()}}.dump());
    const auto result = ModelLibraryDeletion::remove(library.root, entry, {entry, neighbor});
    REQUIRE(result.committed);
    CHECK_FALSE(boost::filesystem::exists(entry.model_path));
    CHECK(boost::filesystem::exists(borrowed));
    CHECK(boost::filesystem::exists(neighbor.model_path));
}

TEST_CASE("Deleting a generated model cannot resurface its incomplete design as another card", "[ModelLibraryDeletion]")
{
    Library library;
    const std::string id = "12345678-1234-1234-1234-123456789abc";
    auto entry = library.entry(id);
    const auto old = entry.model_path;
    entry.model_path = library.root / id / "model-vertex-color.obj";
    boost::filesystem::create_directories(entry.model_path.parent_path());
    boost::filesystem::rename(old, entry.model_path);
    const auto state = entry.model_path.parent_path() / "job.json";
    library.write(state, Json{{"version",1}, {"id",id}, {"source","text"},
        {"state","stopped"}, {"preview_path","preview.png"}}.dump());
    library.preview(entry.model_path.parent_path() / "preview.png");
    REQUIRE(library.read().size() == 1);
    REQUIRE_FALSE(library.read().front().design_only);
    const auto result = ModelLibraryDeletion::remove(library.root, entry, {entry});
    REQUIRE(result.committed);
    CHECK_FALSE(boost::filesystem::exists(state));
    CHECK(library.read().empty());
}

TEST_CASE("Design only history uses the same deletion transaction and remains removed", "[ModelLibraryDeletion]")
{
    Library library;
    const std::string id = "87654321-1234-1234-1234-123456789abc";
    const auto parent = library.root / id;
    library.write(parent / "job.json", Json{{"version",1}, {"id",id}, {"source","text"},
        {"state","failed"}, {"preview_path","preview.png"}}.dump());
    library.preview(parent / "preview.png");
    const auto entries = library.read();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries.front().design_only);
    const auto result = ModelLibraryDeletion::remove(library.root, entries.front(), entries);
    REQUIRE(result.committed);
    CHECK(result.error.empty());
    CHECK(library.read().empty());
    CHECK(ModelLibraryDeletion::removed(library.root, id, parent / "preview.png", true));
}

TEST_CASE("Deletion preserves Unicode model paths and handles externally missing files", "[ModelLibraryDeletion]")
{
    Library library;
    library.root /= boost::filesystem::path(std::filesystem::u8path(u8"模型库副本").native());
    boost::filesystem::create_directories(library.root / "downloads");
    const auto entry = library.entry("unicode");
    REQUIRE(library.read().size() == 1);
    const auto result = ModelLibraryDeletion::remove(library.root, entry, {entry});
    REQUIRE(result.committed);
    CHECK(result.error.empty());
    CHECK(library.read().empty());
    const auto missing = library.entry("missing");
    REQUIRE(boost::filesystem::remove(missing.model_path));
    const auto stale = ModelLibraryDeletion::remove(library.root, missing, {missing});
    REQUIRE(stale.committed);
    CHECK(stale.error.empty());
    CHECK(ModelLibraryDeletion::removed(library.root, missing.job_id, missing.model_path));
}

TEST_CASE("Deleting a hard linked history card preserves other visible model identities", "[ModelLibraryDeletion]")
{
    Library library;
    const auto original = library.entry("original");
    auto alias = library.entry("alias");
    REQUIRE(boost::filesystem::remove(alias.model_path));
    boost::system::error_code error;
    boost::filesystem::create_hard_link(original.model_path, alias.model_path, error);
    if (error) SKIP("The temporary filesystem does not support hard links");
    const auto result = ModelLibraryDeletion::remove(library.root, original, {original, alias});
    REQUIRE(result.committed);
    CHECK(result.retained_dependencies);
    CHECK(boost::filesystem::exists(original.model_path));
    CHECK(boost::filesystem::exists(alias.model_path));
    const auto entries = library.read();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().job_id == alias.job_id);
}

TEST_CASE("Deletion work runs off the GUI thread and hands back one completed transaction", "[ModelLibraryDeletion]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Library library;
    const auto entry = library.entry("asynchronous");
    std::mutex mutex;
    std::condition_variable ready;
    std::function<void()> callback;
    using namespace Slic3r::GUI::Presentation;
    TaskRunner runner([&](std::function<void()> complete) {
        std::lock_guard<std::mutex> lock(mutex);
        callback = std::move(complete);
        ready.notify_all();
    });
    const auto gui_thread = std::this_thread::get_id();
    std::thread::id worker_thread, commit_thread;
    int completions = 0;
    std::optional<ModelLibraryDeletion::Result> outcome;
    TaskScope scope;
    const auto context = scope.begin(entry.job_id, TaskLifetime::Transaction);
    REQUIRE(runner.submit(context, [&](const TaskContext& task) {
        worker_thread = std::this_thread::get_id();
        return ModelLibraryDeletion::remove(library.root, entry, {entry}, [&] { return task.canceled(); });
    }, [&](TaskResult<ModelLibraryDeletion::Result> result) {
        commit_thread = std::this_thread::get_id();
        ++completions;
        outcome = std::move(result.value);
    }));
    std::function<void()> commit;
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(ready.wait_for(lock, std::chrono::seconds(5), [&] { return bool(callback); }));
        commit = std::move(callback);
    }
    CHECK(completions == 0);
    CHECK_FALSE(runner.idle());
    commit();
    CHECK(worker_thread != gui_thread);
    CHECK(commit_thread == gui_thread);
    CHECK(completions == 1);
    REQUIRE(outcome);
    CHECK(outcome->committed);
    CHECK(outcome->error.empty());
    CHECK(runner.idle());
    CHECK(library.read().empty());
}

TEST_CASE("Durable removal is announced before byte cleanup and never before commit", "[ModelLibraryDeletion][ModelLibraryRemoval]")
{
    Library library; const auto entry=library.entry("commit-event");
    size_t commits=0;bool receipt_visible=false,bytes_retained=false;
    const auto result=ModelLibraryDeletion::remove(library.root,entry,{entry},{},[&] {
        ++commits;receipt_visible=ModelLibraryDeletion::removed(library.root,entry.job_id,entry.model_path);
        bytes_retained=boost::filesystem::exists(entry.model_path);
    });
    REQUIRE(result.committed);CHECK(commits==1);CHECK(receipt_visible);CHECK(bytes_retained);
    CHECK_FALSE(boost::filesystem::exists(entry.model_path));
    const auto canceled=library.entry("not-committed");
    const auto refused=ModelLibraryDeletion::remove(library.root,canceled,{canceled},[] {return true;},[&] {++commits;});
    CHECK_FALSE(refused.committed);CHECK(commits==1);
}

TEST_CASE("A task mirror cannot restore a removed history item after a restart", "[ModelLibraryDeletion][ModelLibraryRemoval]")
{
    Library library;const auto entry=library.entry("mirror");
    const auto task=library.root/entry.job_id/"model-vertex-color.obj";
    library.write(task,"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    auto dependent=entry;dependent.job_id="consumer";
    REQUIRE(ModelLibraryDeletion::remove(library.root,entry,{entry,dependent}).committed);
    REQUIRE(boost::filesystem::exists(entry.model_path));
    CHECK(ModelLibraryDeletion::removed(library.root,entry.job_id,task));
    CHECK(library.read().empty());
}
