#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchSession.hpp"

// Exercise the production session with no preview, wx types or native project.
namespace {
using Controller = Slic3r::GUI::BeautyWorkbenchTransactionController;
using Path = boost::filesystem::path;
struct Snapshot {
    Path candidate;
    int version {0};
};
using Session = Slic3r::GUI::BeautyWorkbenchSession<Snapshot>;

struct Workbench {
    Session session;
    int visible {0}, restores {0};
    bool valid {true}, restore_succeeds {true};
    std::vector<Path> removed;

    Session::Ports ports() {
        return {[this](const auto&) { return valid; },
            [this](const auto& snapshot) {
                ++restores;
                if (!restore_succeeds) return false;
                visible = snapshot.version;
                return true;
            }, [this](const auto& path) { removed.push_back(path); }};
    }
    std::shared_ptr<Snapshot> snapshot(int version, Path file = {}) {
        return std::make_shared<Snapshot>(Snapshot {std::move(file), version});
    }
    Controller::Entry accepted(int before, int after) {
        const auto restore = ports().restore;
        return {Controller::OperationKind::AcceptCandidate, "accepted", {}, {}, {}, {},
            [restore, before] { return restore(Snapshot {{}, before}); },
            [restore, after] { return restore(Snapshot {{}, after}); }};
    }
    void preview(int before, int after, const Path& file) {
        session.transactions().finish(true, true);
        session.record(Controller::OperationKind::AppearanceRecolor,
            snapshot(before), snapshot(after, file), ports());
        visible = after;
    }
};
}

TEST_CASE("Discard restores the draft before removing its candidates and retains earlier history", "[BeautyWorkbenchSession]")
{
    Workbench workbench;
    auto& session = workbench.session;
    session.transactions().record(workbench.accepted(0, 1));
    session.track_candidate("earlier.glb");
    session.start_draft(workbench.snapshot(1));
    workbench.preview(1, 2, "candidate.glb");
    session.track_candidate("candidate.source-appearance.obj");
    auto ports = workbench.ports();
    std::vector<int> visible_at_removal;
    ports.remove_candidate = [&](const auto& path) {
        workbench.removed.push_back(path);
        visible_at_removal.push_back(workbench.visible);
    };
    REQUIRE(session.discard({}, ports));
    CHECK(workbench.visible == 1);
    CHECK(workbench.removed == std::vector<Path> {"candidate.glb", "candidate.source-appearance.obj"});
    CHECK(visible_at_removal == std::vector<int> {1, 1});
    CHECK_FALSE(session.source());
    CHECK(session.transactions().undo_count() == 1);
    CHECK(session.transactions().redo_count() == 0);
    REQUIRE(session.transactions().undo());
    CHECK(workbench.visible == 0);
}

TEST_CASE("Asynchronous discard preserves its draft and files until a verified restore succeeds", "[BeautyWorkbenchSession][UiLifecycle]")
{
    Workbench workbench;
    auto& session = workbench.session;
    session.start_draft(workbench.snapshot(1));
    session.track_candidate("candidate.glb");
    auto ports = workbench.ports();
    std::function<void(bool)> pending;
    ports.restore_async = [&](const auto& source, auto complete) {
        CHECK(source.version == 1);
        pending = std::move(complete);
    };
    bool restored = true;
    session.discard_async({}, ports, [&](bool ok) { restored = ok; });
    CHECK(session.transactions().processing());
    CHECK(session.source());
    CHECK(workbench.removed.empty());
    REQUIRE(pending);
    pending(false);
    CHECK_FALSE(restored);
    CHECK(session.source());
    CHECK(workbench.removed.empty());
    session.discard_async({}, ports, [&](bool ok) { restored = ok; });
    pending(true);
    pending(true);
    CHECK(restored);
    CHECK_FALSE(session.source());
    REQUIRE(workbench.removed.size() == 1);
    CHECK(workbench.removed.front() == Path("candidate.glb"));
    CHECK_FALSE(session.transactions().processing());
}

TEST_CASE("A replacement draft rejects late asynchronous discard cleanup", "[BeautyWorkbenchSession][UiLifecycle]")
{
    Workbench workbench;
    auto& session = workbench.session;
    session.start_draft(workbench.snapshot(1));
    session.track_candidate("first.glb");
    auto ports = workbench.ports();
    std::function<void(bool)> pending;
    ports.restore_async = [&](const auto&, auto complete) { pending = std::move(complete); };
    int callbacks = 0;
    session.discard_async({}, ports, [&](bool) { ++callbacks; });
    session.reset();
    session.start_draft(workbench.snapshot(3));
    session.track_candidate("second.glb");
    pending(true);
    REQUIRE(session.source());
    CHECK(session.source()->version == 3);
    CHECK(callbacks == 0);
    CHECK(workbench.removed.empty());
}

