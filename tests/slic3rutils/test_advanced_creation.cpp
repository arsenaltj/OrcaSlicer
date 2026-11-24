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

TEST_CASE("Advanced invalid drafts remain clickable without authorizing generation",
          "[AdvancedCreation][GenerationClickFeedback]")
{
    const std::string invalid = GENERATE("empty", "front", "prompt", "custom-empty", "custom-long");
    Session session;
    auto draft = session.draft().design;
    std::string reason;
    if (invalid == "empty") reason = "empty_input";
    if (invalid == "front") { session.set_upload(ViewId::Left, "left.png"); reason = "missing_front"; }
    if (invalid == "prompt") { draft.prompt.assign(2001,'a'); reason = "prompt_too_long"; }
    if (invalid == "custom-empty") { draft.prompt = "object"; draft.style = "custom"; reason = "custom_style_empty"; }
    if (invalid == "custom-long") { draft.prompt = "object"; draft.custom_style.assign(1001,'a'); reason = "custom_style_too_long"; }
    session.set_design_draft(draft);
    const auto retained = session.draft().design;
    const auto eligible = session.eligibility({true,true,true,true,true}, {true,true,true});
    CHECK(eligible.click_generate_design);
    CHECK(eligible.click_generate_model);
    CHECK_FALSE(eligible.generate_design);
    CHECK_FALSE(eligible.generate_model);
    CHECK(session.input_rejection() == reason);
    CHECK_FALSE(session.group().has_value());
    CHECK(session.draft().design == retained);
}

TEST_CASE("Advanced model selection defaults to every available direction and always includes front",
          "[AdvancedCreation][AdvancedFlexibleViews]")
{
    const int mask = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    auto group = complete_group();
    std::vector<ViewId> expected {ViewId::Front};
    group.uploads[0] = "front-upload.png";
    group.design_targets = {true, false, false, false};
    for (std::size_t slot = 1; slot < 4; ++slot) {
        if (mask & (1 << (slot - 1))) {
            expected.push_back(AllViews[slot]);
            group.design_targets[slot] = true;
            group.uploads[slot] = std::string(view_name(AllViews[slot])) + "-upload.png";
        } else group.views[slot] = {};
    }
    Session session;
    REQUIRE(session.apply_snapshot(group, session.begin_open(group.id), true));
    CHECK(session.effective_views() == expected);
    CHECK(session.effective_model_views() == expected);
    Capabilities capabilities {true,true,true,true,true,{true,true,true,true},true};
    REQUIRE(session.eligibility(capabilities,{true,true,true}).generate_model);
    session.click(ViewId::Front, 100, 10, SelectionContext::Model);
    CHECK_FALSE(session.flush_click(110));
    CHECK(session.effective_model_views() == expected);
    CHECK(session.double_click(ViewId::Front).has_value());
    for (ViewId view : expected) if (view != ViewId::Front) {
        session.click(view, 200, 10, SelectionContext::Model);
        REQUIRE(session.flush_click(210));
    }
    CHECK(session.effective_model_views() == std::vector<ViewId>{ViewId::Front});
    CHECK(session.eligibility(capabilities,{true,true,true}).generate_model);
    capabilities.model_image_counts[0] = false;
    CHECK_FALSE(session.eligibility(capabilities,{true,true,true}).generate_model);
}

TEST_CASE("Explicit model changes and design repaint selection have independent command identities",
          "[AdvancedCreation][AdvancedFlexibleViews]")
{
    auto session = ready_session();
    const auto command = session.command_context();
    session.click(ViewId::Right, 100, 10, SelectionContext::Model);
    REQUIRE(session.flush_click(110));
    CHECK(session.selection() == std::array<bool,4>{false,false,false,false});
    CHECK_FALSE(session.model_selection()[3]);
    CHECK(session.matches_command(command));
    CHECK_FALSE(session.matches_model_command(command));
    session.click(ViewId::Right, 200, 10, SelectionContext::Design);
    REQUIRE(session.flush_click(210));
    CHECK(session.effective_views() == std::vector<ViewId>{ViewId::Right});
    CHECK_FALSE(session.model_selection()[3]);
    auto changed = *session.group(); ++changed.revision;
    changed.views[3].version_id = changed.views[3].canonical_version_id = "right-v2";
    REQUIRE(session.apply_snapshot(changed, session.context()));
    CHECK_FALSE(session.model_selection()[3]);
    session.click(ViewId::Left, 300, 10, SelectionContext::DesignForNextModel);
    REQUIRE(session.flush_click(310));
    CHECK(session.effective_views() == std::vector<ViewId>{ViewId::Left,ViewId::Right});
    CHECK(session.effective_model_views() == std::vector<ViewId>{ViewId::Front,ViewId::Left,ViewId::Right});
}

TEST_CASE("Accepting the first uploaded creation does not retain a pending replacement request",
          "[AdvancedCreation][AdvancedFlexibleViews]")
{
    Session session;
    REQUIRE(session.set_upload(ViewId::Front,"uploaded.png",true));
    auto group = complete_group(); group.uploads[0] = "canonical.png";
    REQUIRE(session.apply_snapshot(group,session.begin_open(group.id),true));
    CHECK_FALSE(session.draft().replaced_uploads[0]);
    CHECK_FALSE(session.draft_dirty());
    auto draft = session.draft().design; draft.prompt = "Changed text";
    session.set_design_draft(draft);
    CHECK_FALSE(session.draft().replaced_uploads[0]);
    CHECK(session.group()->views[0].usable);
}

