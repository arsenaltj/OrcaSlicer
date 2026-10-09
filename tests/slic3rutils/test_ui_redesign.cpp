#include "slic3r/GUI/Redesign/RedesignCommand.hpp"
#include "slic3r/GUI/Redesign/RedesignFeatureFlags.hpp"
#include "slic3r/GUI/Redesign/OrcaBusinessAdapter.hpp"
#include "slic3r/GUI/Redesign/RedesignState.hpp"
#include "slic3r/GUI/Redesign/ImageHistoryPagination.hpp"
#include "slic3r/GUI/Redesign/AssetsWorkspacePolicy.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationHost.hpp"
#include "slic3r/GUI/Redesign/RedesignModelRoute.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

using namespace Slic3r::GUI;

TEST_CASE("The model gallery embeds only its installed page and delegates external links", "[UiRedesign][AssetsWorkspace]")
{
    CHECK(gallery_navigation("file", "", true) == GalleryNavigation::Embedded);
    CHECK(gallery_navigation("file", "localhost", true) == GalleryNavigation::Embedded);
    CHECK(gallery_navigation("file", "") == GalleryNavigation::Blocked);
    CHECK(gallery_navigation("file", "models.example.com", true) == GalleryNavigation::Blocked);
    CHECK(gallery_navigation("https", "arsenaltj.github.io") == GalleryNavigation::Browser);
    CHECK(gallery_navigation("https", "models.example.com") == GalleryNavigation::Browser);
    CHECK(gallery_navigation("https", "arsenaltj.github.io.example.com") == GalleryNavigation::Browser);
    for (const auto& scheme : {"file", "javascript", "bbl", "wxfs", "http", ""})
        CHECK(gallery_navigation(scheme, "arsenaltj.github.io") == GalleryNavigation::Blocked);
    CHECK(gallery_navigation("https", "") == GalleryNavigation::Blocked);
}

TEST_CASE("Saving the print project waits for import and unsaved model edits", "[UiRedesign][AssetsWorkspace]")
{
    using Status = PostGenerationUiState::Status;
    CHECK(print_project_save_available(true, false, false, false, Status::Ready));
    CHECK(print_project_save_available(true, false, false, false, Status::Empty));
    CHECK_FALSE(print_project_save_available(false, false, false, false, Status::Ready));
    CHECK_FALSE(print_project_save_available(true, true, false, false, Status::Ready));
    CHECK_FALSE(print_project_save_available(true, false, true, false, Status::Ready));
    CHECK_FALSE(print_project_save_available(true, false, false, true, Status::Ready));
    for (const auto status : {Status::Editing, Status::Processing, Status::CandidateReady, Status::ComparingBefore})
        CHECK_FALSE(print_project_save_available(true, false, false, false, status));
}

TEST_CASE("Image history pagination exposes each record exactly once", "[UiRedesign][ImageHistoryPagination]")
{
    ImageHistoryPagination pages;
    pages.set_count(23);
    REQUIRE(pages.pages() == 3);
    size_t next = 0;
    for (size_t page = 0; page < pages.pages(); ++page) {
        pages.go_to(page);
        CHECK(pages.begin() == next);
        for (size_t i = pages.begin(); i < pages.end(); ++i) {
            CHECK(pages.contains(i));
            ++next;
        }
        CHECK_FALSE(pages.contains(pages.end()));
    }
    CHECK(next == 23);
    pages.go_to(999);
    CHECK(pages.page() == 2);
    CHECK(pages.begin() == 20);
    CHECK_FALSE(pages.contains(0)); // Hit/keyboard actions must not target another page.
}

TEST_CASE("Removing the last image on the last page returns to a valid page", "[UiRedesign][ImageHistoryPagination]")
{
    ImageHistoryPagination pages;
    pages.set_count(21); pages.go_to(2);
    pages.set_count(20);
    CHECK(pages.page() == 1);
    CHECK(pages.begin() == 10);
    CHECK(pages.end() == 20);
    pages.set_count(0);
    CHECK(pages.pages() == 0);
    CHECK(pages.begin() == pages.end());
    CHECK_FALSE(pages.contains(0));
    pages.go_to(999);
    CHECK(pages.page() == 0);
}

