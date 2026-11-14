#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/AI/ModelGeneration/AdvancedCreationSession.hpp"
#include "slic3r/GUI/AI/ModelGeneration/AdvancedCreationFeatureHost.hpp"
#include "slic3r/GUI/AI/ModelGeneration/AdvancedMultiviewClient.hpp"
#include "slic3r/GUI/Redesign/RedesignModelRoute.hpp"
#include "../test_utils.hpp"
#include <wx/app.h>
#include <wx/evtloop.h>
#include <wx/init.h>
#include <wx/socket.h>
#include <chrono>

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

TEST_CASE("An unchanged verified advanced image stays ready while another view generates", "[AdvancedCreation][AdvancedImageRefresh]")
{
    auto session = ready_session();
    auto snapshot = *session.group();
    ++snapshot.revision;
    snapshot.views[index(ViewId::Back)].attempt_state = "running";
    for (auto& view : snapshot.views) {
        view.canonical_version_id = view.version_id;
        view.usable = false;
        view.asset_pending = true;
    }
    REQUIRE(session.apply_snapshot(snapshot, session.context()));
    for (ViewId view : {ViewId::Front, ViewId::Left, ViewId::Right}) {
        CHECK(session.group()->views[index(view)].usable);
        CHECK_FALSE(session.group()->views[index(view)].asset_pending);
    }
    snapshot = *session.group();
    ++snapshot.revision;
    snapshot.views[index(ViewId::Left)].canonical_version_id = "left-v2";
    snapshot.views[index(ViewId::Left)].usable = false;
    snapshot.views[index(ViewId::Left)].asset_pending = true;
    REQUIRE(session.apply_snapshot(snapshot, session.context()));
    CHECK(session.group()->views[index(ViewId::Left)].asset_pending);
    CHECK(session.group()->views[index(ViewId::Left)].version_id == "left-v1");
}

TEST_CASE("Advanced cache failures and changed digests affect only their own view", "[AdvancedCreation][AdvancedImageRefresh]")
{
    auto session=ready_session();auto snapshot=*session.group();++snapshot.revision;
    for(auto& view:snapshot.views) {view.canonical_version_id=view.version_id;view.usable=false;view.asset_pending=true;}
    snapshot.views[index(ViewId::Front)].sha256=std::string(64,'a');
    snapshot.views[index(ViewId::Back)].display_error=true;
    REQUIRE(session.apply_snapshot(snapshot,session.context()));
    CHECK(session.group()->views[index(ViewId::Front)].asset_pending);
    CHECK(session.group()->views[index(ViewId::Back)].display_error);
    CHECK_FALSE(session.group()->views[index(ViewId::Left)].asset_pending);
    CHECK_FALSE(session.group()->views[index(ViewId::Right)].asset_pending);
    auto ready=*session.group();++ready.revision;
    ready.views[index(ViewId::Front)].sha256=std::string(64,'a');
    ready.views[index(ViewId::Front)].usable=true;ready.views[index(ViewId::Front)].asset_pending=false;
    REQUIRE(session.apply_snapshot(ready,session.context()));
    REQUIRE_FALSE(session.apply_snapshot(snapshot,session.context()));
    CHECK(session.group()->views[index(ViewId::Front)].sha256==std::string(64,'a'));
    const auto old=session.context();session.begin_open("other-group");
    CHECK_FALSE(session.apply_snapshot(ready,old));
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

TEST_CASE("Advanced auto-save leaves editing available while explicit commands retain their lock",
          "[AdvancedCreation][AdvancedDraftInput]")
{
    const bool exclusive = GENERATE(false, true);
    wxInitializer init;
    REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-draft-input"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.path().string());
    wxIPV4address address;
    REQUIRE(address.Hostname("127.0.0.1"));
    REQUIRE(address.Service(0));
    wxSocketServer server(address, wxSOCKET_NOWAIT);
    REQUIRE(server.IsOk());
    REQUIRE(server.GetLocal(address));
    REQUIRE(address.Service() != 0);
    const std::string endpoint = "http://127.0.0.1:" + std::to_string(address.Service());
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, endpoint);
    host.session() = ready_session();
    auto draft = host.session().draft().design;
    draft.prompt = "Editing before save";
    REQUIRE(host.session().set_design_draft(draft));
    wxEventLoop loop;
    wxEventLoopActivator active(&loop);
    const auto until = [&](auto ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ready() && std::chrono::steady_clock::now() < deadline) {
            wxTheApp->ProcessPendingEvents();
            loop.DispatchTimeout(20);
        }
        return ready();
    };
    // Hold the real client's request open; there is no generation or external service.
    if (exclusive) host.open_group("22222222-2222-2222-2222-222222222222");
    else host.changed();
    REQUIRE(until([&] { return host.command_pending(); }));
    CHECK(host.draft_editing_locked() == exclusive);
    if (!exclusive) {
        draft.prompt = "Editing continues while saving";
        REQUIRE(host.session().set_design_draft(draft));
        host.changed();
        CHECK_FALSE(host.draft_editing_locked());
    }
    std::unique_ptr<wxSocketBase> connection;
    REQUIRE(until([&] {
        if (!connection) connection.reset(server.Accept(false));
        return bool(connection);
    }));
    const std::string response = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    connection->Write(response.data(), response.size());
    REQUIRE(connection->LastCount() == response.size());
    connection->Close();
    REQUIRE(until([&] { return !host.command_pending(); }));
    CHECK_FALSE(host.draft_editing_locked());
    CHECK(host.session().draft().design.prompt == draft.prompt);
    CHECK(host.session().draft_dirty());
    host.shutdown();
}

