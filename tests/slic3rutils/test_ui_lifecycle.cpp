#include "slic3r/GUI/Presentation/WindowMountLease.hpp"
#include <wx/frame.h>
#include <wx/panel.h>
#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/Presentation/UiRuntime.hpp"
#include "slic3r/GUI/Presentation/AsyncPreviewImage.hpp"
#include "slic3r/GUI/Presentation/WindowCloseController.hpp"
#include "slic3r/GUI/Redesign/RedesignPageCoordinator.hpp"
#include <wx/init.h>
#include <wx/app.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <stdexcept>
#include <future>
#include <wx/filename.h>
#include <wx/filefn.h>
#include <wx/log.h>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Presentation;

// Catch supplies main; wxInitialize still needs a real GUI application when
// exercising hidden native windows rather than wxAppConsole's fallback.
class LifecycleTestApp : public wxApp {
public:
    bool OnInit() override { return true; }
};
wxIMPLEMENT_APP_NO_MAIN(LifecycleTestApp);

TEST_CASE("Deferred close confirms once and rejects duplicate requests until presentation drains", "[UiLifecycle][CloseAdmission]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    auto* frame = new wxFrame(nullptr, wxID_ANY, "close-test");
    int confirmations = 0, accepted = 0, finished = 0, drain_calls = 0;
    bool allow = false, ready = false;
    std::function<void()> complete;
    {
        WindowCloseController close(*frame, [] { return true; }, [] { return true; },
            [&] { ++accepted; }, [&](std::function<void()> done) {
                ++drain_calls;
                if (ready) return true;
                complete = std::move(done);
                return false;
            });
        close.set_confirmations({{}, [&] { ++confirmations; return allow; }, {}});
        frame->Bind(wxEVT_CLOSE_WINDOW, [&](wxCloseEvent& event) {
            if (close.request_close(event)) ++finished;
        });
        frame->Close(false);
        CHECK(confirmations == 1);
        CHECK(accepted == 0);
        CHECK(drain_calls == 0);
        allow = true;
        frame->Close(false);
        CHECK(confirmations == 2);
        CHECK(accepted == 1);
        CHECK(close.phase() == WindowCloseController::Phase::Presentation);
        frame->Close(false);
        CHECK(drain_calls == 1);
        CHECK(finished == 0);
        ready = true;
        REQUIRE(bool(complete));
        complete();
        CHECK(confirmations == 2);
        CHECK(accepted == 1);
        CHECK(drain_calls == 2);
        CHECK(finished == 1);
        CHECK(close.phase() == WindowCloseController::Phase::ShuttingDown);
        frame->Close(false);
        CHECK(finished == 1);
    }
    delete frame;
}

TEST_CASE("Close drain callbacks cannot reenter a released controller", "[UiLifecycle][CloseAdmission]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    auto* frame = new wxFrame(nullptr, wxID_ANY, "released-close-test");
    std::function<void()> complete;
    {
        WindowCloseController close(*frame, [] { return true; }, [] { return true; }, [] {},
            [&](std::function<void()> done) { complete = std::move(done); return false; });
        wxCloseEvent event(wxEVT_CLOSE_WINDOW);
        event.SetCanVeto(true);
        CHECK_FALSE(close.request_close(event));
        CHECK(event.GetVeto());
    }
    REQUIRE(bool(complete));
    complete();
    delete frame;
}

TEST_CASE("Preview image preparation keeps aspect ratio cover alpha and cancellation off the GUI thread", "[UiLifecycle][PreviewImage]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG)) wxInitAllImageHandlers();
    const auto name=wxFileName::CreateTempFileName("orca-preview-image-");
    struct Remove { wxString name; ~Remove() { wxRemoveFile(name); } } cleanup{name};
    wxImage source(80,160);
    source.SetRGB(wxRect(0,0,80,160),20,100,200);
    REQUIRE(source.SaveFile(name,wxBITMAP_TYPE_PNG));
    PreviewImageRequest request;
    request.path=std::string(name.ToUTF8()); request.revision="revision-a"; request.bounds={40,40};
    auto future=std::async(std::launch::async,[request] {
        return prepare_preview_image(request,[] { return false; });
    });
    auto image=future.get();
    REQUIRE(image.IsOk());
    REQUIRE(image.GetWidth()==20);
    REQUIRE(image.GetHeight()==40);
    REQUIRE(image.GetBlue(10,10)==200);
    request.cover=true; request.radius=8;
    auto covered=prepare_preview_image(request,[] { return false; });
    REQUIRE(covered.GetWidth()==40);
    REQUIRE(covered.GetHeight()==40);
    REQUIRE(covered.HasAlpha());
    REQUIRE(covered.GetAlpha(0,0)==0);
    REQUIRE(covered.GetAlpha(20,20)==255);
    REQUIRE_FALSE(prepare_preview_image(request,[] { return true; }).IsOk());
}