TEST_CASE("Searching image history starts at the first matching page", "[UiRedesign][ImageHistoryPagination]")
{
    ImageHistoryPagination pages;
    pages.set_count(100); pages.go_to(8);
    pages.set_count(25, true);
    CHECK(pages.page() == 0);
    CHECK(pages.pages() == 3);
    pages.go_to(1);
    pages.set_count(25); // Reopening/refetching retains the user's page.
    CHECK(pages.page() == 1);
}

TEST_CASE("Resizing image history keeps the former first record on screen", "[UiRedesign][ImageHistoryPagination]")
{
    ImageHistoryPagination pages;
    pages.set_count(37); pages.go_to(2);
    pages.set_capacity(6);
    CHECK(pages.contains(20));
    CHECK(pages.pages() == 7);
    const size_t first = pages.begin();
    pages.set_capacity(10);
    CHECK(pages.contains(first));
    pages.set_capacity(0);
    CHECK(pages.capacity() == 1);
}

TEST_CASE("Image history fits whole square rows above the fixed pager", "[UiRedesign][ImageHistoryPagination]")
{
    const auto design = ImageHistoryGrid::fit(312, 798, 12);
    CHECK(design.rows == 5);
    CHECK(design.edge == 150);
    CHECK(ImageHistoryGrid::fit(312, 797, 12).rows == 4);
    for (int scale : {1, 2, 3}) {
        for (int height : {50, 150, 300, 620, 798, 1200}) {
            const auto grid = ImageHistoryGrid::fit(312 * scale, height * scale, 12 * scale);
            CHECK(grid.rows >= 1);
            CHECK(grid.rows <= 5);
            CHECK(grid.rows * grid.edge + (grid.rows - 1) * 12 * scale <= height * scale);
            CHECK(2 * grid.edge + 12 * scale <= 312 * scale);
        }
    }
}

namespace {

void set_environment(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

class ScopedEnvironment final
{
public:
    explicit ScopedEnvironment(const char* name) : m_name(name)
    {
        if (const char* value = std::getenv(name)) {
            m_had_value = true;
            m_value = value;
        }
    }

    ~ScopedEnvironment()
    {
        set_environment(m_name, m_had_value ? m_value.c_str() : nullptr);
    }

private:
    const char* m_name;
    bool m_had_value { false };
    std::string m_value;
};

}

TEST_CASE("Image home is the default surface with an explicit legacy fallback", "[UiRedesign]")
{
    ScopedEnvironment global("ORCASLICER_UI_REDESIGN");
    ScopedEnvironment image_home("ORCASLICER_UI_REDESIGN_IMAGE_HOME");
    set_environment("ORCASLICER_UI_REDESIGN", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN_IMAGE_HOME", nullptr);
    CHECK(RedesignFeatureFlags::surface_enabled("IMAGE_HOME"));
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("OTHER"));

    set_environment("ORCASLICER_UI_REDESIGN_IMAGE_HOME", "0");
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("IMAGE_HOME"));
    set_environment("ORCASLICER_UI_REDESIGN_IMAGE_HOME", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN", "0");
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("IMAGE_HOME"));
    set_environment("ORCASLICER_UI_REDESIGN_IMAGE_HOME", "1");
    CHECK(RedesignFeatureFlags::surface_enabled("IMAGE_HOME"));
}

TEST_CASE("Image home startup requires no pending 3D work", "[UiRedesign]")
{
    CHECK(RedesignFeatureFlags::image_home_on_startup(true, false, false, false));
    CHECK_FALSE(RedesignFeatureFlags::image_home_on_startup(false, false, false, false));
    CHECK_FALSE(RedesignFeatureFlags::image_home_on_startup(true, true, false, false));
    CHECK_FALSE(RedesignFeatureFlags::image_home_on_startup(true, false, true, false));
    CHECK_FALSE(RedesignFeatureFlags::image_home_on_startup(true, false, false, true));
}