TEST_CASE("Advanced save acknowledgements preserve newer text and pending view selection",
          "[AdvancedCreation][AdvancedDraftInput]")
{
    auto session = ready_session();
    auto draft = session.draft().design;
    draft.style = "custom";
    draft.custom_style = "First style";
    REQUIRE(session.set_design_draft(draft));
    const auto submitted = session.draft();
    auto saved = *session.group();
    ++saved.revision;
    saved.input_epoch = "saved-style-epoch";
    saved.saved_draft = submitted.design;
    saved.pending_views.fill(true);
    draft.prompt = u8"换一个方向\n中文与 English 继续输入";
    draft.custom_style = u8"第二次风格编辑";
    REQUIRE(session.set_design_draft(draft));
    REQUIRE(session.apply_snapshot(saved, session.context(), false, &submitted));
    session.mark_draft_saved(submitted.design);
    CHECK(session.draft().design == draft);
    CHECK(session.draft_dirty());
    for (ViewId view : AllViews) CHECK(session.selection()[index(view)]);
    // Cache completion is a second ordinary snapshot, not a draft restore.
    REQUIRE(session.apply_snapshot(saved, session.context()));
    CHECK(session.draft().design == draft);
    CHECK(session.draft_dirty());
    auto latest = session.draft();
    saved.saved_draft = latest.design;
    saved.input_epoch = "latest-style-epoch";
    ++saved.revision;
    REQUIRE(session.apply_snapshot(saved, session.context(), false, &latest));
    session.mark_draft_saved(latest.design);
    CHECK_FALSE(session.draft_dirty());
}

TEST_CASE("Advanced upload acknowledgements adopt canonical paths without losing newer replacements",
          "[AdvancedCreation][AdvancedDraftInput]")
{
    auto session = ready_session();
    REQUIRE(session.set_upload(ViewId::Front, "chosen-front.png"));
    const auto submitted = session.draft();
    auto saved = *session.group();
    ++saved.revision;
    saved.input_epoch = "saved-upload-epoch";
    saved.uploads[index(ViewId::Front)] = "cache/front-asset.png";
    REQUIRE(session.set_upload(ViewId::Right, "newer-right.png"));
    REQUIRE(session.apply_snapshot(saved, session.context(), false, &submitted));
    session.mark_draft_saved(submitted.design);
    CHECK(session.draft().uploads[index(ViewId::Front)] == "cache/front-asset.png");
    CHECK(session.draft().uploads[index(ViewId::Right)] == "newer-right.png");
    CHECK(session.draft_dirty());
}

TEST_CASE("Advanced unchanged upload acknowledgements finish saving after canonicalization",
          "[AdvancedCreation][AdvancedDraftInput]")
{
    auto session = ready_session();
    REQUIRE(session.set_upload(ViewId::Front, "chosen-front.png"));
    const auto submitted = session.draft();
    auto saved = *session.group();
    ++saved.revision;
    saved.input_epoch = "saved-upload-epoch";
    saved.uploads[index(ViewId::Front)] = "cache/front-asset.png";
    REQUIRE(session.apply_snapshot(saved, session.context(), false, &submitted));
    session.mark_draft_saved(submitted.design);
    CHECK(session.draft().uploads == saved.uploads);
    CHECK_FALSE(session.draft_dirty());
}