TEST_CASE("Automatic draft selection changes preserve model preference while new directions default selected",
          "[AdvancedCreation][AdvancedFlexibleViews]")
{
    auto group = complete_group(); group.views[2] = {};
    Session session; REQUIRE(session.apply_snapshot(group,session.begin_open(group.id),true));
    session.click(ViewId::Left,100,10,SelectionContext::Model);
    REQUIRE(session.flush_click(110));
    auto draft = session.draft().design; draft.style = "anime"; session.set_design_draft(draft);
    CHECK_FALSE(session.model_selection()[1]);
    auto next = complete_group(); next.revision += 1;
    REQUIRE(session.apply_snapshot(next,session.context()));
    CHECK(session.model_selection()[2]);
    CHECK_FALSE(session.model_selection()[1]);
    const auto saved = session.local_draft();
    auto context = session.begin_open("group-b"); next.id = "group-b";
    REQUIRE(session.apply_snapshot(next,context,true));
    CHECK(session.model_selection()[1]);
    context = session.begin_open("group-a"); next.id = "group-a";
    REQUIRE(session.apply_snapshot(next,context,true));
    session.restore_local_draft(saved);
    CHECK_FALSE(session.model_selection()[1]);
    CHECK(session.model_selection()[2]);
}

TEST_CASE("Upload deletion and same path replacement hide designs through stale snapshots and local recovery",
          "[AdvancedCreation][AdvancedFlexibleViews][AdvancedDraftInput]")
{
    const bool replacement = GENERATE(false,true);
    auto session = ready_session();
    auto original = *session.group(); original.uploads[1] = "same-path.png";
    REQUIRE(session.apply_snapshot(original,session.context(),true));
    REQUIRE(session.set_upload(ViewId::Left,replacement ? "same-path.png" : "",replacement));
    REQUIRE_FALSE(session.group()->views[1].usable);
    CHECK_FALSE(session.model_selection()[1]);
    REQUIRE(session.apply_snapshot(original,session.context()));
    CHECK_FALSE(session.group()->views[1].usable);
    const auto saved = session.local_draft();
    Session reopened; REQUIRE(reopened.apply_snapshot(original,reopened.begin_open(original.id),true));
    reopened.restore_local_draft(saved);
    REQUIRE(reopened.apply_snapshot(original,reopened.context()));
    CHECK_FALSE(reopened.group()->views[1].usable);
    auto cleared = original; ++cleared.revision; cleared.input_epoch = "input-b";
    cleared.views[1] = {}; cleared.uploads[1] = replacement ? "new-canonical.png" : "";
    cleared.pending_views[1] = replacement;
    const auto submitted = reopened.draft();
    REQUIRE(reopened.apply_snapshot(cleared,reopened.context(),false,&submitted));
    CHECK_FALSE(reopened.group()->views[1].usable);
    CHECK_FALSE(reopened.draft_dirty());
    CHECK_FALSE(reopened.apply_snapshot(original,reopened.context()));
    if (replacement) {
        auto success = cleared; ++success.revision; success.pending_views[1] = false;
        success.views[1] = original.views[1]; success.views[1].version_id = "left-v2";
        success.views[1].canonical_version_id = "left-v2";
        REQUIRE(reopened.apply_snapshot(success,reopened.context()));
        CHECK(reopened.group()->views[1].usable);
        CHECK(reopened.model_selection()[1]);
    }
}

TEST_CASE("A save cannot acknowledge a newer same path replacement or shrink initial guidance targets",
          "[AdvancedCreation][AdvancedFlexibleViews][AdvancedInitialGuidance]")
{
    auto original = complete_group();
    original.uploads = {"front-upload.png", "left-upload.png", "", ""};
    original.design_targets = {true, true, false, false};
    original.views[2] = original.views[3] = {};
    Session session;
    REQUIRE(session.apply_snapshot(original, session.begin_open(original.id), true));
    session.arm_initial_design_guidance(session.context(),{ViewId::Front,ViewId::Left},"input-a");
    REQUIRE(session.set_upload(ViewId::Left,"replacement.png",true));
    const auto submitted = session.draft();
    REQUIRE(session.set_upload(ViewId::Left,"replacement.png",true));
    auto cleared = original; ++cleared.revision; cleared.input_epoch = "input-b";
    cleared.views[1] = {};
    REQUIRE(session.apply_snapshot(cleared,session.context(),false,&submitted));
    CHECK(session.draft_dirty());
    CHECK_FALSE(session.consume_initial_design_guidance());
    auto success = original; success.revision += 2; success.input_epoch = "input-b";
    const auto current = session.draft();
    success.uploads = current.uploads; success.views[1] = {};
    REQUIRE(session.apply_snapshot(success,session.context(),false,&current));
    session.mark_draft_saved(session.draft().design);
    CHECK_FALSE(session.consume_initial_design_guidance());

    // Saving a smaller input set does not acknowledge a new generation plan.
    REQUIRE(session.set_upload(ViewId::Left, ""));
    const auto removed_input = session.draft();
    auto removed = success; ++removed.revision; removed.input_epoch = "input-c";
    removed.uploads = removed_input.uploads;
    removed.design_targets = {true, false, false, false};
    removed.pending_views.fill(false);
    REQUIRE(session.apply_snapshot(removed, session.context(), false, &removed_input));
    REQUIRE(session.inputs_match_results());
    CHECK_FALSE(session.consume_initial_design_guidance());

    // Only an accepted operation can replace the full guidance target set.
    session.track_initial_design_plan(session.context(), {ViewId::Front}, "input-c");
    auto running = removed; ++running.revision; running.active_operation_id = "new-operation";
    REQUIRE(session.apply_snapshot(running, session.context()));
    CHECK_FALSE(session.consume_initial_design_guidance());
    auto completed = running; ++completed.revision; completed.active_operation_id.clear();
    completed.views[0].version_id = completed.views[0].canonical_version_id = "front-new-plan";
    REQUIRE(session.apply_snapshot(completed, session.context()));
    CHECK(session.consume_initial_design_guidance());
    CHECK_FALSE(session.consume_initial_design_guidance());
}

