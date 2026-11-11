#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/AI/ModelGeneration/AdvancedCreationSession.hpp"
#include "slic3r/GUI/AI/ModelGeneration/AdvancedMultiviewClient.hpp"

using Slic3r::GUI::AdvancedMultiviewClient;
using namespace Slic3r::GUI::AdvancedCreation;

namespace {
GroupSnapshot complete_group()
{
    GroupSnapshot group;
    group.id = "group-a";
    group.input_epoch = "input-a";
    group.revision = 4;
    group.published = true;
    group.initial_input.prompt = "An object";
    group.saved_draft = group.initial_input;
    group.latest_submitted_draft = group.initial_input;
    for (ViewId view : AllViews) {
        group.views[index(view)].usable = true;
        group.views[index(view)].version_id = std::string(view_name(view)) + "-v1";
        group.views[index(view)].image_path = std::string(view_name(view)) + ".png";
    }
    return group;
}

Session ready_session()
{
    Session session;
    auto context = session.begin_open("group-a");
    session.apply_snapshot(complete_group(), context, true);
    return session;
}
}

TEST_CASE("Advanced selection toggles independently and derives a fixed ordered scope", "[AdvancedCreation]")
{
    auto session = ready_session();
    CHECK(session.effective_views().size() == 4);
    REQUIRE(session.toggle(ViewId::Right));
    REQUIRE(session.toggle(ViewId::Left));
    CHECK(session.effective_views() == std::vector<ViewId> {ViewId::Left, ViewId::Right});
    REQUIRE(session.toggle(ViewId::Right));
    CHECK(session.effective_views() == std::vector<ViewId> {ViewId::Left});
    REQUIRE(session.toggle(ViewId::Left));
    CHECK(session.effective_views().size() == 4);
    for (ViewId view : AllViews) session.toggle(view);
    CHECK(session.effective_views() == std::vector<ViewId>(AllViews.begin(), AllViews.end()));
    CHECK(session.draft().design.prompt == "An object");
}

TEST_CASE("Advanced double click preserves both selected and unselected images", "[AdvancedCreation]")
{
    const bool selected = GENERATE(false, true);
    auto session = ready_session();
    session.toggle(ViewId::Left);
    if (selected) session.toggle(ViewId::Back);
    const auto before = session.selection();
    session.click(ViewId::Back, 100, 500);
    CHECK_FALSE(session.flush_click(300));
    CHECK(session.double_click(ViewId::Back) == ViewId::Back);
    CHECK_FALSE(session.flush_click(900));
    CHECK(session.selection() == before);
}

TEST_CASE("Advanced quick clicks on different views accumulate without losing a pending click", "[AdvancedCreation]")
{
    auto session = ready_session();
    session.click(ViewId::Left, 100, 500);
    session.click(ViewId::Right, 200, 500);
    CHECK(session.selection()[index(ViewId::Left)]);
    CHECK_FALSE(session.selection()[index(ViewId::Right)]);
    CHECK(session.flush_click(700));
    CHECK(session.selection()[index(ViewId::Right)]);
    session.click(ViewId::Right, 800, 500);
    CHECK(session.flush_click(1300));
    CHECK_FALSE(session.selection()[index(ViewId::Right)]);
}

TEST_CASE("Advanced missing images cannot be selected or opened as successful designs", "[AdvancedCreation]")
{
    Session session;
    auto group = complete_group();
    group.views[index(ViewId::Back)].usable = false;
    session.apply_snapshot(group, session.begin_open(group.id), true);
    CHECK_FALSE(session.toggle(ViewId::Back));
    CHECK_FALSE(session.double_click(ViewId::Back));
    session.click(ViewId::Back, 100, 500);
    CHECK_FALSE(session.flush_click(600));
}

TEST_CASE("Advanced snapshots reject callbacks from an old history open and an older revision", "[AdvancedCreation]")
{
    auto session = ready_session();
    auto old = session.context();
    auto current = session.begin_open("group-b");
    CHECK_FALSE(session.apply_snapshot(complete_group(), old));
    auto group = complete_group();
    group.id = "group-b";
    REQUIRE(session.apply_snapshot(group, current, true));
    group.revision = 3;
    CHECK_FALSE(session.apply_snapshot(group, current));
    CHECK(session.group()->revision == 4);
}