TEST_CASE("Cancellation retains request ownership until its completion drains", "[UiLifecycle]")
{
    TaskScope scope;
    const auto canceled = scope.begin("asset-a", TaskLifetime::Transaction);
    scope.cancel();
    CHECK(scope.owns(canceled));
    CHECK_FALSE(scope.matches(canceled));
    const auto replacement = scope.begin("asset-a", TaskLifetime::Transaction);
    CHECK_FALSE(scope.owns(canceled));
    CHECK(scope.matches(replacement));
}
namespace {
class Mailbox {
public:
    void post(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(mutex); callbacks.push_back(std::move(callback)); changed.notify_all();
    }
    std::size_t size() { std::lock_guard<std::mutex> lock(mutex); return callbacks.size(); }
    bool wait() {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::seconds(5),[&] { return !callbacks.empty(); });
    }
    bool pump() {
        std::function<void()> callback;
        { std::unique_lock<std::mutex> lock(mutex);
          if (!changed.wait_for(lock, std::chrono::milliseconds(10), [&] { return !callbacks.empty(); })) return false;
          callback = std::move(callbacks.front()); callbacks.pop_front(); }
        callback(); return true;
    }
    bool until(const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!done() && std::chrono::steady_clock::now() < deadline) pump();
        return done();
    }
    Dispatcher dispatcher() { return [this](auto callback) { post(std::move(callback)); }; }
private:
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::function<void()>> callbacks;
};
}

TEST_CASE("Image channels reject stale completions and keep unchanged bitmaps across movement and hiding", "[UiLifecycle][PreviewImage]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG)) wxInitAllImageHandlers();
    const auto first=wxFileName::CreateTempFileName("orca-image-first-");
    const auto second=wxFileName::CreateTempFileName("orca-image-second-");
    const auto delayed=wxFileName::CreateTempFileName("orca-image-delayed-");
    struct Remove { wxString a,b,c; ~Remove() { wxRemoveFile(a); wxRemoveFile(b); wxRemoveFile(c); } } cleanup{first,second,delayed};
    wxImage blue(80,160); blue.SetRGB(wxRect(0,0,80,160),0,0,255);
    wxImage red(160,80); red.SetRGB(wxRect(0,0,160,80),255,0,0);
    REQUIRE(blue.SaveFile(first,wxBITMAP_TYPE_PNG));
    REQUIRE(red.SaveFile(second,wxBITMAP_TYPE_PNG));
    wxFrame frame(nullptr,wxID_ANY,"Preview lifecycle test",wxPoint(-32000,-32000),wxSize(100,100));
    frame.ShowWithoutActivating(); // Tool-owned, offscreen; never the user's main window.
    Mailbox mailbox;
    TaskRunner tasks(mailbox.dispatcher());
    AsyncPreviewImage channel(&frame,tasks);
    PreviewImageRequest key{std::string(first.ToUTF8()),"a",{40,40}};
    channel.request(key);
    REQUIRE(mailbox.wait());
    key.path=std::string(second.ToUTF8()); key.revision="b";
    channel.request(key); // First CPU result is already queued, but has not installed.
    REQUIRE(mailbox.until([&] { return tasks.idle(); }));
    REQUIRE(channel.bitmap().IsOk());
    REQUIRE(channel.bitmap().GetWidth()==40);
    REQUIRE(channel.bitmap().GetHeight()==20);
    REQUIRE(channel.bitmap().ConvertToImage().GetRed(5,5)==255);
    for (int i=0;i<20;++i) channel.request(key);
    REQUIRE(tasks.idle());
    REQUIRE(mailbox.size()==0);
    channel.set_active(false);
    key.bounds={60,60}; channel.request(key);
    REQUIRE(tasks.idle());
    REQUIRE(channel.bitmap().GetWidth()==40);
    channel.set_active(true);
    REQUIRE(mailbox.until([&] { return tasks.idle(); }));
    REQUIRE(channel.bitmap().GetWidth()==60);
    // The same path can be replaced explicitly: its resource revision, rather
    // than another size event or a paint, invalidates the installed bitmap.
    red.SetRGB(wxRect(0,0,160,80),0,255,0);
    REQUIRE(red.SaveFile(second,wxBITMAP_TYPE_PNG));
    key.revision="new-resource";
    channel.request(key);
    REQUIRE(channel.bitmap().ConvertToImage().GetRed(5,5)==255);
    REQUIRE(mailbox.until([&] { return tasks.idle(); }));
    REQUIRE(channel.bitmap().ConvertToImage().GetGreen(5,5)==255);
    // The upload path can arrive before its cached bytes. A failed preparation
    // retains the old complete image and accepts the same key once bytes arrive.
    key.path=std::string(delayed.ToUTF8()); key.revision="deferred-cache";
    {
        struct QuietDecoder {
            wxLogLevel previous {wxLog::GetLogLevel()};
            QuietDecoder() { wxLog::SetLogLevel(wxLOG_FatalError); }
            ~QuietDecoder() { wxLog::SetLogLevel(previous); }
        } expected_decode_failure;
        channel.request(key);
        REQUIRE(mailbox.until([&] { return tasks.idle(); }));
    }
    REQUIRE(channel.bitmap().ConvertToImage().GetGreen(5,5)==255);
    REQUIRE(blue.SaveFile(delayed,wxBITMAP_TYPE_PNG));
    channel.request(key);
    REQUIRE(mailbox.until([&] { return tasks.idle(); }));
    REQUIRE(channel.bitmap().ConvertToImage().GetBlue(5,5)==255);
    key.dpi=144;
    channel.request(key);
    REQUIRE_FALSE(tasks.idle());
    REQUIRE(mailbox.until([&] { return tasks.idle(); }));
    frame.Hide();
}