TEST_CASE("First and newly created advanced designs guide only after their actual targets succeed",
          "[AdvancedCreation][AdvancedFlexibleViews][AdvancedInitialGuidance]")
{
    const int count = GENERATE(1,2,3,4);
    Session session;
    for (int creation = 0; creation < 2; ++creation) {
        session.new_creation();
        auto group = complete_group(); const auto context = session.begin_open(group.id);
        std::vector<ViewId> targets;
        for (int slot = 0; slot < 4; ++slot) {
            group.design_targets[slot] = slot < count;
            if (slot < count) targets.push_back(AllViews[slot]); else group.views[slot] = {};
        }
        session.arm_initial_design_guidance(context,targets,group.input_epoch);
        auto loading = group; loading.active_operation_id = "operation";
        REQUIRE(session.apply_snapshot(loading,context,true));
        CHECK_FALSE(session.consume_initial_design_guidance());
        ++group.revision; REQUIRE(session.apply_snapshot(group,context));
        CHECK(session.consume_initial_design_guidance());
        CHECK_FALSE(session.consume_initial_design_guidance());
    }
}

TEST_CASE("Three successive upload replacements keep only the newest acknowledged design",
          "[AdvancedCreation][AdvancedFlexibleViews][AdvancedDraftInput]")
{
    const bool same_path = GENERATE(false, true);
    auto session = ready_session();
    const auto initial = *session.group();
    std::array<InputDraft, 3> inputs;
    std::array<GroupSnapshot, 3> snapshots;
    for (std::size_t slot = 0; slot < inputs.size(); ++slot) {
        REQUIRE(session.set_upload(ViewId::Left, same_path ? "replacement.png" :
            "replacement-" + std::to_string(slot) + ".png", true));
        inputs[slot] = session.draft();
        snapshots[slot] = initial;
        snapshots[slot].revision += slot + 1;
        snapshots[slot].input_epoch = "replacement-epoch-" + std::to_string(slot);
        snapshots[slot].uploads = inputs[slot].uploads;
        snapshots[slot].views[1].version_id = snapshots[slot].views[1].canonical_version_id =
            "replacement-design-" + std::to_string(slot);
        snapshots[slot].views[1].image_path = "replacement-design-" + std::to_string(slot) + ".png";
        CHECK_FALSE(session.group()->views[1].usable);
    }
    REQUIRE(session.apply_snapshot(snapshots[2], session.context(), false, &inputs[2]));
    REQUIRE(session.group()->views[1].usable);
    const auto newest = session.group()->views[1].version_id;
    const auto command = session.command_context();
    for (std::size_t old = 0; old < 2; ++old) {
        CHECK_FALSE(session.apply_snapshot(snapshots[old], session.context(), false, &inputs[old]));
        CHECK(session.group()->views[1].version_id == newest);
        CHECK(session.draft().upload_revisions[1] == inputs[2].upload_revisions[1]);
        CHECK(session.matches_model_command(command));
    }
}

TEST_CASE("A failed local draft write preserves its checkpoint and keeps invalidated designs hidden",
          "[AdvancedCreation][AdvancedFlexibleViews][AdvancedDraftInput]")
{
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-local-draft-write"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, "https://not-a-local-service.invalid");
    auto group = complete_group();
    group.id = "11111111-1111-4111-8111-111111111111";
    group.uploads[1] = "left-upload.png";
    REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
    host.changed();
    const auto checkpoint = temporary.path() / "advanced-cache" / "drafts" / (group.id + ".json");
    REQUIRE(boost::filesystem::is_regular_file(checkpoint));
    std::ifstream original_file(checkpoint.string());
    const auto original = nlohmann::json::parse(original_file);
    original_file.close();
    const auto blocked = boost::filesystem::path(checkpoint.string() + ".part");
    REQUIRE(boost::filesystem::create_directory(blocked));
    REQUIRE(host.session().set_upload(ViewId::Left, ""));
    host.changed();
    CHECK_FALSE(host.session().group()->views[1].usable);
    CHECK_FALSE(host.session().model_selection()[1]);
    CHECK(host.session().draft_dirty());
    CHECK_FALSE(host.command_pending());
    CHECK_FALSE(host.has_pending_receipt());
    CHECK(host.message().find("草稿暂时无法保存在本地") != std::string::npos);
    std::ifstream preserved_file(checkpoint.string());
    CHECK(nlohmann::json::parse(preserved_file) == original);
    preserved_file.close();

    REQUIRE(boost::filesystem::remove(blocked));
    host.changed();
    std::ifstream recovered_file(checkpoint.string());
    const auto recovered = nlohmann::json::parse(recovered_file);
    CHECK(recovered.at("invalidated_uploads").at(1) == true);
    CHECK(recovered.at("uploads").at(1) == "");
    CHECK(recovered.at("dirty") == true);
    CHECK_FALSE(host.session().group()->views[1].usable);
    CHECK(host.session().draft_dirty());
    CHECK(host.message().empty());
    host.shutdown();
}

TEST_CASE("Delayed image clicks keep their original design or model context",
          "[AdvancedCreation][AdvancedFlexibleViews]")
{
    const auto context = GENERATE(SelectionContext::Design, SelectionContext::Model,
                                 SelectionContext::DesignForNextModel);
    auto session = ready_session();
    session.click(ViewId::Left, 100, 500, context);
    CHECK_FALSE(session.flush_click(300));
    const auto command = session.command_context();
    // A later double click must settle the first card in its captured context.
    REQUIRE(session.double_click(ViewId::Right).has_value());
    CHECK_FALSE(session.flush_click(1000));
    if (context == SelectionContext::Model) {
        CHECK_FALSE(session.model_selection()[1]);
        CHECK_FALSE(session.selection()[1]);
        CHECK(session.matches_command(command));
        CHECK_FALSE(session.matches_model_command(command));
    } else {
        CHECK(session.selection()[1]);
        CHECK_FALSE(session.matches_command(command));
        if (context == SelectionContext::DesignForNextModel)
            CHECK(session.effective_model_views() == std::vector<ViewId>{ViewId::Front, ViewId::Left});
        else CHECK(session.effective_model_views().size() == 4);
    }
    const auto model_selection = session.model_selection();
    const auto design_selection = session.selection();
    session.click(ViewId::Back, 1100, 500, context);
    REQUIRE(session.double_click(ViewId::Back).has_value());
    CHECK_FALSE(session.flush_click(2000));
    CHECK(session.model_selection() == model_selection);
    CHECK(session.selection() == design_selection);
}