TEST_CASE("Advanced polling and sidebar changes preserve edited text selection and model options", "[AdvancedCreation]")
{
    auto session = ready_session();
    auto draft = session.draft().design;
    draft.prompt = "New\nmultiline description";
    session.set_design_draft(draft);
    session.toggle(ViewId::Back);
    auto group = complete_group();
    group.revision = 5;
    REQUIRE(session.apply_snapshot(group, session.context()));
    session.set_sidebar_expanded(false);
    session.set_sidebar_expanded(true);
    CHECK(session.selection()[index(ViewId::Back)]);
    CHECK(session.draft().design.prompt == draft.prompt);
    CHECK(session.draft_dirty());
    session.mark_draft_saved(group.saved_draft);
    CHECK(session.draft_dirty());
    session.mark_draft_saved(draft);
    CHECK_FALSE(session.draft_dirty());
}

TEST_CASE("Advanced upload and style changes select in-place design targets", "[AdvancedCreation]")
{
    auto session = ready_session();
    CHECK(session.set_upload(ViewId::Right, "replacement.png"));
    CHECK(session.selection()[index(ViewId::Right)]);
    CHECK(session.group()->id == "group-a");
    Capabilities capabilities {true, true, true, true, true};
    HostGuards guards {true, true, true};
    auto eligibility = session.eligibility(capabilities, guards);
    CHECK_FALSE(eligibility.generate_model);
    CHECK(eligibility.model_reason == "draft_changed");

    auto draft = session.draft().design;
    draft.style = "sculpture";
    CHECK(session.set_design_draft(draft));
    for (ViewId view : AllViews) CHECK(session.selection()[index(view)]);
}

TEST_CASE("Advanced pending views survive restore and block model generation", "[AdvancedCreation]")
{
    auto session = ready_session();
    auto group = complete_group();
    group.revision = 5;
    group.input_epoch = "input-b";
    group.uploads[index(ViewId::Front)] = "front.png";
    group.uploads[index(ViewId::Left)] = "new-left.png";
    group.pending_views[index(ViewId::Left)] = true;
    REQUIRE(session.apply_snapshot(group, session.context()));
    CHECK(session.draft().uploads[index(ViewId::Left)] == "new-left.png");
    CHECK(session.selection()[index(ViewId::Left)]);
    CHECK_FALSE(session.toggle(ViewId::Left));

    Capabilities capabilities {true, true, true, true, true};
    HostGuards guards {true, true, true};
    const auto eligibility = session.eligibility(capabilities, guards);
    CHECK(eligibility.generate_design);
    CHECK_FALSE(eligibility.generate_model);
    CHECK(eligibility.model_reason == "design_regeneration_required");
}

TEST_CASE("Advanced group names follow snapshots without changing an edited draft", "[AdvancedCreation]")
{
    auto session = ready_session();
    auto draft = session.draft().design;
    draft.prompt = "Unsaved local description";
    session.set_design_draft(draft);
    auto group = complete_group();
    group.revision = 5;
    group.display_name = "Renamed set";
    REQUIRE(session.apply_snapshot(group, session.context()));
    REQUIRE(session.group());
    CHECK(session.group()->display_name == "Renamed set");
    CHECK(session.draft().design.prompt == "Unsaved local description");
    CHECK(session.draft_dirty());
}

TEST_CASE("Advanced new creations default to realistic GPT while history keeps its saved style", "[AdvancedCreation]")
{
    Session session;
    CHECK(session.draft().design.style == "realistic");
    CHECK(session.draft().design.provider == "gpt");

    auto group = complete_group();
    group.saved_draft.style = "sculpture";
    auto context = session.begin_open(group.id);
    REQUIRE(session.apply_snapshot(group, context, true));
    CHECK(session.draft().design.style == "sculpture");

    session.new_creation();
    CHECK(session.draft().design.style == "realistic");
    CHECK(session.draft().design.provider == "gpt");
}

TEST_CASE("Changing the selected image model changes the advanced command identity", "[AdvancedCreation]")
{
    Session session;
    const auto original_command = session.command_context();
    auto draft = session.draft().design;
    draft.provider = "yoyoclaw";
    draft.model = "image-model-library/Qwen-Image-3.0-PC-YOYO";
    REQUIRE(session.set_design_draft(draft));
    CHECK_FALSE(session.matches_command(original_command, false));

    const auto payload = AdvancedMultiviewClient::design_json(session.draft().design);
    CHECK(payload.at("design_provider") == "yoyoclaw");
    CHECK(payload.at("design_model") == "image-model-library/Qwen-Image-3.0-PC-YOYO");

    session.mark_draft_saved(draft);
    CHECK_FALSE(session.draft_dirty());
    const auto qwen_command = session.command_context();
    draft.model = "image-model-library/Hy-Image-3.0-PC-YOYO";
    REQUIRE(session.set_design_draft(draft));
    CHECK(session.draft_dirty());
    CHECK_FALSE(session.matches_command(qwen_command, false));
}