TEST_CASE("A commit retains invalidation raised during layout", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    RefreshScheduler scheduler(mailbox.dispatcher());
    int commits = 0;
    std::vector<Dirty> observed;
    scheduler.configure([] { return true; }, [&](Dirty flags) {
        observed.push_back(flags);
        if (++commits == 1) scheduler.request(Dirty::Overlay);
    }, "test");
    for (int i = 0; i < 20; ++i) scheduler.request(Dirty::Content);
    scheduler.request(Dirty::Layout);
    REQUIRE(mailbox.size() == 1);
    REQUIRE(mailbox.pump());
    REQUIRE(commits == 1);
    REQUIRE(has(observed.front(), Dirty::Layout));
    REQUIRE(mailbox.size() == 1);
    REQUIRE(mailbox.pump());
    REQUIRE(commits == 2);
    REQUIRE(observed.back() == Dirty::Overlay);
    REQUIRE(mailbox.size() == 0);
}

TEST_CASE("Hidden surfaces retain dirty state and disposed surfaces ignore callbacks", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    bool visible = false;
    int commits = 0;
    RefreshScheduler scheduler(mailbox.dispatcher());
    scheduler.configure([&] { return visible; }, [&](Dirty) { ++commits; }, "test");
    for (int i = 0; i < 20; ++i) scheduler.request(Dirty::Paint);
    REQUIRE(commits == 0);
    REQUIRE(mailbox.size() == 0);
    visible = true;
    scheduler.resume();
    REQUIRE(mailbox.pump());
    REQUIRE(commits == 1);
    scheduler.request(Dirty::Layout);
    scheduler.stop();
    REQUIRE(mailbox.pump());
    REQUIRE(commits == 1);
}

TEST_CASE("A restore barrier consumes its pending native repaint only once", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    RefreshScheduler scheduler(mailbox.dispatcher());
    int commits = 0;
    bool barrier = false;
    scheduler.configure([] { return true; }, [&](Dirty flags) {
        ++commits; barrier = has(flags, Dirty::Barrier);
        if (commits == 1) scheduler.request(Dirty::Content);
    }, "restore");
    scheduler.request(Dirty::Layout);
    scheduler.flush();
    REQUIRE(commits == 1);
    REQUIRE(barrier);
    REQUIRE(mailbox.size() == 1);
    REQUIRE(mailbox.pump());
    REQUIRE(commits == 2);
    REQUIRE_FALSE(barrier);
    REQUIRE(mailbox.size() == 0);
}