TEST_CASE("Returning from the model tab consumes guidance without preventing manual model generation",
          "[AdvancedCreation][AdvancedFlexibleViews][AdvancedInitialGuidance]")
{
    auto session = ready_session(); session.arm_initial_design_guidance(session.context());
    session.dismiss_initial_design_guidance();
    CHECK_FALSE(session.consume_initial_design_guidance());
    session.arm_initial_design_guidance(session.context());
    CHECK_FALSE(session.consume_initial_design_guidance());
    CHECK(session.eligibility({true,true,true,true,true},{true,true,true}).generate_model);
}

TEST_CASE("Repainting a successful initial view cannot bypass another missing initial target",
          "[AdvancedCreation][AdvancedFlexibleViews][AdvancedInitialGuidance]")
{
    Session session;
    auto group = complete_group();
    group.design_targets = {true, true, false, false};
    group.views[1] = {};
    group.views[1].attempt_state = "failed";
    group.views[2] = group.views[3] = {};
    const auto callback = session.begin_open(group.id);
    REQUIRE(session.apply_snapshot(group, callback, true));
    session.arm_initial_design_guidance(callback, {ViewId::Front, ViewId::Left}, group.input_epoch);
    CHECK_FALSE(session.consume_initial_design_guidance());
    session.track_initial_design_plan(callback, {ViewId::Front}, group.input_epoch);
    ++group.revision;
    group.views[0].version_id = group.views[0].canonical_version_id = "front-repainted";
    REQUIRE(session.apply_snapshot(group, callback));
    REQUIRE(session.inputs_match_results());
    CHECK_FALSE(session.consume_initial_design_guidance());
    ++group.revision;
    group.views[1] = complete_group().views[1];
    REQUIRE(session.apply_snapshot(group, callback));
    CHECK(session.consume_initial_design_guidance());
    CHECK_FALSE(session.consume_initial_design_guidance());
}

TEST_CASE("Selected auxiliary failures can be removed in both model selection contexts without losing pending work",
          "[AdvancedCreation][AdvancedFlexibleViews]")
{
    const auto auxiliary=GENERATE(ViewId::Left,ViewId::Back,ViewId::Right);
    const auto context=GENERATE(SelectionContext::Model,SelectionContext::DesignForNextModel);
    const std::string failure=GENERATE("failed","stopped","display","asset");
    const bool keep_other=GENERATE(false,true);
    auto group=complete_group();
    const auto slot=index(auxiliary);
    const std::size_t other=slot==1?2:1;
    for (std::size_t i=1;i<4;++i) if (i!=slot && (!keep_other || i!=other)) group.views[i]={};
    Session session; REQUIRE(session.apply_snapshot(group,session.begin_open(group.id),true));
    session.click(auxiliary,100,10,SelectionContext::Design);
    REQUIRE(session.flush_click(110));
    if (keep_other) {
        session.click(AllViews[other],200,10,SelectionContext::Design);
        REQUIRE(session.flush_click(210));
    }
    auto failed=*session.group(); ++failed.revision;
    auto& image=failed.views[slot];
    if (failure=="failed" || failure=="stopped") {
        image.attempt_state=failure; failed.pending_views[slot]=true;
    } else if (failure=="display") image.display_error=true;
    else { image.asset_pending=true; image.canonical_version_id="unverified-new-version"; }
    REQUIRE(session.apply_snapshot(failed,session.context()));
    Capabilities capabilities {true,true,true,true,true,{true,true,true,true},true};
    CHECK(session.model_selection()[slot]);
    CHECK_FALSE(session.eligibility(capabilities,{true,true,true}).generate_model);
    const auto old_command=session.command_context();
    session.click(auxiliary,300,10,context);
    REQUIRE(session.flush_click(310));
    CHECK_FALSE(session.model_selection()[slot]);
    CHECK_FALSE(session.matches_model_command(old_command));
    CHECK(session.group()->pending_views[slot]==failed.pending_views[slot]);
    if (context==SelectionContext::DesignForNextModel) CHECK_FALSE(session.selection()[slot]);
    std::vector<ViewId> expected {ViewId::Front};
    if (keep_other) expected.push_back(AllViews[other]);
    CHECK(session.effective_model_views()==expected);
    REQUIRE(session.eligibility(capabilities,{true,true,true}).generate_model);
    const auto canceled_command=session.command_context();
    session.click(auxiliary,400,10,context);
    CHECK_FALSE(session.flush_click(410));
    CHECK(session.matches_model_command(canceled_command));
    const auto local=session.local_draft();
    Session reopened; REQUIRE(reopened.apply_snapshot(failed,reopened.begin_open(failed.id),true));
    reopened.restore_local_draft(local);
    CHECK(reopened.effective_model_views()==expected);
    auto recovered=group; recovered.revision=failed.revision+1;
    recovered.views[slot].version_id=recovered.views[slot].canonical_version_id="recovered-version";
    REQUIRE(reopened.apply_snapshot(recovered,reopened.context()));
    CHECK_FALSE(reopened.model_selection()[slot]);
    CHECK(reopened.effective_model_views()==expected);
}