TEST_CASE("Failed draft restoration retains candidates and both history timelines for retry", "[BeautyWorkbenchSession]")
{
    const bool invalid_source = GENERATE(true, false);
    Workbench workbench;
    auto& session = workbench.session;
    const auto source = workbench.snapshot(0);
    session.start_draft(source);
    workbench.preview(0, 1, "first.glb");
    workbench.preview(1, 2, "second.glb");
    REQUIRE(session.transactions().undo());
    const auto revision = session.revision();
    workbench.valid = !invalid_source;
    workbench.restore_succeeds = invalid_source;
    CHECK_FALSE(session.discard({}, workbench.ports()));
    CHECK(session.source() == source);
    CHECK(session.revision() == revision);
    CHECK(workbench.visible == 1);
    CHECK(workbench.removed.empty());
    CHECK(session.transactions().undo_count() == 1);
    CHECK(session.transactions().redo_count() == 1);
    workbench.valid = workbench.restore_succeeds = true;
    REQUIRE(session.discard({}, workbench.ports()));
    CHECK(workbench.visible == 0);
    CHECK(workbench.removed == std::vector<Path> {"first.glb", "second.glb"});
    CHECK(session.transactions().undo_count() == 0);
    CHECK(session.transactions().redo_count() == 0);
}

TEST_CASE("A running operation prevents discard and file cleanup", "[BeautyWorkbenchSession]")
{
    Workbench workbench;
    auto& session = workbench.session;
    session.start_draft(workbench.snapshot(0));
    workbench.preview(0, 1, "candidate.glb");
    REQUIRE(session.transactions().begin(Controller::OperationKind::AcceptCandidate));
    CHECK_FALSE(session.discard({}, workbench.ports()));
    CHECK(workbench.restores == 0);
    CHECK(workbench.removed.empty());
    CHECK(session.source());
    CHECK(session.transactions().undo_count() == 1);
}

TEST_CASE("Accepting a recolored version replaces draft edits with one reversible history entry", "[BeautyWorkbenchSession]")
{
    Workbench workbench;
    auto& session = workbench.session;
    session.transactions().record(workbench.accepted(0, 1));
    session.start_draft(workbench.snapshot(1));
    const auto revision = session.revision();
    workbench.preview(1, 2, "first.glb");
    workbench.preview(2, 3, "accepted.glb");
    session.track_candidate("appearance.obj");
    session.draft().manual_color_dirty = true;
    REQUIRE(session.transactions().begin(Controller::OperationKind::AcceptCandidate));
    REQUIRE(session.complete_accept(revision, "accepted.glb", "appearance.obj", workbench.accepted(1, 3)));
    CHECK_FALSE(session.source());
    CHECK_FALSE(session.draft().manual_color_dirty);
    CHECK(session.transactions().undo_count() == 2);
    CHECK_FALSE(session.transactions().processing());
    REQUIRE(session.transactions().undo());
    CHECK(workbench.visible == 1);
    REQUIRE(session.transactions().undo());
    CHECK(workbench.visible == 0);
    REQUIRE(session.transactions().redo());
    CHECK(workbench.visible == 1);
    REQUIRE(session.transactions().redo());
    CHECK(workbench.visible == 3);
    session.clear_candidates(0, {}, workbench.ports());
    CHECK(workbench.removed == std::vector<Path> {"first.glb"});
}

TEST_CASE("An accepted version remains undoable after a failed restore and a later discard", "[BeautyWorkbenchSession]")
{
    Workbench workbench;
    auto& session = workbench.session;
    session.start_draft(workbench.snapshot(0));
    workbench.preview(0, 1, "accepted.glb");
    REQUIRE(session.complete_accept(session.revision(), "accepted.glb", {}, workbench.accepted(0, 1)));
    workbench.restore_succeeds = false;
    CHECK_FALSE(session.transactions().undo());
    CHECK(session.transactions().undo_count() == 1);
    CHECK(session.transactions().redo_count() == 0);
    CHECK(workbench.visible == 1);
    workbench.restore_succeeds = true;
    session.start_draft(workbench.snapshot(1));
    workbench.preview(1, 2, "pending.glb");
    REQUIRE(session.discard({}, workbench.ports()));
    REQUIRE(session.transactions().undo());
    CHECK(workbench.visible == 0);
    CHECK(workbench.removed == std::vector<Path> {"pending.glb"});
}