TEST_CASE("Startup splash is enabled by default with an explicit override", "[UiRedesign]")
{
    ScopedEnvironment global("ORCASLICER_UI_REDESIGN");
    ScopedEnvironment splash("ORCASLICER_UI_REDESIGN_STARTUP_SPLASH");
    set_environment("ORCASLICER_UI_REDESIGN", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SPLASH", nullptr);
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_SPLASH"));

    set_environment("ORCASLICER_UI_REDESIGN", "0");
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("STARTUP_SPLASH"));
    set_environment("ORCASLICER_UI_REDESIGN", nullptr);

    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SPLASH", "1");
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_SPLASH"));
    CHECK(RedesignFeatureFlags::surface_enabled("IMAGE_HOME"));
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("OTHER"));

    set_environment("ORCASLICER_UI_REDESIGN", "0");
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_SPLASH"));
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SPLASH", "0");
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("STARTUP_SPLASH"));
}

TEST_CASE("Startup buffer defaults to the new layout and remains independently configurable", "[UiRedesign]")
{
    ScopedEnvironment global("ORCASLICER_UI_REDESIGN");
    ScopedEnvironment buffer("ORCASLICER_UI_REDESIGN_STARTUP_BUFFER");
    ScopedEnvironment splash("ORCASLICER_UI_REDESIGN_STARTUP_SPLASH");
    set_environment("ORCASLICER_UI_REDESIGN", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_BUFFER", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SPLASH", nullptr);
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_BUFFER"));
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_BUFFER", "1");
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_BUFFER"));
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_SPLASH"));
    CHECK(RedesignFeatureFlags::surface_enabled("IMAGE_HOME"));
    set_environment("ORCASLICER_UI_REDESIGN", "0");
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_BUFFER"));
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("STARTUP_SPLASH"));
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_BUFFER", "0");
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("STARTUP_BUFFER"));
}

TEST_CASE("First-use setup is enabled by default and respects startup compatibility overrides", "[UiRedesign]")
{
    ScopedEnvironment global("ORCASLICER_UI_REDESIGN");
    ScopedEnvironment setup("ORCASLICER_UI_REDESIGN_STARTUP_SETUP");
    set_environment("ORCASLICER_UI_REDESIGN", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SETUP", nullptr);
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_SETUP"));
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SETUP", "0");
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("STARTUP_SETUP"));
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SETUP", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN", "0");
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("STARTUP_SETUP"));
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SETUP", "1");
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_SETUP"));
}