TEST_CASE("Native snapshots survive hidden routes and minimization without painting the old page", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    RedesignPageCoordinator pages;
    using Page = RedesignWorkspacePage;
    using View = RedesignModelView;
    pages.register_page(Page::Image, View::Result, false, {});
    pages.register_page(Page::Model, View::Slicing, false, {});
    pages.select_page(Page::Image); pages.presented_current();
    bool minimized = false;
    int latest = 0, displayed = 0, commits = 0;
    RefreshScheduler scheduler(mailbox.dispatcher());
    scheduler.configure([&] {
        return !minimized && pages.page() == Page::Model && pages.view() == View::Slicing;
    }, [&](Dirty) { displayed = latest; ++commits; pages.presented_current(); }, "native");
    for (int i = 1; i <= 20; ++i) { latest = i; scheduler.request(Dirty::Content); }
    REQUIRE(mailbox.size() == 0);
    REQUIRE(displayed == 0);
    pages.select_page(Page::Model); pages.select_view(View::Slicing);
    scheduler.resume();
    REQUIRE(mailbox.pump());
    REQUIRE(displayed == 20);
    REQUIRE(commits == 1);

    latest = 21; scheduler.request(Dirty::Content);
    pages.select_page(Page::Image); pages.presented_current();
    REQUIRE(mailbox.pump());
    REQUIRE(displayed == 20);
    REQUIRE(commits == 1);
    pages.select_page(Page::Model);
    minimized = true; scheduler.resume();
    REQUIRE(mailbox.size() == 0);
    minimized = false; scheduler.resume();
    REQUIRE(mailbox.pump());
    REQUIRE(displayed == 21);
    REQUIRE(commits == 2);
    REQUIRE(pages.lifetime(Page::Model, View::Slicing) == RedesignPageCoordinator::Lifetime::Visible);
}

TEST_CASE("Abandoning a prepared page preserves the retained source and its session", "[UiLifecycle]")
{
    RedesignPageCoordinator coordinator;
    using Page = RedesignWorkspacePage;
    using View = RedesignModelView;
    int hidden = 0;
    coordinator.register_page(Page::Image, View::Result, false, {});
    RedesignPageCoordinator::Adapter target;
    target.hidden = [&] { ++hidden; };
    coordinator.register_page(Page::Model, View::Result, false, target);
    coordinator.select_page(Page::Image); coordinator.presented_current();
    coordinator.select_page(Page::Model); coordinator.prepare_current();
    coordinator.select_page(Page::Image);
    REQUIRE(hidden == 1);
    REQUIRE(coordinator.lifetime(Page::Image) == RedesignPageCoordinator::Lifetime::Visible);
    REQUIRE(coordinator.lifetime(Page::Model) == RedesignPageCoordinator::Lifetime::Hidden);
}

TEST_CASE("Leaving a page does not invalidate an independent generation session", "[UiLifecycle]")
{
    TaskScope view, generation;
    const auto old_view = view.begin("model-a");
    const auto job = generation.begin("advanced-job", TaskLifetime::Session, 9);
    const auto next = view.begin("model-b");
    REQUIRE(old_view.canceled());
    REQUIRE_FALSE(view.matches(old_view));
    REQUIRE(view.matches(next));
    REQUIRE(generation.matches(job));
    REQUIRE_FALSE(job.canceled());
    REQUIRE(next.owner == old_view.owner);
    REQUIRE(job.owner != next.owner);
}

TEST_CASE("Retained page adapters activate once and preserve their own mode", "[UiLifecycle]")
{
    RedesignPageCoordinator coordinator;
    using Page = RedesignWorkspacePage;
    using View = RedesignModelView;
    int smart = 0, advanced = 0, hidden = 0, disposed = 0;
    RedesignPageCoordinator::Adapter adapter;
    adapter.presented = [&] { ++smart; };
    adapter.hidden = [&] { ++hidden; };
    adapter.dispose = [&] { ++disposed; };
    coordinator.register_page(Page::Image, View::Result, false, adapter);
    adapter.presented = [&] { ++advanced; };
    coordinator.register_page(Page::Image, View::Result, true, adapter);
    coordinator.select_page(Page::Image);
    for (int i = 0; i < 20; ++i) {
        coordinator.select_mode(false); coordinator.prepare_current(); coordinator.presented_current();
        coordinator.presented_current(); // A same-size redraw is not another activation.
        coordinator.select_mode(true); coordinator.prepare_current(); coordinator.presented_current();
    }
    REQUIRE(smart == 20);
    REQUIRE(advanced == 20);
    REQUIRE(hidden == 39);
    coordinator.dispose(); coordinator.dispose();
    REQUIRE(disposed == 2);
    REQUIRE(hidden == 40);
}

TEST_CASE("A stale mount restore cannot release a newer owner", "[UiLifecycle]")
{
    MountGeneration lease;
    const auto old = lease.acquire();
    const auto current = lease.acquire();
    REQUIRE_FALSE(lease.release(old));
    REQUIRE(lease.current(current));
    REQUIRE(lease.release(current));
    REQUIRE_FALSE(lease.release(current));
}