TEST_CASE("Advanced confirmation becomes stale when its target draft or base changes", "[AdvancedCreation]")
{
    auto session = ready_session();
    auto command = session.command_context();
    session.toggle(ViewId::Left);
    CHECK_FALSE(session.matches_command(command));
    CHECK(session.matches_command(command, false)); // 3D always consumes every view.
    command = session.command_context();
    auto draft = session.draft().design;
    draft.prompt = "Changed";
    session.set_design_draft(draft);
    CHECK_FALSE(session.matches_command(command, false));
    command = session.command_context();
    auto group = complete_group();
    ++group.revision;
    session.apply_snapshot(group, session.context());
    CHECK_FALSE(session.matches_command(command));
}

TEST_CASE("Advanced four image eligibility retains original guards and ignores 2D selection for models", "[AdvancedCreation]")
{
    auto session = ready_session();
    Capabilities capabilities {true, true, true, true, true};
    HostGuards guards {true, true, true};
    session.toggle(ViewId::Back);
    CHECK(session.eligibility(capabilities, guards).generate_model);
    guards.can_start = false;
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_model);
    guards.can_start = true;
    capabilities.four_view_model = false;
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_model);
    capabilities.four_view_model = true;
    auto draft = session.draft().design;
    draft.prompt = "Unsubmitted";
    session.set_design_draft(draft);
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_model);
    CHECK(session.eligibility(capabilities, guards).generate_design);
}

TEST_CASE("Advanced old failed submissions allow new 2D generation while retaining 3D guards", "[AdvancedCreation]")
{
    auto session = ready_session();
    Capabilities capabilities {true, true, true, true, true};
    HostGuards guards {true, true, true};
    auto group = complete_group();
    group.views[2].attempt_state = "failed";
    session.apply_snapshot(group, session.context());
    CHECK(session.eligibility(capabilities, guards).generate_model);
    CHECK(session.eligibility(capabilities, guards).retry);
    group.views[2].attempt_state = "ambiguous";
    session.apply_snapshot(group, session.context());
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_model);
    CHECK(session.eligibility(capabilities, guards).generate_design);
    CHECK(session.eligibility(capabilities, guards).recover);
}

TEST_CASE("Failed empty cards can be selected for generation but cannot open a preview", "[AdvancedCreation]")
{
    const std::string state = GENERATE("failed", "stopped", "blocked", "ambiguous", "result_pending", "result_saved");
    auto group = complete_group();
    group.views[1] = {};
    group.views[1].attempt_state = state;
    Session session;
    REQUIRE(session.apply_snapshot(group, session.begin_open(group.id), true));
    REQUIRE(session.toggle(ViewId::Left));
    CHECK(session.effective_views() == std::vector<ViewId> {ViewId::Left});
    CHECK_FALSE(session.double_click(ViewId::Left));
    CHECK(session.selection()[1]);
    Capabilities capabilities {true, true, true, true, true};
    HostGuards guards {true, true, true};
    CHECK(session.eligibility(capabilities, guards).generate_design);
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_model);
    guards.can_start = false;
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_design);
    guards.can_start = true;
    capabilities.service_available = false;
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_design);
}

TEST_CASE("A missing asset preserves its visible base and refreshes terminal state independently", "[AdvancedCreation]")
{
    auto session = ready_session();
    auto incoming = complete_group();
    incoming.revision = 8;
    incoming.views[1].canonical_version_id = "left-v2";
    incoming.views[1].version_id = "left-v2";
    incoming.views[1].usable = false;
    incoming.views[1].display_error = true;
    incoming.views[1].attempt_state = "succeeded";
    incoming.views[2].version_id = "back-v2";
    REQUIRE(session.apply_snapshot(incoming, session.context()));
    CHECK(session.group()->revision == 8);
    CHECK(session.group()->active_operation_id.empty());
    CHECK(session.group()->views[1].version_id == "left-v1");
    CHECK(session.group()->views[1].canonical_version_id == "left-v2");
    CHECK(session.group()->views[1].image_path == "left.png");
    CHECK(session.group()->views[1].usable);
    CHECK(session.group()->views[2].version_id == "back-v2");
    Capabilities capabilities {true, true, true, true, true};
    HostGuards guards {true, true, true};
    CHECK(session.eligibility(capabilities, guards).generate_design);
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_model);
    incoming.views[1].usable = true;
    incoming.views[1].display_error = false;
    REQUIRE(session.apply_snapshot(incoming, session.context()));
    CHECK(session.group()->views[1].version_id == "left-v2");
    CHECK_FALSE(session.group()->views[1].display_error);
    CHECK(session.eligibility(capabilities, guards).generate_model);
}