TEST_CASE("The complete model workflow is enabled without a review launcher", "[UiRedesign]")
{
    ScopedEnvironment global("ORCASLICER_UI_REDESIGN");
    ScopedEnvironment workflow("ORCASLICER_UI_REDESIGN_MODEL_WORKFLOW");
    ScopedEnvironment review("ORCASLICER_MODEL_WORKFLOW_REVIEW");
    set_environment("ORCASLICER_UI_REDESIGN", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN_MODEL_WORKFLOW", nullptr);
    set_environment("ORCASLICER_MODEL_WORKFLOW_REVIEW", nullptr);
    CHECK(RedesignFeatureFlags::model_workflow_review_enabled());
    set_environment("ORCASLICER_UI_REDESIGN", "0");
    CHECK_FALSE(RedesignFeatureFlags::model_workflow_review_enabled());
    set_environment("ORCASLICER_UI_REDESIGN_MODEL_WORKFLOW", "1");
    CHECK(RedesignFeatureFlags::model_workflow_review_enabled());
    set_environment("ORCASLICER_MODEL_WORKFLOW_REVIEW", "0");
    CHECK_FALSE(RedesignFeatureFlags::model_workflow_review_enabled());
    set_environment("ORCASLICER_UI_REDESIGN_MODEL_WORKFLOW", "0");
    set_environment("ORCASLICER_MODEL_WORKFLOW_REVIEW", "1");
    CHECK(RedesignFeatureFlags::model_workflow_review_enabled());
}

TEST_CASE("Redesign command registry executes registered commands", "[UiRedesign]")
{
    RedesignCommandRegistry registry;
    bool executed = false;

    REQUIRE(registry.register_command(
        RedesignCommandId::NewProject,
        [] { return true; },
        [&executed] { executed = true; }));
    CHECK(registry.contains(RedesignCommandId::NewProject));
    CHECK(registry.can_execute(RedesignCommandId::NewProject));
    CHECK(registry.execute(RedesignCommandId::NewProject));
    CHECK(executed);
    CHECK_FALSE(registry.execute(RedesignCommandId::OpenProject));
    CHECK_FALSE(registry.register_command(
        RedesignCommandId::NewProject,
        [] { return true; },
        [] {}));
}

TEST_CASE("Redesign command registry blocks commands that cannot execute", "[UiRedesign]")
{
    RedesignCommandRegistry registry;
    bool executed = false;

    REQUIRE(registry.register_command(
        RedesignCommandId::StartSlice,
        [] { return false; },
        [&executed] { executed = true; }));
    CHECK_FALSE(registry.can_execute(RedesignCommandId::StartSlice));
    CHECK_FALSE(registry.execute(RedesignCommandId::StartSlice));
    CHECK_FALSE(executed);
}

TEST_CASE("Redesign state store publishes monotonic snapshots", "[UiRedesign]")
{
    RedesignStateStore store;
    RedesignStateSnapshot received;
    int notifications = 0;
    store.subscribe([&](const RedesignStateSnapshot& snapshot) {
        received = snapshot;
        ++notifications;
    });

    RedesignStateSnapshot first;
    first.project_open = true;
    first.project_dirty = true;
    store.publish(first);
    CHECK(store.snapshot().revision == 1);
    CHECK(received.project_dirty);

    RedesignStateSnapshot second;
    second.revision = 1;
    second.slice_status = RedesignSliceStatus::Slicing;
    store.publish(second);
    CHECK(store.snapshot().revision == 2);
    CHECK(store.snapshot().slice_status == RedesignSliceStatus::Slicing);
    CHECK(notifications == 2);
}

TEST_CASE("Orca business adapter stays inert before it is attached", "[UiRedesign]")
{
    OrcaBusinessAdapter adapter;
    RedesignCommandRegistry registry;

    CHECK_FALSE(adapter.attached());
    adapter.register_commands(registry);
    CHECK(registry.size() == 0);

    const RedesignStateSnapshot state = adapter.snapshot();
    CHECK_FALSE(state.project_open);
    CHECK_FALSE(state.project_dirty);
    CHECK_FALSE(state.has_selection);
    CHECK_FALSE(state.can_undo);
    CHECK_FALSE(state.can_redo);
    CHECK(state.slice_status == RedesignSliceStatus::Idle);
}

TEST_CASE("Model generation snapshots distinguish styles within the same family", "[UiRedesign]")
{
    ModelGenerationUIState current;
    current.input.style = "cartoon";
    current.input.prompt = "a small cat";
    for (const std::string style : {"portrait_sketch", "low_poly", "relief", "ink_relief", "diorama", "custom"}) {
        DYNAMIC_SECTION(style) {
            auto updated = current;
            updated.input.style = style;
            CHECK_FALSE(current.input == updated.input);
            CHECK_FALSE(current.same_content(updated));
            current = updated;
            CHECK(current.same_content(updated));
        }
    }
}

TEST_CASE("Design timing changes are published even when progress stays unchanged", "[UiRedesign][DesignGenerationTiming]")
{
    ModelGenerationUIState current;
    auto updated = current;
    updated.design_elapsed_seconds = 12;
    CHECK_FALSE(current.same_content(updated));
    current = updated;
    CHECK(current.same_content(updated));
    updated.design_estimated_seconds = 60;
    CHECK_FALSE(current.same_content(updated));
}

TEST_CASE("Custom style edits publish new snapshots while preserving the unfinished draft", "[UiRedesign]")
{
    ModelGenerationUIState current;
    current.input.style = "custom";
    current.input.custom_style = "soft clay";
    for (const std::string draft : {"", "soft clay ", "soft clay\nwith broad shapes"}) {
        DYNAMIC_SECTION(draft) {
            auto updated = current;
            updated.input.custom_style = draft;
            CHECK_FALSE(current.input == updated.input);
            CHECK_FALSE(current.same_content(updated));
            // Leaving the custom family keeps its draft available when switching back.
            auto another_style = updated;
            another_style.input.style = "low_poly";
            CHECK_FALSE(updated.same_content(another_style));
            another_style.input.style = "custom";
            CHECK(updated.same_content(another_style));
            CHECK(another_style.input.custom_style == draft);
        }
    }
}

TEST_CASE("Provider failures publish alongside unchanged design timing", "[UiRedesign][ModelGenerationSubmissionState]")
{
    ModelGenerationUIState current;
    current.design_elapsed_seconds = 120;
    current.design_estimated_seconds = 60;
    auto failed = current;
    failed.provider_error_code = "image_connection_failed";
    failed.provider_error_category = "image_preprocessing";
    failed.provider_error_retryable = true;
    failed.provider_error_ambiguous = true;
    CHECK_FALSE(current.same_content(failed));
    CHECK(failed.design_elapsed_seconds == current.design_elapsed_seconds);
    current = failed;
    failed.provider_error_ambiguous = false;
    CHECK_FALSE(current.same_content(failed));
}

TEST_CASE("Local design history replaces a locked model session without reusing its task", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState current;
    current.revision = 10;
    current.model_generation_session = 3;
    current.model_generation_context = true;
    current.job_id = "previous-model";
    current.stage = ModelGenerationUIStage::ModelReady;
    auto design = current;
    ++design.revision;
    design.model_generation_context = false;
    design.stage = ModelGenerationUIStage::DesignReady;
    design.job_id = "selected-design";
    CHECK(model_generation_route_action(design, current, true, 3, current.job_id) == RedesignModelRouteAction::Ignore);
    ++design.model_generation_session;
    CHECK(model_generation_route_action(design, current, true, 3, current.job_id) == RedesignModelRouteAction::Image);
    CHECK(model_generation_route_action(current, design, false, 0, {}) == RedesignModelRouteAction::Ignore);
}

TEST_CASE("A design stays on its page until a confirmed model submission starts", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState design;
    design.revision = 10;
    design.model_generation_session = 3;
    design.job_id = "current-design";
    design.stage = ModelGenerationUIStage::DesignReady;
    // Dismissing confirmation leaves the host state unchanged.
    CHECK(model_generation_route_action(design, design, false, 3, design.job_id) == RedesignModelRouteAction::Refresh);

    auto submitted = design;
    ++submitted.revision;
    ++submitted.model_generation_session;
    submitted.model_generation_context = true;
    submitted.busy = true;
    submitted.stage = ModelGenerationUIStage::GeneratingModel;
    CHECK(model_generation_route_action(submitted, design, false, 3, design.job_id) == RedesignModelRouteAction::Model);
    CHECK(model_generation_route_action(design, submitted, true, 4, submitted.job_id) == RedesignModelRouteAction::Ignore);
}

TEST_CASE("History preparation stays on the image page and routes only its confirmed child task", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState design;
    design.revision = 10;
    design.model_generation_session = 3;
    design.job_id = "history-design";
    design.stage = ModelGenerationUIStage::DesignReady;
    auto preparing = design;
    ++preparing.revision;
    preparing.busy = true;
    CHECK(model_generation_route_action(preparing, design, false, 3, design.job_id) == RedesignModelRouteAction::Refresh);

    auto failed = preparing;
    ++failed.revision;
    failed.busy = false;
    CHECK(model_generation_route_action(failed, preparing, false, 3, design.job_id) == RedesignModelRouteAction::Refresh);

    auto submitted = preparing;
    ++submitted.revision;
    ++submitted.model_generation_session;
    submitted.job_id = "new-child-task";
    submitted.model_generation_context = true;
    submitted.stage = ModelGenerationUIStage::GeneratingModel;
    CHECK(model_generation_route_action(submitted, preparing, false, 3, design.job_id) == RedesignModelRouteAction::Model);

    // A late preparation notification must not undo the child task's route.
    preparing.revision = submitted.revision + 1;
    CHECK(model_generation_route_action(preparing, submitted, true, 4, submitted.job_id) == RedesignModelRouteAction::Ignore);
}

TEST_CASE("A restored failed model opens its result page after leaving a history workbench", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState current;
    current.revision = 20;
    current.model_generation_session = 3;
    current.model_generation_context = true;
    current.job_id = "completed-model";
    current.stage = ModelGenerationUIStage::ModelReady;
    auto restored = current;
    restored.revision = 21;
    restored.model_generation_session = 4;
    restored.job_id = "failed-model";
    restored.stage = ModelGenerationUIStage::Failed;
    CHECK(model_generation_route_action(restored, current, true, 3, current.job_id) == RedesignModelRouteAction::Model);

    restored.model_generation_context = false;
    restored.stage = ModelGenerationUIStage::DesignReady;
    CHECK(model_generation_route_action(restored, current, true, 3, current.job_id) == RedesignModelRouteAction::Image);

    restored.job_id.clear();
    CHECK(model_generation_route_action(restored, current, true, 3, current.job_id) == RedesignModelRouteAction::Ignore);
}

TEST_CASE("Old revisions and task identities cannot replace the selected model route", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState current;
    current.revision = 20;
    current.model_generation_session = 3;
    current.model_generation_context = true;
    current.job_id = "active-model";
    auto stale = current;
    stale.revision = 19;
    CHECK(model_generation_route_action(stale, current, true, 3, current.job_id) == RedesignModelRouteAction::Ignore);

    stale.revision = 21;
    stale.model_generation_session = 2;
    CHECK(model_generation_route_action(stale, current, true, 3, current.job_id) == RedesignModelRouteAction::Ignore);

    stale.model_generation_session = 3;
    stale.job_id = "previous-model";
    CHECK(model_generation_route_action(stale, current, true, 3, current.job_id) == RedesignModelRouteAction::Ignore);

    stale.job_id = current.job_id;
    stale.model_generation_context = false;
    CHECK(model_generation_route_action(stale, current, true, 3, current.job_id) == RedesignModelRouteAction::Ignore);
}

TEST_CASE("Task updates preserve the selected model view across failure and recovery", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState current;
    current.model_generation_session = 3;
    current.model_generation_context = true;
    current.job_id = "active-model";
    auto update = current;
    update.revision = 1;
    for (const auto stage : {ModelGenerationUIStage::GeneratingModel, ModelGenerationUIStage::Failed,
                            ModelGenerationUIStage::Stopped, ModelGenerationUIStage::ModelReady}) {
        update.stage = stage;
        CHECK(model_generation_route_action(update, current, true, 3, current.job_id) == RedesignModelRouteAction::Refresh);
    }
}

TEST_CASE("Returning to the design page is preserved until a new model session starts", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState current;
    current.model_generation_session = 3;
    current.model_generation_context = true;
    current.job_id = "active-model";
    auto update = current;
    update.revision = 1;
    CHECK(model_generation_route_action(update, current, false, 0, {}) == RedesignModelRouteAction::Refresh);

    ++update.model_generation_session;
    CHECK(model_generation_route_action(update, current, false, 0, {}) == RedesignModelRouteAction::Model);
}