TEST_CASE("Removing an unavailable auxiliary does not bypass model generation guards",
          "[AdvancedCreation][AdvancedFlexibleViews]")
{
    const std::string blocker=GENERATE("active","model","stopping","readonly","options","service",
        "draft","ambiguous","result_pending","result_saved","front");
    auto group=complete_group(); group.views[2]=group.views[3]={};
    Session session; REQUIRE(session.apply_snapshot(group,session.begin_open(group.id),true));
    auto failed=*session.group(); ++failed.revision;
    failed.pending_views[1]=true; failed.views[1].attempt_state="failed";
    REQUIRE(session.apply_snapshot(failed,session.context()));
    session.click(ViewId::Left,100,10,SelectionContext::Model);
    REQUIRE(session.flush_click(110));
    CHECK(session.effective_model_views()==std::vector<ViewId>{ViewId::Front});
    auto blocked=*session.group(); ++blocked.revision;
    HostGuards guards {true,true,true};
    if (blocker=="active") blocked.active_operation_id="operation";
    if (blocker=="model" || blocker=="stopping") blocked.model_busy=true;
    if (blocker=="stopping") blocked.stop_intent=true;
    if (blocker=="readonly") guards.can_start=false;
    if (blocker=="options") guards.model_options_valid=false;
    if (blocker=="service") guards.model_service_available=false;
    if (blocker=="front") blocked.views[0].display_error=true;
    if (blocker=="ambiguous" || blocker=="result_pending" || blocker=="result_saved")
        blocked.views[1].attempt_state=blocker;
    REQUIRE(session.apply_snapshot(blocked,session.context()));
    if (blocker=="draft") {
        auto draft=session.draft().design; draft.prompt="changed";
        REQUIRE(session.set_design_draft(draft));
    }
    CHECK_FALSE(session.eligibility({true,true,true,true,true,{true,true,true,true},true},guards).generate_model);
    session.click(ViewId::Front,200,10,SelectionContext::Model);
    CHECK_FALSE(session.flush_click(210));
    CHECK(session.model_selection()[0]);
}

TEST_CASE("Hidden auxiliary slots and ordinary repaint clicks retain their existing selection rules",
          "[AdvancedCreation][AdvancedFlexibleViews]")
{
    const bool replacement=GENERATE(false,true);
    auto session=ready_session();
    auto uploaded=*session.group(); uploaded.uploads[1]="original-left.png";
    REQUIRE(session.apply_snapshot(uploaded,session.context(),true));
    REQUIRE(session.set_upload(ViewId::Left,replacement?"replacement.png":"",replacement));
    const auto before=session.local_draft();
    const auto command=session.command_context();
    session.click(ViewId::Left,100,10,SelectionContext::Model);
    CHECK_FALSE(session.flush_click(110));
    CHECK(session.local_draft().model_selected==before.model_selected);
    CHECK(session.matches_model_command(command));
    session.click(ViewId::Left,200,10,SelectionContext::DesignForNextModel);
    CHECK_FALSE(session.flush_click(210));
    CHECK(session.local_draft().model_selected==before.model_selected);
    auto ordinary=ready_session();
    auto pending=*ordinary.group(); ++pending.revision;
    pending.pending_views[1]=true; pending.views[1].attempt_state="failed";
    REQUIRE(ordinary.apply_snapshot(pending,ordinary.context(),true));
    REQUIRE(ordinary.selection()[1]);
    ordinary.click(ViewId::Left,300,10,SelectionContext::Design);
    CHECK_FALSE(ordinary.flush_click(310));
    CHECK(ordinary.selection()[1]);
    CHECK(ordinary.model_selection()[1]);
}

TEST_CASE("Unselected ordinary auxiliary failure does not block modeling but unresolved submission does",
          "[AdvancedCreation][AdvancedFlexibleViews]")
{
    const std::string state = GENERATE("failed","stopped","blocked","ambiguous","result_pending","result_saved");
    auto group = complete_group(); group.views[2] = {}; group.views[2].attempt_state = state;
    group.pending_views[2] = true;
    Session session; REQUIRE(session.apply_snapshot(group,session.begin_open(group.id),true));
    Capabilities capabilities {true,true,true,true,true,{true,true,true,true},true};
    const auto allowed = session.eligibility(capabilities,{true,true,true});
    CHECK(allowed.generate_model == (state == "failed" || state == "stopped" || state == "blocked"));
    CHECK_FALSE(session.model_selection()[2]);
}

TEST_CASE("Advanced interaction guards survive missing views and distinguish unresolved actions",
          "[AdvancedCreation][GenerationClickFeedback]")
{
    const std::string blocker = GENERATE("workbench", "active", "model", "ambiguous", "result_pending", "result_saved", "history");
    Session session;
    auto group = complete_group(); ++group.revision;
    group.views[0] = {}; // No previously verified visible base for this slot.
    HostGuards guards {true,true,true};
    if (blocker == "workbench") guards.can_start = false;
    else if (blocker == "active") group.active_operation_id = "active";
    else if (blocker == "model") group.model_busy = true;
    else if (blocker != "history") group.views[1].attempt_state = blocker;
    REQUIRE(session.apply_snapshot(group,session.begin_open(group.id),true));
    REQUIRE_FALSE(session.group()->views[0].usable);
    if (blocker == "history") session.begin_open("different-group");
    const auto eligible = session.eligibility({true,true,true,true,true},guards);
    const bool unresolved = blocker == "ambiguous" || blocker == "result_pending" || blocker == "result_saved";
    CHECK(eligible.click_generate_design == unresolved);
    CHECK_FALSE(eligible.click_generate_model);
    CHECK_FALSE(eligible.generate_model);
    if (blocker == "workbench") CHECK(eligible.model_reason == "missing_current_view");
    if (unresolved) CHECK(eligible.generate_design);
}

TEST_CASE("Advanced byte boundaries and selection flushing keep their original command semantics",
          "[AdvancedCreation][GenerationClickFeedback]")
{
    Session session;
    auto draft = session.draft().design;
    for (int i=0;i<666;++i) draft.prompt += "猫";
    draft.prompt += "ab"; draft.style = "custom"; draft.custom_style.assign(1000,'a');
    REQUIRE(draft.prompt.size() == 2000);
    session.set_design_draft(draft);
    REQUIRE(session.eligibility({true,true,true,true,true},{true,true,true}).generate_design);
    draft.custom_style += "a"; session.set_design_draft(draft);
    CHECK_FALSE(session.eligibility({true,true,true,true,true},{true,true,true}).generate_design);
    CHECK(session.eligibility({true,true,true,true,true},{true,true,true}).click_generate_design);
    session = ready_session();
    session.click(ViewId::Left,100,500);
    CHECK_FALSE(session.selection()[index(ViewId::Left)]);
    CHECK(session.flush_click(600));
    CHECK(session.selection()[index(ViewId::Left)]);
    CHECK(session.eligibility({true,true,true,true,true},{true,true,true}).generate_model);
}

