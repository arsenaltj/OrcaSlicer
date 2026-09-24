#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchTransactionController.hpp"

using Controller = Slic3r::GUI::BeautyWorkbenchTransactionController;

TEST_CASE("Beauty transaction controller keeps view access while processing", "[BeautyWorkbench]")
{
    Controller controller;
    CHECK(controller.capabilities().can_orbit);
    CHECK(controller.capabilities().can_zoom);
    CHECK(controller.begin(Controller::OperationKind::AppearanceRecolor));
    CHECK(controller.state() == Controller::TaskState::Submitting);
    CHECK(controller.capabilities().can_orbit);
    CHECK(controller.capabilities().can_view_original);
    CHECK(controller.capabilities().can_view_semantic_regions);
    CHECK(controller.capabilities().can_view_logs);
    CHECK_FALSE(controller.capabilities().can_submit_operation);
    CHECK_FALSE(controller.capabilities().can_reoptimize);
    CHECK_FALSE(controller.capabilities().can_accept);
    CHECK(controller.capabilities().can_cancel);
}

TEST_CASE("Beauty transaction controller records operation-level undo and redo", "[BeautyWorkbench]")
{
    Controller controller;
    int value = 0;
    controller.record({Controller::OperationKind::Selection, "selection",
        [&] { value = 0; }, [&] { value = 1; }});
    value = 1;
    CHECK(controller.undo());
    CHECK(value == 0);
    CHECK(controller.redo());
    CHECK(value == 1);
}

TEST_CASE("Beauty transaction failures remain retryable without undo entries", "[BeautyWorkbench]")
{
    Controller controller;
    REQUIRE(controller.begin(Controller::OperationKind::SemanticReoptimization));
    controller.finish(false, false, "worker failed");
    CHECK(controller.state() == Controller::TaskState::Failed);
    CHECK(controller.capabilities().can_submit_operation);
    CHECK(controller.capabilities().can_reoptimize);
    CHECK_FALSE(controller.capabilities().can_accept);
    CHECK(controller.undo_count() == 0);
    CHECK(controller.last_error() == "worker failed");
}

TEST_CASE("Beauty partition calculation locks model edits and restores them afterward", "[BeautyWorkbench]")
{
    Controller controller;
    REQUIRE(controller.begin(Controller::OperationKind::Selection));
    CHECK_FALSE(controller.capabilities().can_edit_selection);
    CHECK_FALSE(controller.capabilities().can_submit_operation);
    CHECK_FALSE(controller.capabilities().can_change_model);
    CHECK_FALSE(controller.capabilities().can_undo);
    CHECK(controller.capabilities().can_view_original);
    CHECK(controller.capabilities().can_view_logs);
    CHECK(controller.capabilities().can_cancel);
    controller.finish(true);
    CHECK(controller.capabilities().can_edit_selection);
    CHECK(controller.capabilities().can_submit_operation);
}

TEST_CASE("Beauty candidate remains acceptable after recording its undo entry", "[BeautyWorkbench]")
{
    Controller controller;
    REQUIRE(controller.begin(Controller::OperationKind::AppearanceRecolor));
    controller.finish(true, true);
    int candidate = 1;
    controller.record({Controller::OperationKind::AppearanceRecolor, "candidate",
        [&] { candidate = 0; }, [&] { candidate = 1; }});
    CHECK(controller.state() == Controller::TaskState::PreviewReady);
    CHECK(controller.capabilities().can_accept);
    CHECK(controller.capabilities().can_discard);
    REQUIRE(controller.undo());
    CHECK(candidate == 0);
    REQUIRE(controller.redo());
    CHECK(candidate == 1);
}

TEST_CASE("Beauty history keeps its current entry when a model file cannot be restored", "[BeautyWorkbench]")
{
    Controller controller;
    int candidate = 1;
    controller.record({Controller::OperationKind::AppearanceRecolor, "candidate",
        [&] { candidate = 0; }, [&] { candidate = 1; }, [] { return false; }});
    CHECK_FALSE(controller.undo());
    CHECK(candidate == 1);
    CHECK(controller.undo_count() == 1);
    CHECK(controller.redo_count() == 0);
}

TEST_CASE("Discarding a pending Beauty candidate retains prior accepted history", "[BeautyWorkbench]")
{
    Controller controller;
    int version = 0;
    controller.record({Controller::OperationKind::AcceptCandidate, "accepted",
        [&] { version = 0; }, [&] { version = 1; }});
    const size_t accepted_history = controller.undo_count();
    controller.record({Controller::OperationKind::AppearanceRecolor, "pending",
        [&] { version = 1; }, [&] { version = 2; }});
    controller.truncate_to(accepted_history);
    CHECK(controller.undo_count() == accepted_history);
    CHECK(controller.redo_count() == 0);
    REQUIRE(controller.undo());
    CHECK(version == 0);
}