TEST_CASE("Progress coalescing always delivers phase changes and terminal results", "[UiLifecycle]")
{
    ProgressGate gate;
    const auto start = ProgressGate::Clock::now();
    REQUIRE(gate.accept(1, false, start));
    REQUIRE_FALSE(gate.accept(1, false, start + std::chrono::milliseconds(20)));
    REQUIRE(gate.accept(2, false, start + std::chrono::milliseconds(21)));
    REQUIRE(gate.accept(2, true, start + std::chrono::milliseconds(22)));
    REQUIRE(gate.accept(2, false, start + std::chrono::milliseconds(122)));
}

TEST_CASE("Queued preview replacement drains old work before installing the newest result", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    TaskRunner runner(mailbox.dispatcher());
    TaskScope scope;
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false, release = false;
    std::atomic<int> prepared {0};
    int installed = 0, finished = 0;
    const auto first = scope.begin("a");
    REQUIRE(runner.submit(first, [&](const TaskContext&) {
        std::unique_lock<std::mutex> lock(mutex); started = true; changed.notify_all();
        changed.wait(lock, [&] { return release; }); ++prepared; return 1;
    }, [&](TaskResult<int>) { if (scope.matches(first)) ++installed; ++finished; }));
    bool observed;
    { std::unique_lock<std::mutex> lock(mutex);
      observed = changed.wait_for(lock, std::chrono::seconds(3), [&] { return started; }); }
    const auto second = scope.begin("b");
    REQUIRE(runner.submit(second, [&](const TaskContext&) { ++prepared; return 2; },
        [&](TaskResult<int>) { if (scope.matches(second)) ++installed; ++finished; }));
    const auto third = scope.begin("c");
    REQUIRE(runner.submit(third, [&](const TaskContext&) { ++prepared; return 3; },
        [&](TaskResult<int> result) { if (scope.matches(third) && result.value) installed += *result.value; ++finished; }));
    { std::lock_guard<std::mutex> lock(mutex); release = true; } changed.notify_all();
    REQUIRE(observed);
    REQUIRE(mailbox.until([&] { return finished == 3; }));
    REQUIRE(prepared == 2); // The canceled middle model is never parsed.
    REQUIRE(installed == 3);
    REQUIRE(runner.idle());
}

TEST_CASE("Shutdown drains transaction completion while canceling view work", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    TaskRunner runner(mailbox.dispatcher());
    TaskScope transaction, view;
    const auto save = transaction.begin("save", TaskLifetime::Transaction);
    const auto preview = view.begin("preview");
    int publications = 0, completed = 0;
    bool drained = false;
    REQUIRE(runner.submit(save, [](const TaskContext&) { return 42; }, [&](TaskResult<int> value) {
        if (value.value) publications += *value.value; ++completed;
    }));
    REQUIRE(runner.submit(preview, [](const TaskContext&) { return 1; }, [&](TaskResult<int>) { ++completed; }));
    REQUIRE_FALSE(runner.request_shutdown([&] { drained = true; }));
    REQUIRE(preview.canceled());
    REQUIRE_FALSE(save.canceled());
    REQUIRE(mailbox.until([&] { return drained; }));
    REQUIRE(publications == 42);
    REQUIRE(completed == 2);
    REQUIRE(runner.idle());
    REQUIRE_FALSE(runner.submit(save, [](const TaskContext&) { return 1; }, [](TaskResult<int>) {}));
}


TEST_CASE("Cancel wakes a low priority request paused by slicing", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    TaskRunner runner(mailbox.dispatcher());
    TaskScope scope;
    runner.set_native_busy(true);
    bool ran = false, canceled = false;
    const auto task = scope.begin("thumbnail");
    REQUIRE(runner.submit(task, [&](const TaskContext&) { ran = true; return true; },
        [&](TaskResult<bool> value) { canceled = value.canceled; }, TaskPriority::Low));
    scope.cancel();
    REQUIRE(mailbox.until([&] { return canceled; }));
    REQUIRE_FALSE(ran);
    REQUIRE(runner.idle());
}