TEST_CASE("Advanced incomplete or stale results accept feedback clicks but never authorize model generation",
          "[AdvancedCreation][GenerationClickFeedback]")
{
    const std::string invalid = GENERATE("missing", "display", "asset", "version", "pending", "draft", "capability");
    Session session;
    auto group = complete_group(); ++group.revision;
    if (invalid == "missing") group.views[0].usable = false;
    if (invalid == "display") group.views[0].display_error = true;
    if (invalid == "asset") group.views[0].asset_pending = true;
    if (invalid == "version") group.views[0].canonical_version_id = "front-v2";
    if (invalid == "pending") group.pending_views[0] = true;
    REQUIRE(session.apply_snapshot(group,session.begin_open(group.id),true));
    if (invalid == "missing") REQUIRE_FALSE(session.group()->views[0].usable);
    if (invalid == "draft") {
        auto draft = session.draft().design; draft.prompt = "changed";
        REQUIRE(session.set_design_draft(draft));
    }
    Capabilities capabilities {true,true,true,true,invalid != "capability"};
    const auto eligible = session.eligibility(capabilities,{true,true,true});
    CHECK(eligible.click_generate_model);
    CHECK_FALSE(eligible.generate_model);
    CHECK_FALSE(eligible.model_reason.empty());
    CHECK(session.group()->id == group.id);
}

TEST_CASE("Advanced rejected clicks do not create commands groups receipts or confirmation dialogs",
          "[AdvancedCreation][GenerationClickFeedback]")
{
    const std::string blocker = GENERATE("none", "readonly", "viewport-loading");
    const bool readonly = blocker == "readonly";
    bool viewport_loading = blocker == "viewport-loading";
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-generation-rejected-click"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR",temporary.path().string());
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr,"https://not-a-local-service.invalid");
    host.set_editing_guard([&] { return !readonly; });
    host.set_generation_guard([&] { return !viewport_loading; });
    const auto context = host.session().context();
    int confirmed = 0, rejected = 0;
    const auto confirm = [&](const auto&) { ++confirmed; return true; };
    const auto reject = [&](const std::string& reason) { ++rejected; CHECK_FALSE(reason.empty()); };
    for (int click = 0; click < 3; ++click) {
        host.generate_design(confirm,reject);
        host.generate_model(confirm,reject);
    }
    CHECK(confirmed == 0);
    CHECK(rejected == 6);
    CHECK_FALSE(host.command_pending());
    CHECK_FALSE(host.has_pending_receipt());
    CHECK_FALSE(host.session().group().has_value());
    CHECK(host.session().matches(context));
    if (readonly || viewport_loading) {
        CHECK_FALSE(host.eligibility().click_generate_design);
        CHECK_FALSE(host.eligibility().click_generate_model);
    }
    if (!readonly) {
        viewport_loading = false;
        CHECK(host.eligibility().click_generate_design);
        CHECK(host.eligibility().click_generate_model);
    }
    host.shutdown();
    CHECK_FALSE(host.eligibility().click_generate_design);
}

TEST_CASE("An accepted initial design guides once while historical snapshots do not", "[AdvancedCreation][AdvancedInitialGuidance]")
{
    auto session = ready_session();
    CHECK_FALSE(session.consume_initial_design_guidance());
    session.arm_initial_design_guidance(session.context());
    REQUIRE(session.consume_initial_design_guidance());
    CHECK_FALSE(session.consume_initial_design_guidance());
    session.arm_initial_design_guidance(session.context());
    CHECK_FALSE(session.consume_initial_design_guidance());
    auto snapshot = complete_group();
    ++snapshot.revision;
    REQUIRE(session.apply_snapshot(snapshot, session.context()));
    CHECK_FALSE(session.consume_initial_design_guidance());
}

TEST_CASE("An initial design guides only after its missing views become usable", "[AdvancedCreation][AdvancedInitialGuidance]")
{
    const auto usable_count = GENERATE(0, 1, 2, 3);
    Session session;
    const auto context = session.begin_open("group-a");
    session.arm_initial_design_guidance(context);
    auto snapshot = complete_group();
    for (std::size_t i = usable_count; i < snapshot.views.size(); ++i)
        snapshot.views[i] = {};
    REQUIRE(session.apply_snapshot(snapshot, context, true));
    CHECK_FALSE(session.consume_initial_design_guidance());
    snapshot = complete_group();
    ++snapshot.revision;
    REQUIRE(session.apply_snapshot(snapshot, context));
    REQUIRE(session.consume_initial_design_guidance());
    CHECK_FALSE(session.consume_initial_design_guidance());
}

TEST_CASE("Unresolved design attempts and retained old images cannot trigger initial guidance", "[AdvancedCreation][AdvancedInitialGuidance]")
{
    const std::string blocker = GENERATE("queued", "submitting", "failed", "blocked", "stopped",
        "ambiguous", "result_pending", "result_saved", "active", "stop", "pending",
        "asset", "display", "version");
    auto session = ready_session();
    session.arm_initial_design_guidance(session.context());
    auto snapshot = complete_group();
    ++snapshot.revision;
    auto& view = snapshot.views[0];
    if (blocker == "active") snapshot.active_operation_id = "initial-operation";
    else if (blocker == "stop") snapshot.stop_intent = true;
    else if (blocker == "pending") snapshot.pending_views[0] = true;
    else if (blocker == "asset") {
        view.usable = false; view.asset_pending = true;
        view.canonical_version_id = "front-v2";
    } else if (blocker == "display") view.display_error = true;
    else if (blocker == "version") view.canonical_version_id = "front-v2";
    else view.attempt_state = blocker;
    REQUIRE(session.apply_snapshot(snapshot, session.context()));
    CHECK_FALSE(session.consume_initial_design_guidance());
    snapshot = complete_group();
    snapshot.revision += 2;
    REQUIRE(session.apply_snapshot(snapshot, session.context()));
    CHECK(session.consume_initial_design_guidance());
}