TEST_CASE("Advanced late saves cannot replace a new group and explicit restores still restore text",
          "[AdvancedCreation][AdvancedDraftInput]")
{
    auto session = ready_session();
    const auto old = session.context();
    const auto submitted = session.draft();
    const auto saved = *session.group();
    session.new_creation();
    auto draft = session.draft().design;
    draft.prompt = "New creation text";
    REQUIRE(session.set_design_draft(draft));
    REQUIRE_FALSE(session.apply_snapshot(saved, old, false, &submitted));
    CHECK(session.draft().design == draft);
    REQUIRE(session.apply_snapshot(saved, session.begin_open(saved.id), true, &submitted));
    CHECK(session.draft().design == saved.saved_draft);
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

namespace {
const std::string routed_group = "11111111-1111-4111-8111-111111111111";
const std::string routed_job = "22222222-2222-4222-8222-222222222222";
nlohmann::json model_acceptance()
{
    return {{"id", routed_job}, {"creation_kind", "advanced_multiview_v1"},
        {"advanced_group_id", routed_group}, {"workflow_mode", "advanced"},
        {"state", "queued"}, {"phase", "queued"}, {"updated_at", 100.0}, {"message", "Four-view model accepted."}};
}
Session model_session()
{
    Session session;
    auto group = complete_group(); group.id = routed_group; group.model_job_id = "old-job";
    session.apply_snapshot(group, session.begin_open(group.id), true);
    return session;
}
Slic3r::GUI::ModelGenerationUIState model_projection()
{
    Slic3r::GUI::ModelGenerationUIState state;
    state.job_id = routed_job; state.model_generation_context = true;
    state.stage = Slic3r::GUI::ModelGenerationUIStage::GeneratingModel;
    return state;
}
}

TEST_CASE("Advanced acceptance reaches the shared model route before cached group images finish", "[AdvancedCreation][ModelGenerationRoute]")
{
    using Slic3r::GUI::AdvancedModelRouteIntent;
    using Slic3r::GUI::AdvancedMultiviewClient;
    const bool group_first = GENERATE(false, true);
    auto session = model_session();
    session.toggle(ViewId::Left);
    const auto selection = session.selection();
    const auto context = session.context();
    auto cached_group = *session.group();
    ModelJobEvent event {context, routed_job, 1};
    AdvancedModelRouteIntent route;
    route.request(event, 10);
    auto state = model_projection();
    CHECK_FALSE(route.ready(context, session.group()->model_job_id, state, 10, true, false));
    if (group_first) {
        cached_group.model_job_id = routed_job;
        REQUIRE(session.apply_snapshot(cached_group, context));
    }
    const auto accepted = AdvancedMultiviewClient::parse_model_job(model_acceptance(), routed_group, routed_job);
    REQUIRE(accepted);
    REQUIRE(session.apply_model_job(*accepted, context));
    event.phase = ModelJobEvent::Phase::Accepted;
    route.accept(event);
    CHECK(session.group()->model_busy);
    // No generated file, ready flag, image-cache completion or edit eligibility
    // is involved in this gate. The old group's task ID cannot swallow acceptance.
    CHECK_FALSE(state.model_ready);
    CHECK_FALSE(state.can_open_workbench);
    CHECK(route.ready(context, session.group()->model_job_id, state, 10, true, false));
    if (!group_first) REQUIRE(session.apply_snapshot(cached_group, context));
    CHECK(session.group()->model_job_id == routed_job);
    CHECK(session.selection() == selection);
    CHECK(session.context().epoch == context.epoch);
    CHECK(route.ready(context, session.group()->model_job_id, state, 10, true, false));
    // Repeated reconcile/status events cannot open the shared page twice.
    route.cancel();
    route.accept(event);
    CHECK_FALSE(route.ready(context, routed_job, state, 10, true, false));
}

TEST_CASE("Advanced acceptance rejects a response from another plan group or creator", "[AdvancedCreation][ModelGenerationRoute]")
{
    using Slic3r::GUI::AdvancedMultiviewClient;
    const auto key = GENERATE("id", "advanced_group_id", "creation_kind", "workflow_mode", "state");
    auto job = model_acceptance();
    job[key] = key == std::string("state") ? "" : "different-identity";
    CHECK_FALSE(AdvancedMultiviewClient::parse_model_job(job, routed_group, routed_job));
    job = model_acceptance(); job.erase(key);
    CHECK_FALSE(AdvancedMultiviewClient::parse_model_job(job, routed_group, routed_job));
}

TEST_CASE("Advanced receipt recovery registers the group before accepting its task without waiting for images", "[AdvancedCreation][ModelGenerationRoute]")
{
    Session session;
    const auto context = session.begin_open(routed_group);
    const auto accepted = Slic3r::GUI::AdvancedMultiviewClient::parse_model_job(model_acceptance(), routed_group, routed_job);
    REQUIRE(accepted);
    CHECK_FALSE(session.apply_model_job(*accepted, context));
    auto group = complete_group(); group.id = routed_group;
    for (auto& view : group.views) {
        view.usable = false; view.asset_pending = true;
    }
    REQUIRE(session.apply_snapshot(group, context, true));
    REQUIRE(session.apply_model_job(*accepted, context));
    CHECK(session.group()->model_job_id == routed_job);
    CHECK(session.group()->model_busy);
    Slic3r::GUI::AdvancedModelRouteIntent route;
    ModelJobEvent event {context, routed_job, 1};
    route.request(event, 1);
    event.phase = ModelJobEvent::Phase::Accepted; route.accept(event);
    CHECK(route.ready(context, routed_job, model_projection(), 1, true, false));
    session.begin_open("another-group");
    CHECK_FALSE(session.apply_model_job(*accepted, context));
}

TEST_CASE("Advanced delayed task projections wait for transitions and keep the foreground identity", "[AdvancedCreation][ModelGenerationRoute]")
{
    Slic3r::GUI::AdvancedModelRouteIntent route;
    auto session = model_session();
    ModelJobEvent event {session.context(), routed_job, 8};
    route.request(event, 10);
    auto projection = model_projection();
    // Cancellation, explicit rejection and uncertain submission do not emit
    // acceptance, even if an old projection happens to have the same job ID.
    CHECK_FALSE(route.ready(event.context, routed_job, projection, 10, true, false));
    event.phase = ModelJobEvent::Phase::Accepted; route.accept(event);
    auto old_projection = projection; old_projection.job_id = "old-job";
    CHECK_FALSE(route.ready(event.context, routed_job, old_projection, 10, true, false));
    CHECK_FALSE(route.ready(event.context, routed_job, projection, 10, true, true));
    CHECK(route.ready(event.context, routed_job, projection, 10, true, false));
    CHECK_FALSE(route.ready(event.context, routed_job, projection, 11, true, false));
    CHECK_FALSE(route.ready(event.context, routed_job, projection, 10, false, false));
    auto other_group = event.context; other_group.group_id = "other-group";
    CHECK_FALSE(route.ready(other_group, routed_job, projection, 10, true, false));
    auto other_epoch = event.context; ++other_epoch.epoch;
    CHECK_FALSE(route.ready(other_epoch, routed_job, projection, 10, true, false));
    // Leaving then returning, switching mode and shutdown all revoke the
    // presentation token; a late accepted callback cannot re-arm it.
    route.cancel(); route.accept(event);
    CHECK_FALSE(route.ready(event.context, routed_job, projection, 10, true, false));
    auto resume = event; resume.request = 9; resume.phase = ModelJobEvent::Phase::Requested;
    route.request(resume, 11); route.accept(event);
    CHECK_FALSE(route.ready(resume.context, routed_job, projection, 11, true, false));
    resume.phase = ModelJobEvent::Phase::Accepted; route.accept(resume);
    CHECK(route.ready(resume.context, routed_job, projection, 11, true, false));
}

TEST_CASE("Advanced current group model reconciliation preserves selection draft and context", "[AdvancedCreation][ModelGenerationRoute]")
{
    auto session = model_session();
    session.toggle(ViewId::Back);
    auto draft = session.draft().design; draft.prompt = "Preserve the current draft";
    session.set_design_draft(draft);
    const auto selection = session.selection(); const auto context = session.context();
    auto accepted = *Slic3r::GUI::AdvancedMultiviewClient::parse_model_job(model_acceptance(), routed_group, routed_job);
    REQUIRE(session.apply_model_job(accepted, context));
    auto group = *session.group(); ++group.revision;
    REQUIRE(session.apply_snapshot(group, context));
    CHECK(session.selection() == selection);
    CHECK(session.draft().design == draft);
    CHECK(session.draft_dirty());
    CHECK(session.context().epoch == context.epoch);
    for (const auto* state : {"stopping", "stopped", "failed", "running", "ready"}) {
        accepted.state = state;
        REQUIRE(session.apply_model_job(accepted, context));
        CHECK(session.selection() == selection);
        CHECK(session.group()->model_state == state);
    }
    session.begin_open("another-group");
    CHECK_FALSE(session.apply_model_job(accepted, context));
}

TEST_CASE("Advanced image cache completion cannot restore an older stopped model after resume", "[AdvancedCreation][ModelGenerationRoute]")
{
    auto session = model_session();
    auto resumed = *Slic3r::GUI::AdvancedMultiviewClient::parse_model_job(model_acceptance(), routed_group, routed_job);
    resumed.state = "running"; resumed.updated_at = 200;
    REQUIRE(session.apply_model_job(resumed, session.context()));
    auto old_snapshot = *session.group();
    old_snapshot.model_state = "stopped"; old_snapshot.model_busy = false; old_snapshot.model_updated_at = 100;
    REQUIRE(session.apply_snapshot(old_snapshot, session.context()));
    CHECK(session.group()->model_state == "running");
    CHECK(session.group()->model_busy);
    auto completed = *session.group(); completed.model_state = "ready";
    completed.model_busy = false; completed.model_updated_at = 300;
    REQUIRE(session.apply_snapshot(completed, session.context()));
    REQUIRE(session.apply_snapshot(old_snapshot, session.context()));
    CHECK(session.group()->model_state == "ready");
    CHECK_FALSE(session.group()->model_busy);
}
