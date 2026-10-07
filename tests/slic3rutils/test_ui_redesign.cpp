#include "slic3r/GUI/Redesign/RedesignCommand.hpp"
#include "slic3r/GUI/Redesign/RedesignFeatureFlags.hpp"
#include "slic3r/GUI/Redesign/OrcaBusinessAdapter.hpp"
#include "slic3r/GUI/Redesign/RedesignState.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

using namespace Slic3r::GUI;

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