TEST_CASE("Initial guidance waits for the current draft and survives hidden observation", "[AdvancedCreation][AdvancedInitialGuidance]")
{
    auto session = ready_session();
    session.arm_initial_design_guidance(session.context());
    auto draft = session.draft().design;
    draft.prompt = "a revised design";
    REQUIRE(session.set_design_draft(draft));
    CHECK_FALSE(session.consume_initial_design_guidance());
    auto snapshot = complete_group();
    snapshot.saved_draft = draft;
    snapshot.latest_submitted_draft = draft;
    ++snapshot.revision;
    REQUIRE(session.apply_snapshot(snapshot, session.context()));
    // Background snapshots update the Session without consuming navigation.
    ++snapshot.revision;
    REQUIRE(session.apply_snapshot(snapshot, session.context()));
    CHECK(session.consume_initial_design_guidance());
    CHECK_FALSE(session.consume_initial_design_guidance());
}

TEST_CASE("Opening another context invalidates its predecessor's initial guidance", "[AdvancedCreation][AdvancedInitialGuidance]")
{
    auto session = ready_session();
    const auto old_context = session.context();
    session.arm_initial_design_guidance(old_context);
    const auto context = session.begin_open("group-b");
    session.arm_initial_design_guidance(old_context);
    CHECK_FALSE(session.apply_snapshot(complete_group(), old_context));
    auto snapshot = complete_group();
    snapshot.id = "group-b";
    REQUIRE(session.apply_snapshot(snapshot, context, true));
    CHECK_FALSE(session.consume_initial_design_guidance());
    session.arm_initial_design_guidance(context);
    CHECK(session.consume_initial_design_guidance());
    session.new_creation();
    CHECK_FALSE(session.consume_initial_design_guidance());
}

TEST_CASE("Initial design guidance is independent of model provider availability", "[AdvancedCreation][AdvancedInitialGuidance]")
{
    auto session = ready_session();
    Capabilities capabilities;
    HostGuards guards;
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_model);
    session.arm_initial_design_guidance(session.context());
    CHECK(session.consume_initial_design_guidance());
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_model);
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

TEST_CASE("Returning to the submitted image provider restores its model and four view eligibility",
          "[AdvancedCreation][AdvancedDraftInput][Regression]")
{
    const std::string provider = GENERATE("gpt", "yoyoclaw");
    auto group = complete_group();
    group.saved_draft.provider = provider;
    group.saved_draft.model = provider == "gpt" ? "gpt-image-2" : "image-model-library/Qwen-Image-3.0-PC-YOYO";
    group.latest_submitted_draft = group.saved_draft;
    Session session;
    REQUIRE(session.apply_snapshot(group, session.begin_open(group.id), true));
    REQUIRE(session.toggle(ViewId::Left));
    const auto selection = session.selection();
    const auto geometry = session.draft().geometry;
    const auto original_command = session.command_context();
    const Capabilities capabilities {true, true, true, true, true};
    const HostGuards guards {true, true, true};
    REQUIRE(session.eligibility(capabilities, guards).generate_model);

    REQUIRE(session.set_design_provider(provider == "gpt" ? "yoyoclaw" : "gpt"));
    CHECK(session.draft().design.model.empty());
    CHECK_FALSE(session.inputs_match_results());
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_model);
    REQUIRE(session.set_design_provider(provider));
    CHECK(session.draft().design == group.saved_draft);
    CHECK(session.inputs_match_results());
    CHECK(session.eligibility(capabilities, guards).generate_model);
    CHECK(session.selection() == selection);
    CHECK(session.draft().geometry.provider == geometry.provider);
    CHECK(session.draft().geometry.face_limit == geometry.face_limit);
    CHECK_FALSE(session.matches_command(original_command));
    CHECK(session.draft_dirty());
    const auto current_command = session.command_context();
    CHECK_FALSE(session.set_design_provider(provider));
    CHECK(session.matches_command(current_command));
    CHECK(session.draft().design.model == group.saved_draft.model);
}

TEST_CASE("Restoring the submitted provider model preserves independent input changes",
          "[AdvancedCreation][AdvancedDraftInput][Regression]")
{
    const std::string changed = GENERATE("prompt", "style", "upload");
    auto group = complete_group();
    group.saved_draft.model = "gpt-image-2";
    group.latest_submitted_draft = group.saved_draft;
    Session session;
    REQUIRE(session.apply_snapshot(group, session.begin_open(group.id), true));
    REQUIRE(session.set_design_provider("yoyoclaw"));
    auto draft = session.draft().design;
    if (changed == "prompt") draft.prompt = "A changed object";
    else if (changed == "style") { draft.style = "custom"; draft.custom_style = "New sculpted style"; }
    else REQUIRE(session.set_upload(ViewId::Front, "replacement.png"));
    if (changed != "upload") REQUIRE(session.set_design_draft(draft));
    REQUIRE(session.set_design_provider("gpt"));
    CHECK(session.draft().design.model == "gpt-image-2");
    if (changed == "prompt") CHECK(session.draft().design.prompt == draft.prompt);
    else if (changed == "style") CHECK(session.draft().design.custom_style == draft.custom_style);
    else CHECK(session.draft().uploads[0] == "replacement.png");
    CHECK_FALSE(session.inputs_match_results());
    const Capabilities capabilities {true, true, true, true, true};
    const HostGuards guards {true, true, true};
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_model);
    CHECK(session.eligibility(capabilities, guards).model_reason ==
          (changed == "upload" ? "missing_current_view" : "draft_changed"));
}