TEST_CASE("Shutdown retires a retained worker without waiting on the GUI", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    TaskRunner runner(mailbox.dispatcher());
    std::mutex mutex;
    std::condition_variable changed;
    bool release = false;
    std::thread legacy([&] {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return release; });
    });
    int hooks = 0, completed = 0;
    bool accepted_new_work = true;
    bool drained = false;
    runner.on_shutdown(&legacy, [&] {
        ++hooks;
        accepted_new_work = runner.submit(TaskContext{}, [](const TaskContext&) { return 1; }, [](TaskResult<int>) {});
        runner.retire(legacy, [&] { ++completed; });
    });
    const bool immediate = runner.request_shutdown([&] { drained = true; });
    const bool transferred = !legacy.joinable();
    { std::lock_guard<std::mutex> lock(mutex); release = true; } changed.notify_all();
    REQUIRE_FALSE(immediate);
    REQUIRE(transferred);
    REQUIRE(mailbox.until([&] { return drained; }));
    REQUIRE(hooks == 1);
    REQUIRE_FALSE(accepted_new_work);
    REQUIRE(completed == 1);
    REQUIRE(runner.request_shutdown({}));
    REQUIRE(hooks == 1);
}

TEST_CASE("A replacement queue never parses superseded view requests", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    TaskRunner runner(mailbox.dispatcher());
    TaskScope scope;
    runner.set_native_busy(true);
    std::atomic<int> parsed {0};
    int completed = 0;
    for (int i=0; i<100; ++i) {
        const auto request = scope.begin(std::to_string(i));
        REQUIRE(runner.submit(request, [&](const TaskContext&) { ++parsed; return true; },
            [&](TaskResult<bool>) { ++completed; }, TaskPriority::Low));
        // Consume canceled handoffs in bounded batches, as the GUI does. A
        // separate regression deliberately stops consumption to test refusal.
        if (i % 16 == 15) REQUIRE(mailbox.until([&] { return completed >= i; }));
    }
    runner.set_native_busy(false);
    REQUIRE(mailbox.until([&] { return completed == 100; }));
    REQUIRE(parsed == 1);
    REQUIRE(runner.idle());
}

TEST_CASE("Page rollback retains the visible source and releases abandoned preparation", "[UiLifecycle]")
{
    RedesignPageCoordinator pages;
    using Page = RedesignWorkspacePage;
    using View = RedesignModelView;
    int source_hidden = 0, target_hidden = 0;
    RedesignPageCoordinator::Adapter source, target;
    source.hidden = [&] { ++source_hidden; };
    target.hidden = [&] { ++target_hidden; };
    pages.register_page(Page::Image, View::Result, false, source);
    pages.register_page(Page::Model, View::Workbench, false, target);
    pages.select_page(Page::Image); pages.prepare_current(); pages.presented_current();
    pages.select_page(Page::Model); pages.select_view(View::Workbench); pages.prepare_current();
    REQUIRE(source_hidden == 0);
    pages.select_page(Page::Image); pages.prepare_current(); pages.presented_current();
    REQUIRE(source_hidden == 0);
    REQUIRE(target_hidden == 1);
    REQUIRE(pages.lifetime(Page::Model, View::Workbench) == RedesignPageCoordinator::Lifetime::Hidden);
}

TEST_CASE("Unscoped queued tasks observe shutdown cancellation before executing", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    TaskRunner runner(mailbox.dispatcher());
    runner.set_native_busy(true);
    bool ran = false, completed = false, canceled = false;
    REQUIRE(runner.submit(TaskContext{}, [&](const TaskContext&) { ran = true; return 1; },
        [&](TaskResult<int> result) { completed = true; canceled = result.canceled; }, TaskPriority::Low));
    runner.stop();
    REQUIRE(mailbox.until([&] { return completed; }));
    REQUIRE_FALSE(ran);
    REQUIRE(canceled);
    REQUIRE(runner.idle());
}

TEST_CASE("Stopping workers still joins queued legacy drains", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    TaskRunner runner(mailbox.dispatcher());
    std::mutex mutex;
    std::condition_variable changed;
    int started = 0, completed = 0;
    for (int i = 0; i < 2; ++i) {
        REQUIRE(runner.submit(TaskContext{}, [&](const TaskContext& context) {
            { std::lock_guard<std::mutex> lock(mutex); ++started; changed.notify_all(); }
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!context.canceled() && std::chrono::steady_clock::now() < end) std::this_thread::yield();
            return context.canceled();
        }, [&](TaskResult<bool>) { ++completed; }));
    }
    bool both_started;
    { std::unique_lock<std::mutex> lock(mutex);
      both_started = changed.wait_for(lock, std::chrono::seconds(3), [&] { return started == 2; }); }
    std::atomic_bool exited {false};
    std::thread legacy([&] { exited = true; });
    const bool accepted = runner.retire(legacy, [&] { ++completed; });
    runner.stop();
    // Keep the fixture safe even if a future implementation refuses admission.
    if (legacy.joinable()) legacy.join();
    REQUIRE(both_started);
    REQUIRE(accepted);
    REQUIRE(exited.load());
    REQUIRE(mailbox.until([&] { return completed == 3; }));
    REQUIRE(runner.idle());
}