TEST_CASE("Canceled or failed publication keeps the same draft available for retry", "[BeautyWorkbenchSession]")
{
    const bool canceled = GENERATE(true, false);
    Workbench workbench;
    auto& session = workbench.session;
    session.start_draft(workbench.snapshot(0));
    workbench.preview(0, 1, "candidate.glb");
    const auto revision = session.revision();
    REQUIRE(session.transactions().begin(Controller::OperationKind::AcceptCandidate));
    if (canceled) session.transactions().request_cancel();
    session.transactions().finish(false, false, canceled ? "canceled" : "disk full");
    CHECK(session.source());
    CHECK(session.is_current(revision));
    CHECK(workbench.removed.empty());
    CHECK(session.transactions().undo_count() == 1);
    REQUIRE(session.transactions().begin(Controller::OperationKind::AcceptCandidate));
    REQUIRE(session.complete_accept(revision, "candidate.glb", {}, workbench.accepted(0, 1)));
    CHECK(session.transactions().undo_count() == 1);
    REQUIRE(session.transactions().undo());
    CHECK(workbench.visible == 0);
}

TEST_CASE("Obsolete publication cannot accept a replacement draft or publish twice", "[BeautyWorkbenchSession]")
{
    const int change = GENERATE(0, 1, 2, 3, 4);
    Workbench workbench;
    auto& session = workbench.session;
    const auto source = workbench.snapshot(0);
    session.start_draft(source);
    const auto revision = session.revision();
    if (change == 0) session.reset();
    if (change == 1) session.clear_draft();
    if (change == 2) {
        session.start_draft(workbench.snapshot(2));
        session.start_draft(source); // Switching away and back still invalidates the result.
    }
    if (change == 3) REQUIRE(session.discard({}, workbench.ports()));
    if (change == 4) REQUIRE(session.complete_accept(revision, "already.glb", {}, workbench.accepted(0, 1)));
    const auto current = session.source();
    const auto history = session.transactions().undo_count();
    session.track_candidate("obsolete.glb");
    CHECK_FALSE(session.complete_accept(revision, "obsolete.glb", {}, workbench.accepted(0, 9)));
    CHECK(session.source() == current);
    CHECK(session.transactions().undo_count() == history);
    session.clear_candidates(0, {}, workbench.ports());
    CHECK(workbench.removed == std::vector<Path> {"obsolete.glb"});
}

TEST_CASE("Publication completed before shutdown retains its model and owned appearance carrier", "[BeautyWorkbenchSession]")
{
    Workbench workbench;
    auto& session = workbench.session;
    for (const auto* path : {"published.glb", "appearance.obj", "temporary.glb"}) session.track_candidate(path);
    session.preserve_publication("published.glb", "appearance.obj");
    session.clear_candidates(0, {}, workbench.ports());
    CHECK(workbench.removed == std::vector<Path> {"temporary.glb"});
    // Referencing a carrier does not confer ownership over a later draft.
    session.preserve_publication("other.glb", "external.obj");
    session.track_candidate("external.obj");
    session.clear_candidates(0, {}, workbench.ports());
    CHECK(workbench.removed == std::vector<Path> {"temporary.glb", "external.obj"});
}

TEST_CASE("A failed asynchronous rollback retains its source and releases the busy gate for retry", "[BeautyWorkbenchSession][UiLifecycle]")
{
    Workbench workbench;
    auto& session = workbench.session;
    session.start_draft(workbench.snapshot(0));
    workbench.preview(0, 1, "pending.glb");
    const auto source = session.source();
    const auto revision = session.revision();
    auto ports = workbench.ports();
    std::function<void(bool)> restore;
    ports.restore_async = [&](const auto&, auto complete) { restore = std::move(complete); };
    int callbacks = 0;
    bool restored = true;
    session.discard_async({}, ports, [&](bool ok) { ++callbacks; restored = ok; });
    REQUIRE(bool(restore));
    CHECK(session.transactions().processing());
    CHECK(workbench.removed.empty());
    CHECK(session.source() == source);
    CHECK_FALSE(session.transactions().begin(Controller::OperationKind::AppearanceRecolor));
    restore(false);
    CHECK_FALSE(restored);
    CHECK(callbacks == 1);
    CHECK_FALSE(session.transactions().processing());
    CHECK(session.revision() == revision);
    CHECK(session.transactions().undo_count() == 1);
    CHECK(workbench.removed.empty());
    restore(true); // A duplicate late result cannot turn failure into success.
    CHECK(callbacks == 1);
    CHECK(session.source() == source);
    session.discard_async({}, ports, [&](bool ok) { ++callbacks; restored = ok; });
    restore(true);
    CHECK(restored);
    CHECK(callbacks == 2);
    CHECK_FALSE(session.source());
    CHECK(session.transactions().undo_count() == 0);
    CHECK(workbench.removed == std::vector<Path>{"pending.glb"});
}
