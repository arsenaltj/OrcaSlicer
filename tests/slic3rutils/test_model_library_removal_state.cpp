#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryApplication.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchHistorySnapshot.hpp"
#include "slic3r/GUI/Presentation/UiRuntime.hpp"
#include "../test_utils.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace Slic3r::GUI;
namespace {
struct Fixture {
    ScopedTemporaryDir temporary {"orca-removal-projection"};
    boost::filesystem::path root {temporary.path()};
    std::shared_ptr<ModelLibraryRemovalState> removals {std::make_shared<ModelLibraryRemovalState>(root)};
    ModelLibraryApplication application {root};
    Fixture() { application.set_removal_state(removals); }
    ModelLibraryEntry entry(const std::string& id) const {
        ModelLibraryEntry entry; entry.job_id=id;entry.model_path=root/(id+".obj");entry.title="same title";return entry;
    }
};
}

TEST_CASE("Accepted removal updates the visible library in one GUI commit before filesystem completion", "[ModelLibraryRemoval][UiLifecycle]")
{
    Fixture f; const auto first=f.entry("first"),second=f.entry("second");
    f.application.replace({first,second});
    const auto old_token=f.application.token(0);
    std::vector<std::function<void()>> queued;
    Presentation::RefreshScheduler refresh([&](auto callback) {queued.push_back(std::move(callback));});
    size_t commits=0;
    refresh.configure([] {return true;},[&](auto) {++commits;f.application.apply_removals();},"removal-test");
    const auto removal=f.removals->begin(first);
    refresh.request(Presentation::Dirty::Content);refresh.request(Presentation::Dirty::Layout);
    REQUIRE(queued.size()==1);
    queued.front()();
    CHECK(commits==1);
    REQUIRE(f.application.entries().size()==1);
    CHECK(f.application.entries().front().job_id==second.job_id);
    CHECK_FALSE(f.application.find(old_token));
    CHECK_FALSE(f.removals->committed(removal.request));
    CHECK(f.removals->active());
}

TEST_CASE("Failed removal restores only its exclusion and preserves newer library records", "[ModelLibraryRemoval]")
{
    Fixture f;const auto first=f.entry("first"),second=f.entry("second"),newer=f.entry("newer");
    f.application.replace({first,second});
    const auto removal=f.removals->begin(first);f.application.apply_removals();
    f.application.replace({newer,first,second},f.removals->revision());
    REQUIRE(f.application.entries().size()==2);
    REQUIRE(f.removals->finish(removal.request,false));f.application.apply_removals();
    REQUIRE(f.application.entries().size()==3);
    CHECK(f.application.entries()[0].job_id==newer.job_id);
    CHECK(f.application.entries()[1].job_id==first.job_id);
    CHECK_FALSE(f.removals->active());
}

TEST_CASE("Committed removal survives cleanup failure and stale scans but permits explicit registration", "[ModelLibraryRemoval]")
{
    Fixture f;const auto entry=f.entry("selected");
    const auto stale_revision=f.removals->revision();
    const auto removal=f.removals->begin(entry);
    REQUIRE(f.removals->commit(removal.request));
    CHECK_FALSE(f.removals->commit(removal.request));
    REQUIRE(f.removals->finish(removal.request,false)); // Cleanup failed after durable commit.
    CHECK_FALSE(f.removals->finish(removal.request,false));
    CHECK_FALSE(f.removals->commit(removal.request));
    f.application.replace({entry},stale_revision);
    CHECK(f.application.entries().empty());
    f.application.replace({},f.removals->revision());
    CHECK(f.removals->blocked(entry));
    // Only a new authoritative receipt-filtered scan can acknowledge re-download.
    f.application.replace({entry},f.removals->revision());
    CHECK_FALSE(f.removals->blocked(entry));
    CHECK(f.application.entries().size()==1);
}

TEST_CASE("An older completion cannot clear a newer deletion or reinstall its asset", "[ModelLibraryRemoval]")
{
    Fixture f;const auto first=f.entry("first"),second=f.entry("second");
    const auto old=f.removals->begin(first);REQUIRE(f.removals->finish(old.request,false));
    const auto current=f.removals->begin(second);
    CHECK_FALSE(f.removals->commit(old.request));CHECK_FALSE(f.removals->finish(old.request,true));
    CHECK(f.removals->active());CHECK(f.removals->blocked(second));
    REQUIRE(f.removals->commit(current.request));REQUIRE(f.removals->finish(current.request,true));
    CHECK(f.removals->blocked(second));
}