TEST_CASE("A background task cannot operate a GUI presentation entry", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    bool rejected = false;
    std::thread worker([&] {
        try { assert_gui_thread(); }
        catch (const std::logic_error&) { rejected = true; }
    });
    worker.join();
    REQUIRE(rejected);
}

TEST_CASE("A rejected replacement retains cancellation delivery for queued work", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    TaskRunner runner(mailbox.dispatcher());
    std::mutex mutex;
    std::condition_variable changed;
    int running = 0, completed = 0;
    bool release = false;
    for (int i = 0; i < 2; ++i) {
        REQUIRE(runner.submit(TaskContext{}, [&](const TaskContext&) {
            std::unique_lock<std::mutex> lock(mutex);
            ++running; changed.notify_all();
            changed.wait_for(lock, std::chrono::seconds(5), [&] { return release; });
            return 1;
        }, [&](TaskResult<int>) { ++completed; }));
    }
    bool started;
    { std::unique_lock<std::mutex> lock(mutex);
      started = changed.wait_for(lock, std::chrono::seconds(3), [&] { return running == 2; }); }
    TaskContext old;
    old.owner = std::make_shared<const char>(0);
    old.cancellation = std::make_shared<std::atomic_bool>(false);
    const bool old_accepted = runner.submit(old, [](const TaskContext&) { return 1; },
        [&](TaskResult<int>) { ++completed; }, TaskPriority::Low);
    bool queued = true;
    for (int i = 0; i < 63; ++i) queued = runner.submit(TaskContext{}, [](const TaskContext&) { return 1; },
        [&](TaskResult<int>) { ++completed; }, TaskPriority::Low) && queued;
    std::thread legacy([] {});
    const bool retired = runner.retire(legacy, [&] { ++completed; });
    TaskContext next;
    next.owner = old.owner;
    const bool accepted = runner.submit(next, [](const TaskContext&) { return 2; }, [](TaskResult<int>) {}, TaskPriority::Low);
    { std::lock_guard<std::mutex> lock(mutex); release = true; } changed.notify_all();
    if (legacy.joinable()) legacy.join();
    REQUIRE(started);
    REQUIRE(old_accepted);
    REQUIRE(queued);
    REQUIRE(retired);
    REQUIRE_FALSE(accepted);
    REQUIRE_FALSE(old.canceled());
    REQUIRE(mailbox.until([&] { return completed == 67; }));
    REQUIRE(runner.idle());
}

TEST_CASE("Shutdown persists its final transaction before releasing the owner", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    TaskRunner runner(mailbox.dispatcher());
    TaskScope scope;
    bool accepted = false, ordinary_accepted = true, committed = false, drained = false;
    runner.on_shutdown(&scope, [&] {
        const auto snapshot = scope.begin("final-draft", TaskLifetime::Transaction);
        ordinary_accepted = runner.submit(snapshot, [](const TaskContext&) { return 1; }, [](TaskResult<int>) {});
        accepted = runner.submit_finalizer(snapshot, [](const TaskContext&) { return 42; },
            [&](TaskResult<int> result) { committed = result.value && *result.value == 42 && !result.canceled; });
    });
    REQUIRE_FALSE(runner.request_shutdown([&] { drained = true; }));
    REQUIRE_FALSE(ordinary_accepted);
    REQUIRE(accepted);
    REQUIRE(mailbox.until([&] { return drained; }));
    REQUIRE(committed);
    REQUIRE(runner.idle());
    REQUIRE_FALSE(runner.submit_finalizer(scope.begin("late", TaskLifetime::Transaction),
        [](const TaskContext&) { return 1; }, [](TaskResult<int>) {}));
}

TEST_CASE("The latest durable snapshot survives shutdown without publishing superseded drafts", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    TaskRunner runner(mailbox.dispatcher());
    TaskScope scope;
    runner.set_native_busy(true);
    std::atomic<int> written {0};
    int completed = 0;
    for (int i = 1; i <= 20; ++i) {
        auto snapshot = scope.begin("draft", TaskLifetime::Transaction);
        snapshot.coalesce_pending = true;
        REQUIRE(runner.submit(snapshot, [&, i](const TaskContext&) { written = i; return i; },
            [&](TaskResult<int>) { ++completed; }, TaskPriority::Low));
    }
    bool drained = false;
    REQUIRE_FALSE(runner.request_shutdown([&] { drained = true; }));
    REQUIRE(mailbox.until([&] { return drained; }));
    REQUIRE(written == 20);
    REQUIRE(completed == 20);
    REQUIRE(runner.idle());
}