TEST_CASE("Image provider model restoration uses only the current group's submitted design",
          "[AdvancedCreation][AdvancedDraftInput][Regression]")
{
    auto group = complete_group();
    group.saved_draft.model = "gpt-image-2";
    group.latest_submitted_draft = group.saved_draft;
    Session session;
    REQUIRE(session.apply_snapshot(group, session.begin_open(group.id), true));
    session.new_creation();
    REQUIRE(session.set_design_provider("yoyoclaw"));
    REQUIRE(session.set_design_provider("gpt"));
    CHECK(session.draft().design.model.empty());

    group.id = "group-b";
    group.saved_draft.model = "other-submitted-model";
    group.latest_submitted_draft = group.saved_draft;
    REQUIRE(session.apply_snapshot(group, session.begin_open(group.id), true));
    REQUIRE(session.set_design_provider("yoyoclaw"));
    REQUIRE(session.set_design_provider("gpt"));
    CHECK(session.draft().design.model == "other-submitted-model");

    group.id = "group-c";
    group.latest_submitted_draft.reset();
    REQUIRE(session.apply_snapshot(group, session.begin_open(group.id), true));
    REQUIRE(session.set_design_provider("yoyoclaw"));
    REQUIRE(session.set_design_provider("gpt"));
    CHECK(session.draft().design.model.empty());
    CHECK_FALSE(session.inputs_match_results());
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

TEST_CASE("Capability discovery errors hide raw transport explanations in either scope",
          "[AdvancedCreation][ModelGenerationStatusText][Regression]")
{
    const std::string scope = GENERATE("image", "geometry");
    wxInitializer init;
    REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-capability-errors"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.path().string());
    // The production client's endpoint guard rejects this before any networking.
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, "https://not-a-local-service.invalid");
    wxEventLoop loop;
    wxEventLoopActivator active(&loop);
    host.set_service_available(true);
    wxTheApp->ProcessPendingEvents();
    const auto context = host.session().context();
    std::vector<std::string> messages;
    host.set_listener([&] { messages.push_back(host.message()); });
    const auto before = messages.size();
    host.refresh_capabilities(scope);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (messages.size() == before && std::chrono::steady_clock::now() < deadline) {
        wxTheApp->ProcessPendingEvents();
        loop.DispatchTimeout(20);
    }
    REQUIRE(messages.size() > before);
    for (std::size_t i = before; i < messages.size(); ++i) {
        CHECK_FALSE(messages[i].empty());
        CHECK(messages[i].find("Advanced creation requires") == std::string::npos);
        CHECK(messages[i].find("not-a-local-service.invalid") == std::string::npos);
    }
    CHECK(host.session().matches(context));
    CHECK_FALSE(host.command_pending());
    CHECK_FALSE(host.eligibility().generate_design);
    CHECK_FALSE(host.eligibility().generate_model);
    host.shutdown();
}

TEST_CASE("Opening history or starting a creation releases the previous model stop observation",
          "[AdvancedCreation][AdvancedStopNavigation][Regression]")
{
    const bool fresh = GENERATE(false, true);
    wxInitializer init;
    REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-stop-navigation"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.path().string());
    wxIPV4address address;
    REQUIRE(address.Hostname("127.0.0.1"));
    REQUIRE(address.Service(0));
    wxSocketServer server(address, wxSOCKET_NOWAIT);
    REQUIRE(server.IsOk());
    REQUIRE(server.GetLocal(address));
    const std::string endpoint = "http://127.0.0.1:" + std::to_string(address.Service());
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, endpoint);
    auto group = complete_group();
    group.id = "11111111-1111-4111-8111-111111111111";
    group.model_job_id = "22222222-2222-4222-8222-222222222222";
    group.model_state = "running";
    group.model_busy = true;
    REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
    REQUIRE(host.eligibility().stop);
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
    // The production client uses loopback only. No provider or generation runs.
    host.stop();
    REQUIRE(host.command_pending());
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
    group.model_busy = false;
    group.model_state = "stopped";
    ++group.revision;
    REQUIRE(host.session().apply_snapshot(group, host.session().context()));
    // Keep the stale observation rather than synthesizing monitor callbacks.
    REQUIRE(host.draft_editing_locked());
    const auto previous = host.session().context();
    if (fresh) host.new_creation();
    else host.open_group(group.id);
    CHECK_FALSE(host.session().matches(previous));
    CHECK_FALSE(host.command_pending());
    CHECK_FALSE(host.draft_editing_locked());
    CHECK_FALSE(host.eligibility().stop);
    CHECK_FALSE(host.has_pending_receipt());
    if (fresh) CHECK_FALSE(host.session().group());
    else {
        REQUIRE(host.session().apply_snapshot(group, host.session().context(), true));
        CHECK_FALSE(host.draft_editing_locked());
        CHECK_FALSE(host.eligibility().stop);
        CHECK(host.eligibility().model_reason == "service_unavailable");
    }
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

TEST_CASE("The shared workflow lock rejects advanced draft replacement until explicit return",
          "[AdvancedCreation][CreationWorkflow]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-workflow-lock"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.path().string());
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, "http://127.0.0.1:1");
    host.session() = ready_session();
    const auto context = host.session().context();
    const auto original = host.session().draft();
    const auto selection = host.session().selection();
    host.session().click(ViewId::Left, 100, 500);
    bool unlocked = false;
    host.set_editing_guard([&] { return unlocked; });
    CHECK(host.draft_editing_locked());
    CHECK_FALSE(host.eligibility().generate_design);
    CHECK_FALSE(host.eligibility().generate_model);
    host.new_creation();
    CHECK(host.session().matches(context));
    CHECK(host.session().draft().design == original.design);
    CHECK(host.session().draft().uploads == original.uploads);
    const auto& geometry = host.session().draft().geometry;
    CHECK(geometry.provider == original.geometry.provider);
    CHECK(geometry.model == original.geometry.model);
    CHECK(geometry.face_limit == original.geometry.face_limit);
    CHECK(geometry.geometry_quality == original.geometry.geometry_quality);
    CHECK(geometry.texture_quality == original.geometry.texture_quality);
    CHECK(geometry.output_format == original.geometry.output_format);
    host.open_group("another-group");
    host.rename_group("group-a", 4, "changed name");
    host.generate_design({}); host.generate_model({});
    CHECK(host.session().selection() == selection);
    CHECK(host.session().matches(context));
    CHECK_FALSE(host.command_pending());
    unlocked = true;
    CHECK_FALSE(host.draft_editing_locked());
    host.new_creation();
    CHECK_FALSE(host.session().matches(context));
    host.shutdown();
}