TEST_CASE("Removal identity joins only registered task mirrors and normalized paths", "[ModelLibraryRemoval]")
{
    Fixture f;auto download=f.entry("job");download.model_path=f.root/"downloads/orcaslicer-ai-job.obj";
    auto task=download;task.model_path=f.root/"job/sub/../model-vertex-color.obj";
    CHECK(f.removals->same(download,task));
    auto other=task;other.model_path=f.root/"job/saved-version.obj";
    CHECK_FALSE(f.removals->same(download,other));
    other=task;other.job_id="other-job";
    CHECK_FALSE(f.removals->same(download,other));
    other=task;other.design_only=true;other.preview_path=task.model_path;
    CHECK_FALSE(f.removals->same(task,other));
#ifdef _WIN32
    other=task;other.model_path=boost::filesystem::path(f.root.wstring()+L"/JOB/MODEL-VERTEX-COLOR.OBJ");
    CHECK(f.removals->same(task,other));
#endif
}

TEST_CASE("Resolved history deletion invalidates its editor binding without claiming a different saved version", "[ModelLibraryRemoval][UnifiedWorkbench]")
{
    Fixture f;const auto selected=f.entry("original"),resolved=f.entry("repaired"),newer=f.entry("newer");
    WorkbenchLibraryBinding binding {selected,resolved};
    CHECK(binding.affected(*f.removals,selected,resolved));
    CHECK(binding.affected(*f.removals,resolved,resolved));
    CHECK_FALSE(binding.affected(*f.removals,selected,newer));
    CHECK_FALSE(binding.affected(*f.removals,newer,resolved));
}

TEST_CASE("Checked history removal recognizes the installed asset across its display and registration aliases", "[ModelLibraryRemoval][UnifiedWorkbench][NativeCheckedHistory]")
{
    Fixture f;
    const auto selected=f.entry("original"),registered=f.entry("native-v12-published"),saved=f.entry("saved-edit");
    auto displayed=registered;displayed.job_id="native-partition-digest";
    WorkbenchLibraryBinding binding {selected,registered};
    CHECK(binding.affected(*f.removals,selected,displayed));
    CHECK(binding.affected(*f.removals,registered,displayed));
    CHECK_FALSE(binding.affected(*f.removals,selected,saved));
    CHECK_FALSE(binding.affected(*f.removals,saved,displayed));
    CHECK_FALSE(f.removals->same(registered,displayed));
}

TEST_CASE("Automatic check publication carries its history selection through repeated asset handoffs", "[ModelLibraryRemoval][UnifiedWorkbench][NativeCheckedHistory]")
{
    Fixture f;
    const auto selected=f.entry("original"),checked=f.entry("native-check"),repaired=f.entry("repair"),saved=f.entry("saved-edit");
    WorkbenchLibraryBinding binding {selected,selected};
    binding.replace_checked_asset(*f.removals,selected,checked);
    CHECK(binding.affected(*f.removals,selected,checked));
    binding.replace_checked_asset(*f.removals,checked,repaired);
    CHECK(binding.affected(*f.removals,selected,repaired));
    CHECK_FALSE(binding.affected(*f.removals,selected,saved));
    binding={saved,saved}; // Explicit saved edits start their own selection.
    binding.replace_checked_asset(*f.removals,saved,checked);
    CHECK_FALSE(binding.affected(*f.removals,selected,checked));
    CHECK(binding.affected(*f.removals,saved,checked));
}

TEST_CASE("History fallback uses the same filtered order and moves forward before moving backward", "[ModelLibraryRemoval][UnifiedWorkbench]")
{
    Fixture f;auto a=f.entry("a"),b=f.entry("b"),c=f.entry("c"),design=f.entry("design");
    a.palette={"#000000"};b.palette={"#000000"};c.palette={"#000000","#ffffff"};design.design_only=true;
    const std::vector<ModelLibraryEntry> all {a,b,c,design};
    const auto filtered=filtered_workbench_history_entries(all,"",1);
    REQUIRE(filtered.size()==2);
    CHECK(filtered[0]->job_id==a.job_id);CHECK(filtered[1]->job_id==b.job_id);
    CHECK(workbench_history_fallback_keys({"a","b","c","d"},"b")==std::vector<std::string>{"c","d","a"});
    CHECK(workbench_history_fallback_keys({"a","b"},"b")==std::vector<std::string>{"a"});
    CHECK(workbench_history_fallback_keys({"a"},"a").empty());
    CHECK(workbench_history_fallback_keys({"a"},"missing").empty());
}

TEST_CASE("Shared history consumers suppress a late scan and restore failure without creating another session", "[ModelLibraryRemoval][UnifiedWorkbench]")
{
    Fixture f; const auto first=f.entry("first"),other=f.entry("other");
    ModelLibraryApplication observer(f.root);observer.set_removal_state(f.removals);
    f.application.replace({first,other});observer.replace({first,other});
    const auto scan_revision=f.removals->revision();
    const auto event=f.removals->begin(first);
    f.application.apply_removals();observer.apply_removals();
    CHECK(f.application.entries().size()==1);CHECK(observer.entries().size()==1);
    observer.replace({first,other},scan_revision);
    CHECK(observer.entries().size()==1);
    REQUIRE(f.removals->finish(event.request,false));
    f.application.apply_removals();observer.apply_removals();
    CHECK(f.application.entries().size()==2);CHECK(observer.entries().size()==2);
}