TEST_CASE("Replacing queued previews applies backpressure to undelivered cancellation results", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    Mailbox mailbox;
    TaskRunner runner(mailbox.dispatcher());
    std::mutex mutex;
    std::condition_variable changed;
    int running = 0, completed = 0;
    bool release = false;
    for (int i = 0; i < 2; ++i) {
        REQUIRE(runner.submit(TaskContext{}, [&](const TaskContext&) {
            std::unique_lock<std::mutex> lock(mutex);
            ++running; changed.notify_all();
            changed.wait_for(lock, std::chrono::seconds(5), [&] { return release; });
            return 0;
        }, [&](TaskResult<int>) { ++completed; }));
    }
    bool started;
    { std::unique_lock<std::mutex> lock(mutex);
      started = changed.wait_for(lock, std::chrono::seconds(3), [&] { return running == 2; }); }
    const auto owner = std::make_shared<const char>(0);
    int accepted = 0, rejected = 0, parsed = 0;
    for (int i = 0; i < 200; ++i) {
        TaskContext context; context.owner = owner;
        if (runner.submit(context, [&](const TaskContext&) { ++parsed; return 1; },
                [&](TaskResult<int>) { ++completed; }, TaskPriority::Low)) ++accepted;
        else ++rejected;
    }
    { std::lock_guard<std::mutex> lock(mutex); release = true; } changed.notify_all();
    REQUIRE(started);
    CHECK(accepted > 0);
    CHECK(accepted < 200);
    CHECK(rejected > 0);
    REQUIRE(mailbox.until([&] { return completed == accepted + 2; }));
    CHECK(parsed == 1);
    CHECK(runner.idle());
}

TEST_CASE("A shared native window returns only through its current mount lease", "[UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    auto home = std::make_unique<wxFrame>(nullptr, wxID_ANY, "home");
    auto target = std::make_unique<wxFrame>(nullptr, wxID_ANY, "target");
    auto nested_target = std::make_unique<wxFrame>(nullptr, wxID_ANY, "nested");
    home->SetSizer(new wxBoxSizer(wxHORIZONTAL));
    target->SetSizer(new wxBoxSizer(wxHORIZONTAL));
    nested_target->SetSizer(new wxBoxSizer(wxHORIZONTAL));
    auto* before = new wxPanel(home.get());
    auto* window = new wxPanel(home.get());
    home->GetSizer()->Add(before);
    home->GetSizer()->Add(window, 2, wxEXPAND | wxALL, 3);
    window->Hide();
    WindowMountLease lease, nested;
    const auto token = lease.mount(window, target.get());
    REQUIRE(token.has_value());
    CHECK(window->GetParent() == target.get());
    REQUIRE(lease.mount(window, target.get()) == token);
    CHECK(target->GetSizer()->GetItemCount() == 1);
    const auto newer = nested.mount(window, nested_target.get());
    REQUIRE(newer.has_value());
    CHECK_FALSE(lease.restore(*token));
    CHECK(window->GetParent() == nested_target.get());
    REQUIRE(nested.restore(*newer));
    REQUIRE(lease.restore(*token));
    CHECK(window->GetParent() == home.get());
    CHECK_FALSE(window->IsShown());
    auto* item = home->GetSizer()->GetItem(size_t(1));
    REQUIRE(item);
    CHECK(item->GetWindow() == window);
    CHECK(item->GetProportion() == 2);
    CHECK(item->GetBorder() == 3);
    CHECK_FALSE(lease.restore(*token));

    const auto remount = lease.mount(window, target.get());
    REQUIRE(remount.has_value());
    home->SetSizer(new wxBoxSizer(wxVERTICAL)); // The retained parent rebuilt its sizer.
    REQUIRE(lease.restore(*remount));
    CHECK(home->GetSizer()->GetItemCount() == 1);
    CHECK(home->GetSizer()->GetItem(size_t(0))->GetWindow() == window);
    const auto last = lease.mount(window, target.get());
    REQUIRE(last.has_value());
    home.reset();
    CHECK_FALSE(lease.restore(*last));
    CHECK_FALSE(lease.active());
    CHECK(window->GetParent() == target.get());
}