TEST_CASE("New creation immediately invalidates all old commands and pending card clicks", "[AdvancedCreation]")
{
    auto session = ready_session();
    auto busy = complete_group();
    busy.active_operation_id = "operation-a";
    busy.views[1].attempt_state = "submitting";
    REQUIRE(session.apply_snapshot(busy, session.context()));
    const auto callback = session.context();
    const auto command = session.command_context();
    session.click(ViewId::Left, 100, 500);
    session.new_creation();
    CHECK_FALSE(session.group());
    CHECK_FALSE(session.draft_dirty());
    CHECK(session.draft().design.prompt.empty());
    CHECK(session.draft().design.provider == "gpt");
    for (auto view : AllViews) {
        CHECK_FALSE(session.selection()[index(view)]);
        CHECK(session.draft().uploads[index(view)].empty());
    }
    CHECK_FALSE(session.flush_click(1000));
    CHECK_FALSE(session.matches_command(command));
    CHECK_FALSE(session.apply_snapshot(busy, callback));
    CHECK_FALSE(session.group());
}

TEST_CASE("Queued and running tasks retain the existing single submission guard", "[AdvancedCreation]")
{
    const std::string state = GENERATE("queued", "running");
    auto session = ready_session();
    auto group = complete_group();
    group.active_operation_id = "active-a";
    group.execution_state = state;
    group.views[1].attempt_state = state == "queued" ? "queued" : "submitting";
    REQUIRE(session.apply_snapshot(group, session.context()));
    Capabilities capabilities {true, true, true, true, true};
    HostGuards guards {true, true, true};
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_design);
    CHECK(session.eligibility(capabilities, guards).design_reason == "group_busy");
    CHECK(session.eligibility(capabilities, guards).stop);
}

TEST_CASE("Advanced cards animate only active image submissions", "[AdvancedCreation]")
{
    CHECK(design_attempt_generating("queued"));
    CHECK(design_attempt_generating("submitting"));
    for (const std::string state : {"", "succeeded", "failed", "blocked", "stopped",
                                    "ambiguous", "result_pending", "result_saved"}) {
        DYNAMIC_SECTION(state) { CHECK_FALSE(design_attempt_generating(state)); }
    }
}

TEST_CASE("Advanced uploads require a front reference and support a separate text creation", "[AdvancedCreation]")
{
    Session session;
    Capabilities capabilities {true, true, true, true, true};
    HostGuards guards {true, true, true};
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_design);
    session.set_upload(ViewId::Left, "left.png");
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_design);
    session.set_upload(ViewId::Front, "front.png");
    CHECK(session.eligibility(capabilities, guards).generate_design);
    session.new_creation();
    auto draft = session.draft().design;
    draft.prompt = "A new object";
    session.set_design_draft(draft);
    CHECK(session.eligibility(capabilities, guards).generate_design);
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_model);
}

TEST_CASE("Advanced four cards use both available dimensions and preserve complete image proportions", "[AdvancedCreation]")
{
    const int width = GENERATE(0, 360, 780, 1200, 1800);
    const int height = GENERATE(0, 260, 620, 1000);
    const auto layout = grid_layout(width, height, 16, 12, 30);
    for (std::size_t i = 0; i < 4; ++i) {
        const auto card = layout.cards[i];
        CHECK(card.width == layout.cards[0].width);
        CHECK(card.height == layout.cards[0].height);
        CHECK(card.x >= 0);
        CHECK(card.y >= 0);
        CHECK(card.x + card.width <= width);
        CHECK(card.y + card.height <= height);
        const auto image = contain_image(layout.images[i], 1200, 800);
        CHECK(image.x >= layout.images[i].x);
        CHECK(image.y >= layout.images[i].y);
        CHECK(image.width <= layout.images[i].width);
        CHECK(image.height <= layout.images[i].height);
        CHECK(std::abs(image.width * 2 - image.height * 3) <= 2);
    }
}

TEST_CASE("Advanced custom style requires a nonblank description before generation", "[AdvancedCreation]")
{
    Session session;
    Capabilities capabilities {true, true, true, true, true};
    HostGuards guards {true, true, true};
    auto draft = session.draft().design;
    draft.prompt = "An object"; draft.style = "custom"; draft.custom_style = " \t";
    session.set_design_draft(draft);
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_design);
    draft.custom_style = "Clay sculpture"; session.set_design_draft(draft);
    CHECK(session.eligibility(capabilities, guards).generate_design);
}
