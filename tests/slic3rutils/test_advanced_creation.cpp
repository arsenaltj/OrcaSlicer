#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/GUI/AI/ModelGeneration/AdvancedCreationSession.hpp"
#include "slic3r/GUI/AI/ModelGeneration/AdvancedCreationFeatureHost.hpp"
#include "slic3r/GUI/AI/ModelGeneration/AdvancedMultiviewClient.hpp"
#include "slic3r/GUI/AI/Text/AITextFeatureHost.hpp"
#include "slic3r/GUI/Redesign/RedesignModelRoute.hpp"
#include "slic3r/GUI/Redesign/GenerationParticleImage.hpp"
#include "slic3r/GUI/Redesign/AdvancedPreviewGesture.hpp"
#include "slic3r/GUI/Redesign/AdvancedPreviewLayout.hpp"
#include "../test_utils.hpp"
#include <wx/app.h>
#include <wx/evtloop.h>
#include <wx/init.h>
#include <wx/image.h>
#include <wx/socket.h>
#include <openssl/evp.h>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <array>
#include <wx/mstream.h>
#include <wx/imagpng.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <chrono>
#include <atomic>

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

// Exercise the real asynchronous client without the application sidecar or a
// supplier. Selected responses stay open until the test delivers them.
class AdvancedHttpProbe {
public:
    using Json = nlohmann::json;
    struct Request {
        std::string path;
        Json payload;
        std::unique_ptr<wxSocketBase> connection;
    };
    AdvancedHttpProbe()
    {
        REQUIRE(address.Hostname("127.0.0.1")); REQUIRE(address.Service(0));
        server = std::make_unique<wxSocketServer>(address, wxSOCKET_NOWAIT);
        REQUIRE(server->IsOk()); REQUIRE(server->GetLocal(address));
    }
    std::string endpoint() const { return "http://127.0.0.1:" + std::to_string(address.Service()); }
    static bool ends_with(const std::string& value, const std::string& suffix)
    { return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0; }
    std::size_t count(const std::string& suffix) const
    { return std::count_if(requests.begin(), requests.end(), [&](const auto& request) { return ends_with(request.path, suffix); }); }
    std::size_t at(const std::string& suffix, std::size_t occurrence = 0) const
    {
        for (std::size_t i = 0; i < requests.size(); ++i)
            if (ends_with(requests[i].path, suffix) && occurrence-- == 0) return i;
        FAIL("Expected an observed HTTP request"); return 0;
    }
    void reply(std::size_t index, const Json& value, bool failed = false)
    { reply_bytes(index, value.dump(), "application/json", failed); }
    void reply_bytes(std::size_t index, const std::string& body, const std::string& content_type, bool failed = false)
    {
        auto& request = requests.at(index); REQUIRE(request.connection);
        const auto response = std::string("HTTP/1.1 ") + (failed ? "503 Service Unavailable" : "200 OK") +
            "\r\nContent-Type: " + content_type + "\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        request.connection->Write(response.data(), response.size());
        REQUIRE(request.connection->LastCount() == response.size());
        request.connection->Close(); request.connection.reset();
    }
    void drop(std::size_t index)
    {
        auto& connection = requests.at(index).connection;
        if (connection) { connection->Close(); connection.reset(); }
    }
    void pump()
    {
        if (auto* accepted = server->Accept(false)) {
            std::unique_ptr<wxSocketBase> connection(accepted);
            connection->SetFlags(wxSOCKET_WAITALL); connection->SetTimeout(2);
            std::string header; char byte = 0;
            while (header.find("\r\n\r\n") == std::string::npos && header.size() < 16384) {
                connection->Read(&byte, 1); REQUIRE(connection->LastCount() == 1); header += byte;
            }
            const auto length_at = header.find("Content-Length:");
            const auto length = length_at == std::string::npos ? 0 :
                std::stoul(header.substr(length_at + std::string("Content-Length:").size()));
            std::string body(length, '\0');
            if (length) { connection->Read(body.data(), length); REQUIRE(connection->LastCount() == length); }
            const auto first = header.find(' '), last = header.find(' ', first + 1);
            const auto path = header.substr(first + 1, last - first - 1);
            auto payload = body.empty() ? Json::object() : Json::parse(body, nullptr, false);
            // Input saves use the real client's multipart upload, while plans
            // and commands use JSON. Preserve multipart bytes for assertions.
            if (payload.is_discarded()) payload = body;
            requests.push_back({path, std::move(payload), std::move(connection)});
            if (!ends_with(path, "/model-plans") && !ends_with(path, "/plans") &&
                !ends_with(path, "/model-jobs") && !ends_with(path, "/operations") && !ends_with(path, "/reconcile") &&
                !ends_with(path, "/stop") && std::none_of(held_suffixes.begin(), held_suffixes.end(),
                    [&](const auto& suffix) { return ends_with(path, suffix); }))
                reply(requests.size() - 1, capabilities);
        }
        wxTheApp->ProcessPendingEvents(); loop.DispatchTimeout(10);
    }
    template<class Ready> bool until(Ready ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ready() && std::chrono::steady_clock::now() < deadline) pump();
        return ready();
    }
    void drain()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
        while (std::chrono::steady_clock::now() < deadline) pump();
    }
    Json model_plan(std::size_t request, const std::string& group) const
    {
        const auto& input = requests.at(request).payload;
        Json snapshot {{"schema", "approved-multiview-input-v2"}, {"group_id", group},
            {"snapshot_id", input.at("command_id")}, {"input_epoch", group},
            {"selected_views", input.at("selected_views")}, {"views", input.at("visible_versions")},
            {"input_mode", input.at("selected_views").size() == 1 ? "image" : "multiview"}};
        for (const auto& view : input.at("selected_views"))
            snapshot["views"][view.get<std::string>()]["path"] = "inputs/" + view.get<std::string>() + ".png";
        Json options = Json::object();
        for (const auto* key : {"provider", "geometry_model", "face_limit", "geometry_quality", "texture_quality", "output_format"})
            options[key] = input.at(key);
        if (options.at("geometry_model").get<std::string>().empty()) options["geometry_model"] = "v3.0";
        const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        return {{"schema", Slic3r::GUI::AdvancedMultiviewClient::selected_plan_schema}, {"id", input.at("command_id")},
            {"job_id", input.at("command_id")}, {"group_id", group},
            {"options", options}, {"approved_multiview_snapshot", snapshot}, {"max_new_image_calls", 0},
            {"max_new_model_tasks", 1}, {"max_new_conversion_tasks", 0}, {"expires_at", now + 300},
            {"confirmation_fingerprint", std::string(64, 'e')}};
    }
    Json local_plan(std::size_t request, const GroupSnapshot& group) const
    {
        const auto& input = requests.at(request).payload;
        const auto target = input.at("design_request").at("target_view").get<std::string>();
        return {{"id", input.at("command_id")}, {"group_id", group.id}, {"kind", input.at("kind")},
            {"expected_group_revision", input.at("expected_group_revision")}, {"design_request", input.at("design_request")},
            {"targets", Json::array({target})}, {"base_versions", input.at("base_versions")},
            {"target_modes", {{target, "edit"}}}, {"dependency_views", Json::array()},
            {"max_new_image_calls", 1}, {"max_new_model_tasks", 0},
            {"frozen", {{"input_epoch", group.input_epoch}, {"design_targets", Json::array({"front", "left", "back", "right"})}}},
            {"confirmation_fingerprint", std::string(64, 'd')}};
    }
    Json capabilities = {
        {"schema", "advanced-multiview-capabilities-v1"}, {"model_plans", true}, {"groups", Json::array()},
        {"image_models", Json::array({{{"provider", "gpt"}, {"model", "gpt-image-2"}, {"initial_text", "supported"},
            {"initial_named_references", "supported"}, {"edit_current_with_context", "supported"}}})},
        {"model_providers", {{"tripo", {{"available", true}, {"named_four_view_model", "supported"},
            {"supported_image_counts", Json::array({1, 2, 3, 4})}}}}}};
    std::vector<Request> requests;
    std::vector<std::string> held_suffixes;
private:
    wxIPV4address address;
    std::unique_ptr<wxSocketServer> server;
    wxEventLoop loop;
    wxEventLoopActivator active {&loop};
};

GroupSnapshot host_test_group(unsigned suffix)
{
    auto group = complete_group();
    group.id = "55555555-5555-4555-8555-" + std::to_string(100000000000ULL + suffix);
    group.input_epoch = group.id;
    for (std::size_t i = 0; i < group.views.size(); ++i)
        group.views[i].sha256 = std::string(64, static_cast<char>('a' + i));
    return group;
}
}

TEST_CASE("Preview navigation consumes a rapid click once across relayout",
          "[AdvancedCreation][AdvancedPreviewGesture]")
{
    Slic3r::GUI::AdvancedPreviewClickSequence clicks;
    // The first release switches Grid to Single immediately. Its second
    // release must not activate the thumbnail now occupying the same position.
    REQUIRE(clicks.accept(100, 603, 939, 500, 4));
    CHECK_FALSE(clicks.accept(120, 603, 939, 500, 4));
    CHECK_FALSE(clicks.accept(150, 605, 937, 500, 4));
    // A deliberate click at another thumbnail remains immediate.
    CHECK(clicks.accept(160, 647, 939, 500, 4));
    CHECK_FALSE(clicks.accept(180, 647, 939, 500, 4));
    CHECK(clicks.accept(661, 647, 939, 500, 4));
}

TEST_CASE("A new creation resets the preview click sequence without delaying input",
          "[AdvancedCreation][AdvancedPreviewGesture]")
{
    Slic3r::GUI::AdvancedPreviewClickSequence clicks;
    REQUIRE(clicks.accept(100, 603, 939, 500, 4));
    clicks.reset();
    CHECK(clicks.accept(101, 603, 939, 500, 4));
    CHECK_FALSE(clicks.accept(102, 603, 939, 500, 4));
    // Do not retain a lock if the monotonic clock input is reset.
    CHECK(clicks.accept(1, 603, 939, 500, 4));
}

TEST_CASE("Preview cards fill their sizing budget proportionally and center in the viewport",
          "[AdvancedCreation][AdvancedPreviewLayout]")
{
    using namespace Slic3r::GUI;
    const wxSize viewport = GENERATE(wxSize(900, 950), wxSize(600, 451), wxSize(275, 811));
    const wxSize available(viewport.x, viewport.y - 8);
    const auto single = center_design_preview(viewport, fit_design_preview(available, {548, 740}));
    CHECK(wxRect(wxPoint{}, viewport).Contains(single));
    CHECK(single.height <= available.y);
    CHECK(single.width <= 548);
    CHECK(single.height <= 740);
    CHECK(std::abs(single.width * 740 - single.height * 548) <= 740);
    CHECK(std::abs(single.GetTop() - (viewport.y - single.GetBottom() - 1)) <= 1);

    const auto grid = design_preview_grid(viewport, available.y, {260, 350}, 8);
    for (const auto& card : grid) {
        CHECK(wxRect(wxPoint{}, viewport).Contains(card));
        CHECK(card.GetSize() == grid.front().GetSize());
        CHECK(card.width <= 260);
        CHECK(card.height <= 350);
        CHECK(std::abs(card.width * 350 - card.height * 260) <= 350);
    }
    CHECK(grid[1].GetLeft() - grid[0].GetRight() - 1 == 8);
    CHECK(grid[2].GetTop() - grid[0].GetBottom() - 1 == 8);
    CHECK(grid.back().GetBottom() - grid.front().GetTop() + 1 <= available.y);
    CHECK(std::abs(grid.front().GetTop() - (viewport.y - grid.back().GetBottom() - 1)) <= 1);
}

TEST_CASE("The thumbnail strip stays inside a narrow preview and leaves the star clickable",
          "[AdvancedCreation][AdvancedPreviewLayout]")
{
    using namespace Slic3r::GUI;
    const int scale = GENERATE(1, 2);
    const int width = GENERATE(548, 210, 132);
    const wxSize size(width * scale, width * 740 / 548 * scale);
    const auto controls = design_preview_controls(size, true, [scale](int dip) { return scale * dip; });
    REQUIRE(controls.regenerate.GetSize() == wxSize(24 * scale, 24 * scale));
    for (const auto& thumb : controls.thumbnails) {
        CHECK_FALSE(thumb.IsEmpty());
        CHECK(wxRect(wxPoint{}, size).Contains(thumb));
        CHECK_FALSE(thumb.Intersects(controls.regenerate));
        CHECK(thumb.GetBottom() == size.y - 12 * scale - 1);
    }
    CHECK(std::abs(controls.thumbnails.front().GetLeft() - (size.x - controls.thumbnails.back().GetRight() - 1)) <= 1);
    CHECK(controls.thumbnails[1].GetLeft() - controls.thumbnails[0].GetRight() - 1 == 4 * scale);
}

TEST_CASE("Model check corners reserve their own hit target within each preview thumbnail",
          "[AdvancedCreation][AdvancedPreviewLayout]")
{
    using namespace Slic3r::GUI;
    const int scale = GENERATE(100, 150, 200);
    const int width = GENERATE(548, 132, 68);
    const auto dip = [scale](int n) { return (n * scale + 50) / 100; };
    const auto controls = design_preview_controls({dip(width), dip(width * 740 / 548)}, true, dip);
    for (size_t i = 0; i < controls.thumbnails.size(); ++i) {
        const auto& thumb = controls.thumbnails[i];
        const auto& corner = controls.thumbnail_checks[i];
        REQUIRE_FALSE(corner.IsEmpty());
        CHECK(thumb.Contains(corner));
        CHECK_FALSE(corner.Intersects(controls.regenerate));
        CHECK_FALSE(corner.Contains(thumb.GetTopLeft()));
        const auto checkbox = controls.thumbnail_at(thumb.GetTopRight(), true);
        REQUIRE(checkbox);
        CHECK(checkbox->index == i);
        CHECK(checkbox->selection);
        const auto body = controls.thumbnail_at(thumb.GetTopLeft(), true);
        REQUIRE(body);
        CHECK(body->index == i);
        CHECK_FALSE(body->selection);
        const auto design = controls.thumbnail_at(thumb.GetTopRight(), false);
        REQUIRE(design);
        CHECK(design->index == i);
        CHECK_FALSE(design->selection);
        CHECK_FALSE(controls.thumbnail_at(thumb.GetTopRight() + wxPoint(1, 0), true));
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
    if (invalid == "front") {
        auto local = session.local_draft(); local.dirty = true; local.input.uploads[1] = "left.png";
        session.restore_local_draft(local); reason = "missing_front";
    }
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

TEST_CASE("Full design scope follows inputs independently of repaint and model selections",
          "[AdvancedCreation][Advanced2DPreview]")
{
    auto session = ready_session();
    REQUIRE(session.toggle(ViewId::Left));
    session.click(ViewId::Right,100,1,SelectionContext::Model); REQUIRE(session.flush_click(101));
    CHECK(session.all_design_views() == std::vector<ViewId>{ViewId::Front,ViewId::Left,ViewId::Back,ViewId::Right});
    auto group = complete_group(); group.uploads = {"front-source.png","","back-source.png",""};
    REQUIRE(session.apply_snapshot(group,session.begin_open(group.id),true));
    CHECK(session.all_design_views() == std::vector<ViewId>{ViewId::Front,ViewId::Back});
    REQUIRE(session.set_upload(ViewId::Front,""));
    CHECK(session.all_design_views() == std::vector<ViewId>{ViewId::Back});
    CHECK(session.input_rejection() == "missing_front");
    REQUIRE(session.set_upload(ViewId::Back,""));
    CHECK(session.all_design_views().size() == 4);
}

TEST_CASE("Missing main reference permits auxiliary deletion but rejects additions and replacements",
          "[AdvancedCreation][Advanced2DPreview]")
{
    Session session;
    CHECK_FALSE(session.set_upload(ViewId::Left,"new.png",true));
    CHECK(session.draft().uploads[1].empty());
    REQUIRE(session.set_upload(ViewId::Front,"front.png"));
    REQUIRE(session.set_upload(ViewId::Left,"left.png"));
    REQUIRE(session.set_upload(ViewId::Front,""));
    const auto before = session.command_context();
    CHECK_FALSE(session.set_upload(ViewId::Left,"left.png",true));
    CHECK_FALSE(session.set_upload(ViewId::Left,"other.png"));
    CHECK_FALSE(session.set_upload(ViewId::Right,"new.png"));
    CHECK(session.matches_command(before));
    CHECK(session.draft().uploads[1] == "left.png");
    REQUIRE(session.set_upload(ViewId::Left,""));
    REQUIRE(session.set_upload(ViewId::Front,"restored.png"));
    CHECK(session.set_upload(ViewId::Right,"new.png"));
}

TEST_CASE("Single design edits require a current or failed direction and retain global unresolved guards",
          "[AdvancedCreation][Advanced2DPreview]")
{
    const bool front_upload_only = GENERATE(false, true);
    auto group = complete_group();
    if (front_upload_only) group.uploads[index(ViewId::Front)] = "main-reference.png";
    Session session;
    REQUIRE(session.apply_snapshot(group, session.begin_open(group.id), true));
    const std::vector<ViewId> front_only {ViewId::Front};
    if (front_upload_only) CHECK(session.all_design_views() == front_only);
    CHECK(session.has_local_design_target(ViewId::Left));
    CHECK(session.single_design_rejection(ViewId::Left,"").empty());
    CHECK(session.single_design_rejection(ViewId::Left,std::string(2000,'a')).empty());
    CHECK(session.single_design_rejection(ViewId::Left,std::string(2001,'a')) == "invalid_input");
    group = *session.group(); group.views[1] = {}; ++group.revision;
    REQUIRE(session.apply_snapshot(group,session.context()));
    CHECK_FALSE(session.has_local_design_target(ViewId::Left));
    CHECK(session.single_design_rejection(ViewId::Left,"") == "missing_current_view");
    group.views[1].attempt_state = "failed"; ++group.revision;
    REQUIRE(session.apply_snapshot(group,session.context()));
    CHECK(session.has_local_design_target(ViewId::Left));
    CHECK(session.single_design_rejection(ViewId::Left,"").empty());
    group.views[3].attempt_state = "ambiguous"; ++group.revision;
    REQUIRE(session.apply_snapshot(group,session.context()));
    CHECK(session.single_design_rejection(ViewId::Left, "").empty());
    for (const std::string known : {"result_pending", "result_saved"}) {
        group.views[3].attempt_state = known; ++group.revision;
        REQUIRE(session.apply_snapshot(group,session.context()));
        CHECK(session.single_design_rejection(ViewId::Left, "") == "recovery_required");
    }
}

TEST_CASE("Local design can use its displayed base or its own description when the global prompt is empty",
          "[AdvancedCreation][AdvancedLocalDesign]")
{
    auto group = complete_group();
    group.saved_draft.prompt.clear();
    group.views[index(ViewId::Front)] = {};
    group.views[index(ViewId::Left)] = {};
    group.views[index(ViewId::Left)].attempt_state = "failed";
    Session session;
    REQUIRE(session.apply_snapshot(group, session.begin_open(group.id), true));
    CHECK(session.input_rejection() == "empty_input");
    CHECK(session.all_design_views().empty());
    CHECK(session.single_design_rejection(ViewId::Right, "").empty());
    CHECK(session.has_local_design_target(ViewId::Left));
    CHECK(session.single_design_rejection(ViewId::Left, " ") == "empty_input");
    CHECK(session.single_design_rejection(ViewId::Left, "Render the left profile").empty());
    CHECK(session.single_design_rejection(ViewId::Front, "Render the main view") == "missing_current_view");
}

TEST_CASE("Model selection candidates keep preferences unchanged until their exact final revision commits",
          "[AdvancedCreation][AdvancedModelSelection]")
{
    auto session = ready_session(); const auto before = session.local_draft();
    const auto id = session.begin_model_selection();
    CHECK_FALSE(session.toggle_candidate_view(ViewId::Front));
    REQUIRE(session.toggle_candidate_view(ViewId::Left));
    CHECK(session.local_draft().model_selected == before.model_selected);
    REQUIRE(session.candidate_local_draft());
    CHECK_FALSE(session.candidate_local_draft()->model_selected[1]);
    CHECK_FALSE(session.commit_model_selection(id,0));
    REQUIRE(session.commit_model_selection(id,1));
    CHECK_FALSE(session.model_selection()[1]);
    CHECK_FALSE(session.model_candidate());
    session.begin_model_selection(); REQUIRE(session.toggle_candidate_view(ViewId::Back));
    const auto confirmed = session.local_draft(); session.cancel_model_selection();
    CHECK(session.local_draft().model_selected == confirmed.model_selected);
}

TEST_CASE("Model candidates allow removing unavailable old auxiliaries while retaining creation guards",
          "[AdvancedCreation][AdvancedModelSelection]")
{
    auto session = ready_session(); auto group = *session.group();
    group.views[1].display_error = true; ++group.revision;
    REQUIRE(session.apply_snapshot(group,session.context()));
    const Capabilities caps {true,true,true,true,true,{true,true,true,true},true};
    const HostGuards guards {true,true,true};
    CHECK_FALSE(session.eligibility(caps,guards).generate_model);
    CHECK(session.model_selection_entry_eligibility(caps,guards).generate_model);
    session.begin_model_selection();
    CHECK_FALSE(session.candidate_model_eligibility(caps,guards).generate_model);
    REQUIRE(session.toggle_candidate_view(ViewId::Left));
    CHECK_FALSE(session.toggle_candidate_view(ViewId::Left));
    CHECK(session.candidate_model_eligibility(caps,guards).generate_model);
    CHECK_FALSE(session.candidate_model_eligibility(caps,{false,true,true}).generate_model);
    group.views[3].attempt_state = "ambiguous"; ++group.revision;
    REQUIRE(session.apply_snapshot(group,session.context()));
    CHECK(session.model_selection_entry_eligibility(caps, guards).generate_model);
    session.begin_model_selection();
    CHECK(session.candidate_model_eligibility(caps, guards).model_reason == "design_result_unknown");
    REQUIRE(session.toggle_candidate_view(ViewId::Left));
    REQUIRE(session.toggle_candidate_view(ViewId::Right));
    CHECK(session.candidate_model_eligibility(caps, guards).generate_model);
    session.cancel_model_selection();
    CHECK(session.model_selection()[index(ViewId::Right)]);
}

TEST_CASE("Model candidates keep their selection across unrelated design edits and revisions",
          "[AdvancedCreation][AdvancedModelSelection]")
{
    const std::string change = GENERATE("draft","group-revision","model-selection","creation");
    auto session = ready_session(); const auto id = session.begin_model_selection();
    REQUIRE(session.toggle_candidate_view(ViewId::Right));
    if (change == "draft") { auto draft = session.draft().design; draft.prompt += " changed"; session.set_design_draft(draft); }
    if (change == "group-revision") { auto group = *session.group(); ++group.revision; session.apply_snapshot(group,session.context()); }
    if (change == "model-selection") { session.click(ViewId::Back,100,1,SelectionContext::Model); session.flush_click(101); }
    if (change == "creation") session.new_creation();
    CHECK(session.commit_model_selection(id,1) == (change == "draft" || change == "group-revision"));
}

TEST_CASE("Advanced local plan parsing rejects ignored scope expanded targets and mismatched frozen identity",
          "[AdvancedCreation][Advanced2DPreview]")
{
    using Json = nlohmann::json;
    const std::string variant = GENERATE("valid","legacy","targets","dependencies","calls","modes","base","mode","text","revision","group","id");
    const Json intent {{"version",1},{"scope","single_view"},{"target_view","left"},{"local_prompt","keep this local"}};
    const Json request {{"command_id","operation-a"},{"kind","edit"},{"expected_group_revision",4},{"design_request",intent},
        {"base_versions",{{"left","version-a"}}}};
    Json plan {{"id","operation-a"},{"group_id","group-a"},{"kind","edit"},{"expected_group_revision",4},
        {"design_request",intent},{"targets",Json::array({"left"})},{"base_versions",{{"left","version-a"}}},
        {"target_modes",{{"left","edit"}}},{"dependency_views",Json::array()},{"max_new_image_calls",1},{"max_new_model_tasks",0}};
    if (variant == "legacy") plan.erase("design_request");
    if (variant == "targets") plan["targets"].push_back("front");
    if (variant == "dependencies") plan["dependency_views"].push_back("front");
    if (variant == "calls") plan["max_new_image_calls"] = 2;
    if (variant == "modes") plan["target_modes"]["front"] = "initial";
    if (variant == "base") plan["base_versions"]["left"] = "another-version";
    if (variant == "mode") plan["target_modes"]["left"] = "initial";
    if (variant == "text") plan["design_request"]["local_prompt"] = "different";
    if (variant == "revision") plan["expected_group_revision"] = 5;
    if (variant == "group") plan["group_id"] = "group-b";
    if (variant == "id") plan["id"] = "operation-b";
    CHECK(AdvancedMultiviewClient::design_plan_matches(plan,request,"group-a",{ViewId::Left}) == (variant == "valid"));
}

TEST_CASE("Model selection entry defers exact image count capability until the candidate is chosen",
          "[AdvancedCreation][AdvancedModelSelection]")
{
    const int count = GENERATE(1,2,3,4);
    auto session = ready_session();
    Capabilities caps {true,true,true,true,true,{},true};
    caps.model_image_counts[count-1] = true;
    const HostGuards guards {true,true,true};
    REQUIRE(session.model_selection_entry_eligibility(caps,guards).generate_model);
    session.begin_model_selection();
    for (int i=3; i>=count; --i) REQUIRE(session.toggle_candidate_view(AllViews[i]));
    CHECK(session.candidate_model_eligibility(caps,guards).generate_model);
    CHECK_FALSE(session.model_selection_entry_eligibility(caps,{false,true,true}).generate_model);
    caps.model_image_counts.fill(false);
    CHECK_FALSE(session.model_selection_entry_eligibility(caps,guards).generate_model);
    CHECK_FALSE(session.candidate_model_eligibility(caps,guards).generate_model);
}

TEST_CASE("Model candidate plans must echo exact frozen versions options and creation identity",
          "[AdvancedCreation][AdvancedModelSelection]")
{
    using Json = nlohmann::json;
    const std::string variant = GENERATE("valid","epoch","version","sha","path","extra-view","selection",
        "mode","job","group","provider","model","faces","quality","output","calls");
    const Json request {{"schema", Slic3r::GUI::AdvancedMultiviewClient::selected_plan_schema},{"command_id","operation-a"},
        {"selected_views",Json::array({"front","right"})},
        {"visible_versions",{{"front",{{"version_id","front-a"},{"sha256",std::string(64,'a')}}},
                             {"right",{{"version_id","right-a"},{"sha256",std::string(64,'b')}}}}},
        {"provider","tripo"},{"geometry_model","model-a"},{"face_limit",300000},
        {"geometry_quality","standard"},{"texture_quality","standard"},{"output_format","glb"}};
    Json snapshot {{"schema","approved-multiview-input-v2"},{"group_id","group-a"},{"snapshot_id","operation-a"},
        {"input_epoch","00000000-0000-4000-8000-000000000001"},{"selected_views",request.at("selected_views")},{"input_mode","multiview"},{"views",Json::object()}};
    for (const auto& view : request.at("selected_views")) {
        const auto name=view.get<std::string>();
        snapshot["views"][name]=request.at("visible_versions").at(name);
        snapshot["views"][name]["path"]="inputs/"+name+".png";
    }
    Json options;
    for (const auto* key : {"provider","geometry_model","face_limit","geometry_quality","texture_quality","output_format"})
        options[key]=request.at(key);
    Json plan {{"schema",Slic3r::GUI::AdvancedMultiviewClient::selected_plan_schema},{"id","operation-a"},{"job_id","operation-a"},
        {"group_id","group-a"},{"draft",{{"prompt","Original design metadata"}}},{"options",options},
        {"approved_multiview_snapshot",snapshot},{"max_new_image_calls",0},{"max_new_model_tasks",1},
        {"max_new_conversion_tasks",0},{"confirmation_fingerprint",std::string(64,'c')}};
    auto& approved=plan["approved_multiview_snapshot"];
    if (variant=="epoch") approved["input_epoch"]="epoch-b";
    if (variant=="version") approved["views"]["front"]["version_id"]="another-version";
    if (variant=="sha") approved["views"]["right"]["sha256"]=std::string(64,'d');
    if (variant=="path") approved["views"]["right"]["path"]="inputs/front.png";
    if (variant=="extra-view") approved["views"]["left"]=approved["views"]["right"];
    if (variant=="selection") approved["selected_views"]=Json::array({"front"});
    if (variant=="mode") approved["input_mode"]="image";
    if (variant=="job") plan["job_id"]="job-b";
    if (variant=="group") approved["group_id"]="group-b";
    if (variant=="provider") plan["options"]["provider"]="yoyoclaw";
    if (variant=="model") plan["options"]["geometry_model"]="model-b";
    if (variant=="faces") plan["options"]["face_limit"]=1000000;
    if (variant=="quality") plan["options"]["texture_quality"]="detailed";
    if (variant=="output") plan["options"]["output_format"]="obj";
    if (variant=="calls") plan["max_new_image_calls"]=1;
    CHECK(AdvancedMultiviewClient::model_plan_matches(plan,request,"group-a")== (variant=="valid"));
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
    auto original = *session.group(); original.uploads[0] = "front-upload.png"; original.uploads[1] = "same-path.png";
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
    auto uploaded = *session.group(); uploaded.uploads[0] = "front-upload.png";
    REQUIRE(session.apply_snapshot(uploaded, session.context(), true));
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
        image.display_error = true; // The previous version itself is unavailable.
    } else if (failure=="display") image.display_error=true;
    else { image.asset_pending=true; image.canonical_version_id="unverified-new-version"; }
    REQUIRE(session.apply_snapshot(failed,session.context()));
    Capabilities capabilities {true,true,true,true,true,{true,true,true,true},true};
    CHECK(session.model_selection()[slot]);
    CHECK_FALSE(session.eligibility(capabilities,{true,true,true}).generate_model);
    const auto old_command=session.command_context();
    const bool explicit_check = context == SelectionContext::Model && GENERATE(false, true);
    if (explicit_check) {
        CHECK_FALSE(session.can_toggle_model_view(ViewId::Front));
        CHECK(session.can_toggle_model_view(auxiliary));
        REQUIRE(session.toggle_model_view(auxiliary));
    } else {
        session.click(auxiliary,300,10,context);
        REQUIRE(session.flush_click(310));
    }
    CHECK_FALSE(session.model_selection()[slot]);
    CHECK_FALSE(session.matches_model_command(old_command));
    CHECK(session.group()->pending_views[slot]==failed.pending_views[slot]);
    if (context==SelectionContext::DesignForNextModel) CHECK_FALSE(session.selection()[slot]);
    std::vector<ViewId> expected {ViewId::Front};
    if (keep_other) expected.push_back(AllViews[other]);
    CHECK(session.effective_model_views()==expected);
    REQUIRE(session.eligibility(capabilities,{true,true,true}).generate_model);
    const auto canceled_command=session.command_context();
    if (explicit_check) {
        CHECK_FALSE(session.can_toggle_model_view(auxiliary));
        CHECK_FALSE(session.toggle_model_view(auxiliary));
    } else {
        session.click(auxiliary,400,10,context);
        CHECK_FALSE(session.flush_click(410));
    }
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
        "draft","result_pending","result_saved","front");
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
    if (blocker == "result_pending" || blocker == "result_saved") blocked.views[1].attempt_state = blocker;
    REQUIRE(session.apply_snapshot(blocked,session.context()));
    if (blocker=="draft") {
        auto draft=session.draft().design; draft.prompt="changed";
        REQUIRE(session.set_design_draft(draft));
    }
    const bool unrelated = blocker == "draft" || blocker == "result_pending" || blocker == "result_saved";
    CHECK(session.eligibility({true,true,true,true,true,{true,true,true,true},true},guards).generate_model == unrelated);
    session.click(ViewId::Front,200,10,SelectionContext::Model);
    CHECK_FALSE(session.flush_click(210));
    CHECK(session.model_selection()[0]);
}

TEST_CASE("Hidden auxiliary slots and ordinary repaint clicks retain their existing selection rules",
          "[AdvancedCreation][AdvancedFlexibleViews]")
{
    const bool replacement=GENERATE(false,true);
    auto session=ready_session();
    auto uploaded=*session.group(); uploaded.uploads[0]="front-upload.png"; uploaded.uploads[1]="original-left.png";
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

TEST_CASE("Unselected auxiliary failures and saved results allow modeling with the remaining views",
          "[AdvancedCreation][AdvancedFlexibleViews]")
{
    const std::string state = GENERATE("failed","stopped","blocked","ambiguous","result_pending","result_saved");
    auto group = complete_group(); group.views[2] = {}; group.views[2].attempt_state = state;
    group.pending_views[2] = true;
    Session session; REQUIRE(session.apply_snapshot(group,session.begin_open(group.id),true));
    Capabilities capabilities {true,true,true,true,true,{true,true,true,true},true};
    const auto allowed = session.eligibility(capabilities,{true,true,true});
    CHECK(allowed.generate_model);
    CHECK(allowed.recover == (state == "result_pending" || state == "result_saved"));
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
    CHECK(eligible.click_generate_model == unresolved);
    CHECK_FALSE(eligible.generate_model);
    if (blocker == "workbench") CHECK(eligible.model_reason == "missing_current_view");
    if (unresolved) {
        CHECK(eligible.generate_design == (blocker == "ambiguous"));
        CHECK(eligible.recover == (blocker != "ambiguous"));
        if (blocker != "ambiguous") CHECK(eligible.design_reason == "recovery_required");
    }
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

TEST_CASE("Advanced feedback clicks accept valid published results through draft changes",
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
    const bool usable = invalid == "draft" || invalid == "pending";
    CHECK(eligible.generate_model == usable);
    CHECK(eligible.model_reason.empty() == usable);
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
    CHECK(session.eligibility(capabilities, guards).generate_model);
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
    CHECK(session.eligibility(capabilities, guards).generate_model == (changed != "upload"));
    CHECK(session.eligibility(capabilities, guards).model_reason ==
          (changed == "upload" ? "missing_current_view" : ""));
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

TEST_CASE("Reopening a creation preserves its local model preferences before history responds",
          "[AdvancedCreation][AdvancedDraftInput][AdvancedModelSelection][Regression]")
{
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-reopen-preferences"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    // The model output directory is process-scoped. Exercise both opens within
    // the same isolated directory rather than reinitializing it per generator.
    for (const bool same_group : {true, false}) {
        CAPTURE(same_group);
        Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, "https://not-a-local-service.invalid");
        auto group = complete_group();
        group.id = "11111111-1111-4111-8111-111111111111";
        REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
        host.session().click(ViewId::Left, 100, 10, SelectionContext::Model);
        REQUIRE(host.session().flush_click(110));
        const auto checkpoint = host.session().local_draft();
        host.changed();
        const auto path = temporary.path() / "advanced-cache" / "drafts" / (group.id + ".json");
        REQUIRE(boost::filesystem::is_regular_file(path));
        const auto read_checkpoint = [&] {
            std::ifstream input(path.string());
            REQUIRE(input.is_open());
            return nlohmann::json::parse(input);
        };
        const auto before = read_checkpoint();
        // Opening retains the installed context and its checkpoint until the
        // requested group is actually admitted.
        const std::string opened = same_group ? group.id : "22222222-2222-4222-8222-222222222222";
        host.open_group(opened);
        REQUIRE(host.session().context().group_id == group.id);
        CHECK(read_checkpoint() == before);
        if (same_group) {
            CHECK(host.session().local_draft().model_selected == checkpoint.model_selected);
            CHECK(host.session().local_draft().model_seen == checkpoint.model_seen);
        }
        host.shutdown();
        CHECK(read_checkpoint() == before);
    }
}

TEST_CASE("Shared text client snapshots binary input and reports missing originals locally",
          "[AIText][CreationInputRegression]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-text-snapshot"};
    AdvancedHttpProbe probe;
    const std::string route = "/text-capabilities/image_prompt_polish";
    probe.held_suffixes = {route};
    Slic3r::GUI::AITextFeatureHost text(probe.endpoint());
    using Client = Slic3r::GUI::AITextClient;
    const auto image = temporary.path() / "original.webp";
    const std::string original {"RIFF\x04\0\0\0WEBP\0\xff", 14};
    {
        boost::filesystem::ofstream file(image, std::ios::binary);
        file.write(original.data(), original.size());
    }
    std::atomic<bool> completed {false};
    std::atomic<bool> failed {false};
    std::string error;
    auto request = text.polish_image_prompt({true, image, "A small sculpture"},
        [&](Client::PolishResult) { completed = true; },
        [&](Client::Error failure) { error = failure.code; failed = true; });
    REQUIRE(request);
    {
        boost::filesystem::ofstream file(image, std::ios::binary | std::ios::trunc);
        file << "replacement";
    }
    REQUIRE(probe.until([&] { return probe.count(route) == 1; }));
    const auto& payload = probe.requests.at(probe.at(route)).payload;
    REQUIRE(payload.is_string());
    const auto bytes = payload.get<std::string>();
    CHECK(bytes.find(original) != std::string::npos);
    CHECK(bytes.find("replacement") == std::string::npos);
    probe.reply(probe.at(route), {{"status", "success"}, {"optimized_prompt", "A small sculpture"},
        {"warnings", nlohmann::json::array()}, {"diagnostic_id", "test-diagnostic"}});
    REQUIRE(probe.until([&] { return completed.load() || failed.load(); }));
    CHECK(completed.load());
    CHECK(error.empty());
    request = text.polish_image_prompt({true, temporary.path() / "missing.png", "A small sculpture"},
        [&](Client::PolishResult) { FAIL("Missing image was accepted"); },
        [&](Client::Error failure) { error = failure.code; });
    CHECK_FALSE(request);
    CHECK(error == "invalid_reference_image");
}

TEST_CASE("Advanced history commits only a validated target and protects editing until then",
          "[AdvancedCreation][AdvancedDraftInput][CreationInputRegression]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-history-admission"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.path().string());
    using Json = nlohmann::json;
    const std::string original_id = "11111111-1111-4111-8111-111111111111";
    const std::string target_id = "22222222-2222-4222-8222-222222222222";
    for (const std::string outcome : {"success", "http_error", "empty", "invalid", "wrong_id", "stale", "superseded"}) {
        CAPTURE(outcome);
        AdvancedHttpProbe probe;
        Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, probe.endpoint());
        auto original = complete_group();
        original.id = original_id;
        original.input_epoch = "33333333-3333-4333-8333-333333333333";
        original.saved_draft.prompt = "Preserve previous description";
        REQUIRE(host.session().apply_snapshot(original, host.session().begin_open(original.id), true));
        const auto previous = host.session().context();
        const auto initial = host.session().draft();
        const auto target = outcome == "stale" ? original_id : target_id;
        const auto route = "/groups/" + target;
        probe.held_suffixes = {route};
        host.open_group(target);
        REQUIRE(probe.until([&] { return probe.count(route) == 1; }));
        CHECK(host.session().matches(previous));
        CHECK(host.session().draft().design == initial.design);
        CHECK(host.draft_editing_locked());
        host.open_group(target);
        CHECK(probe.count(route) == 1);
        auto draft = original.saved_draft;
        draft.prompt = "Target description";
        const auto metadata = AdvancedMultiviewClient::design_json(draft);
        Json snapshot {{"schema", "advanced-multiview-group-v1"}, {"workflow_kind", "advanced_multiview_v1"},
            {"id", outcome == "wrong_id" ? original_id : target}, {"input_epoch", "44444444-4444-4444-8444-444444444444"},
            {"revision", outcome == "stale" ? original.revision - 1 : original.revision + 1},
            {"initial_input", metadata}, {"saved_draft", metadata}, {"latest_submitted_draft", metadata},
            {"active_operation_id", nullptr}, {"stop_intent", false}, {"published", true},
            {"uploads", Json::object()}, {"views", Json::object()}};
        for (const auto view : AllViews) {
            snapshot["uploads"][view_name(view)] = nullptr;
            snapshot["views"][view_name(view)] = {{"current_version_id", nullptr}, {"current", nullptr}, {"latest_attempt", nullptr}};
        }
        if (outcome == "superseded") {
            host.new_creation();
            auto next = host.session().draft().design;
            next.prompt = "New creation after cancellation";
            REQUIRE(host.session().set_design_draft(next));
        }
        if (outcome == "http_error") probe.reply(probe.at(route), {{"error", "history unavailable"}}, true);
        else if (outcome == "empty") probe.reply(probe.at(route), Json::object());
        else if (outcome == "invalid") probe.reply(probe.at(route), {{"group", Json::object()}});
        else probe.reply(probe.at(route), {{"group", snapshot}});
        REQUIRE(probe.until([&] { return !host.command_pending(); }));
        CHECK_FALSE(host.draft_editing_locked());
        if (outcome == "success") {
            CHECK(host.session().context().group_id == target);
            CHECK(host.session().draft().design.prompt == draft.prompt);
        } else if (outcome == "superseded") {
            probe.drain();
            CHECK_FALSE(host.session().group());
            CHECK(host.session().draft().design.prompt == "New creation after cancellation");
        } else {
            CHECK(host.session().matches(previous));
            CHECK(host.session().draft().design == initial.design);
        }
        host.shutdown();
    }
}

namespace {
// A valid group reply with no cached assets to fetch after admission.
nlohmann::json advanced_history_draft_response(const GroupSnapshot& group)
{
    using Json = nlohmann::json;
    Json snapshot {{"schema", "advanced-multiview-group-v1"}, {"workflow_kind", "advanced_multiview_v1"},
        {"id", group.id}, {"input_epoch", group.input_epoch}, {"revision", group.revision},
        {"initial_input", AdvancedMultiviewClient::design_json(group.initial_input)},
        {"saved_draft", AdvancedMultiviewClient::design_json(group.saved_draft)},
        {"latest_submitted_draft", AdvancedMultiviewClient::design_json(group.initial_input)},
        {"active_operation_id", nullptr}, {"stop_intent", false}, {"published", true},
        {"uploads", Json::object()}, {"views", Json::object()}};
    for (const auto view : AllViews) {
        snapshot["uploads"][view_name(view)] = nullptr;
        snapshot["views"][view_name(view)] = {{"current_version_id", nullptr}, {"current", nullptr}, {"latest_attempt", nullptr}};
    }
    return {{"group", std::move(snapshot)}};
}
}

TEST_CASE("Stopping an old design operation cannot release a pending history open",
          "[AdvancedCreation][AdvancedStopNavigation][CreationInputRegression][AdvancedHistoryConcurrency]")
{
    const bool stop_failed = GENERATE(false, true);
    CAPTURE(stop_failed);
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-history-stop-ownership"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.path().string());
    AdvancedHttpProbe probe;
    std::size_t notifications = 0;
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, probe.endpoint());
    auto current = host_test_group(91);
    current.active_operation_id = host_test_group(92).id;
    current.execution_state = "running";
    const auto target = host_test_group(93);
    const auto third = host_test_group(94);
    REQUIRE(host.session().apply_snapshot(current, host.session().begin_open(current.id), true));
    const auto source_context = host.session().context();
    host.set_listener([&] { ++notifications; });
    const auto target_route = "/groups/" + target.id;
    const auto third_route = "/groups/" + third.id;
    const auto stop_route = "/groups/" + current.id + "/operations/" + current.active_operation_id + "/stop";
    probe.held_suffixes = {target_route, third_route};
    host.open_group(target.id);
    REQUIRE(probe.until([&] { return probe.count(target_route) == 1; }));
    CHECK(host.draft_editing_locked());
    REQUIRE(host.eligibility().stop);
    host.stop();
    REQUIRE(probe.until([&] { return probe.count(stop_route) == 1; }));
    const auto before_reply = notifications;
    probe.reply(probe.at(stop_route), nlohmann::json::object(), stop_failed);
    REQUIRE(probe.until([&] { return notifications > before_reply; }));
    CHECK(host.session().matches(source_context));
    CHECK(host.command_pending());
    CHECK(host.draft_editing_locked());
    host.open_group(third.id);
    probe.drain();
    CHECK(probe.count(third_route) == 0);
    probe.reply(probe.at(target_route), advanced_history_draft_response(target));
    REQUIRE(probe.until([&] { return !host.command_pending() && host.session().context().group_id == target.id; }));
    CHECK_FALSE(host.draft_editing_locked());
    host.shutdown();
}

TEST_CASE("A failed history pre-save retains the scheduled autosave on the original group",
          "[AdvancedCreation][AdvancedDraftInput][CreationInputRegression][AdvancedHistoryConcurrency]")
{
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-history-autosave"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.path().string());
    AdvancedHttpProbe probe;
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, probe.endpoint());
    auto original = host_test_group(95);
    REQUIRE(host.session().apply_snapshot(original, host.session().begin_open(original.id), true));
    const auto context = host.session().context();
    const auto target = host_test_group(96).id;
    const auto draft_route = "/groups/" + original.id + "/draft";
    const auto history_route = "/groups/" + target;
    probe.held_suffixes = {draft_route, history_route};
    auto draft = host.session().draft().design;
    draft.prompt = "Latest description before opening history";
    REQUIRE(host.session().set_design_draft(draft));
    host.changed();
    host.open_group(target);
    REQUIRE(probe.until([&] { return probe.count(draft_route) == 1; }));
    CHECK(host.draft_editing_locked());
    probe.reply(probe.at(draft_route), nlohmann::json::object(), true);
    REQUIRE(probe.until([&] { return !host.draft_editing_locked(); }));
    CHECK(host.session().matches(context));
    CHECK(host.session().draft_dirty());
    CHECK(host.session().draft().design == draft);
    CHECK(probe.count(history_route) == 0);
    REQUIRE(probe.until([&] { return probe.count(draft_route) == 2; }));
    CHECK(probe.requests[probe.at(draft_route, 1)].payload == probe.requests[probe.at(draft_route)].payload);
    original.saved_draft = draft;
    ++original.revision;
    probe.reply(probe.at(draft_route, 1), advanced_history_draft_response(original));
    REQUIRE(probe.until([&] { return !host.command_pending() && !host.session().draft_dirty(); }));
    CHECK(host.session().matches(context));
    CHECK(host.session().draft().design == draft);
    CHECK(probe.count(history_route) == 0);
    host.shutdown();
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

TEST_CASE("A failed save before a local edit releases submission and permits an explicit retry",
          "[AdvancedCreation][AdvancedLocalDesign][AdvancedDraftInput]")
{
    const std::string failure = GENERATE("http", "missing_group", "invalid_group");
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-local-save-failure"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    wxIPV4address address;
    REQUIRE(address.Hostname("127.0.0.1")); REQUIRE(address.Service(0));
    wxSocketServer server(address, wxSOCKET_NOWAIT);
    REQUIRE(server.IsOk()); REQUIRE(server.GetLocal(address));
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr,
        "http://127.0.0.1:" + std::to_string(address.Service()));
    host.session() = ready_session();
    wxEventLoop loop; wxEventLoopActivator active(&loop);
    const auto pump = [&] { wxTheApp->ProcessPendingEvents(); loop.DispatchTimeout(20); };
    const auto reply = [&](const std::string& body, bool error = false) {
        std::unique_ptr<wxSocketBase> connection(server.Accept(false));
        if (!connection) return false;
        const std::string response = std::string("HTTP/1.1 ") +
            (error ? "503 Service Unavailable" : "200 OK") +
            "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        connection->Write(response.data(), response.size());
        REQUIRE(connection->LastCount() == response.size());
        connection->Close(); return true;
    };
    const std::string capabilities = R"({"schema":"advanced-multiview-capabilities-v1","image_models":[{"provider":"gpt","model":"gpt-image-2","initial_text":"supported","initial_named_references":"supported","edit_current_with_context":"supported"}],"model_plans":true,"model_providers":{},"groups":[]})";
    host.set_service_available(true);
    const auto discover_until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < discover_until) {
        reply(capabilities); pump();
    }
    REQUIRE(host.eligibility().generate_design);
    auto draft = host.session().draft().design; draft.prompt = "A newly edited global description";
    REQUIRE(host.session().set_design_draft(draft));
    int callbacks = 0, saves = 0;
    const std::string body = failure == "invalid_group" ? R"({"group":{}})" : "{}";
    for (int attempt = 1; attempt <= 2; ++attempt) {
        host.generate_single_design(ViewId::Left, "Only recolor this view", [&](bool accepted) {
            CHECK_FALSE(accepted); ++callbacks;
        });
        REQUIRE(host.command_pending());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (callbacks < attempt && std::chrono::steady_clock::now() < deadline) {
            if (reply(body, failure == "http")) ++saves;
            pump();
        }
        REQUIRE(callbacks == attempt);
        CHECK(saves == attempt);
        CHECK_FALSE(host.command_pending());
        CHECK_FALSE(host.has_pending_receipt());
        CHECK(host.session().draft_dirty());
        CHECK(host.session().draft().design.prompt == draft.prompt);
        CHECK(host.session().group()->views[1].version_id == "left-v1");
    }
    host.shutdown();
}

TEST_CASE("A model candidate can be confirmed after an unsent checkpoint failure is repaired",
          "[AdvancedCreation][AdvancedModelSelection][AdvancedDraftInput][Regression]")
{
    using Json = nlohmann::json;
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-candidate-save"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    for (const bool receipt_failure : {true, false}) {
        CAPTURE(receipt_failure);
        wxIPV4address address;
        REQUIRE(address.Hostname("127.0.0.1")); REQUIRE(address.Service(0));
        wxSocketServer server(address, wxSOCKET_NOWAIT);
        REQUIRE(server.IsOk()); REQUIRE(server.GetLocal(address));
        Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr,
            "http://127.0.0.1:" + std::to_string(address.Service()));
        auto group = complete_group();
        group.id = receipt_failure ? "33333333-3333-4333-8333-333333333333" :
                                     "44444444-4444-4444-8444-444444444444";
        group.input_epoch = group.id;
        for (std::size_t i = 0; i < group.views.size(); ++i)
            group.views[i].sha256 = std::string(64, static_cast<char>('a' + i));
        REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
        host.changed();
        const auto checkpoint = temporary.path() / "advanced-cache" / "drafts" / (group.id + ".json");
        const auto read_checkpoint = [&] {
            std::ifstream input(checkpoint.string()); REQUIRE(input.is_open()); return Json::parse(input);
        };
        const auto original = read_checkpoint();
        const auto preferences = host.session().model_selection();
        wxEventLoop loop; wxEventLoopActivator active(&loop);
        Json prepared, submitted;
        int model_posts = 0, reconciles = 0;
        const Json capabilities = {
            {"schema", "advanced-multiview-capabilities-v1"}, {"model_plans", true}, {"groups", Json::array()},
            {"image_models", Json::array({{{"provider", "gpt"}, {"model", "gpt-image-2"},
                {"initial_text", "supported"}, {"initial_named_references", "supported"},
                {"edit_current_with_context", "supported"}}})},
            {"model_providers", {{"tripo", {{"available", true}, {"named_four_view_model", "supported"},
                {"supported_image_counts", Json::array({1, 2, 3, 4})}}}}}};
        const auto reply = [&] {
            std::unique_ptr<wxSocketBase> connection(server.Accept(false));
            if (!connection) return false;
            // Use the existing real-client socket seam. This is an offline HTTP
            // response, never the application sidecar or a supplier service.
            connection->SetFlags(wxSOCKET_WAITALL); connection->SetTimeout(2);
            std::string header;
            char byte = 0;
            while (header.find("\r\n\r\n") == std::string::npos && header.size() < 16384) {
                connection->Read(&byte, 1); REQUIRE(connection->LastCount() == 1); header += byte;
            }
            const auto length_at = header.find("Content-Length:");
            const auto length = length_at == std::string::npos ? 0 :
                std::stoul(header.substr(length_at + std::string("Content-Length:").size()));
            std::string payload(length, '\0');
            if (length) { connection->Read(payload.data(), length); REQUIRE(connection->LastCount() == length); }
            const auto first_space = header.find(' '), second_space = header.find(' ', first_space + 1);
            const auto path = header.substr(first_space + 1, second_space - first_space - 1);
            Json body = capabilities;
            bool failure = false;
            if (path.find("/model-plans") != std::string::npos) {
                const auto request = Json::parse(payload);
                Json snapshot {{"schema", "approved-multiview-input-v2"}, {"group_id", group.id},
                    {"snapshot_id", request.at("command_id")}, {"input_epoch", group.input_epoch},
                    {"selected_views", request.at("selected_views")}, {"input_mode", "multiview"},
                    {"views", request.at("visible_versions")}};
                for (const auto& view : request.at("selected_views"))
                    snapshot["views"][view.get<std::string>()]["path"] = "inputs/" + view.get<std::string>() + ".png";
                Json options = Json::object();
                for (const auto* key : {"provider", "geometry_model", "face_limit", "geometry_quality", "texture_quality", "output_format"})
                    options[key] = request.at(key);
                if (options.at("geometry_model").get<std::string>().empty()) options["geometry_model"] = "v3.0";
                const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
                body = {{"plan", {{"schema", Slic3r::GUI::AdvancedMultiviewClient::selected_plan_schema}, {"id", request.at("command_id")},
                    {"job_id", request.at("command_id")}, {"group_id", group.id},
                    {"options", options}, {"approved_multiview_snapshot", snapshot}, {"max_new_image_calls", 0},
                    {"max_new_model_tasks", 1}, {"max_new_conversion_tasks", 0}, {"expires_at", now + 300},
                    {"confirmation_fingerprint", std::string(64, 'e')}}}};
            } else if (path.find("/reconcile") != std::string::npos) {
                ++reconciles; body = {{"accepted", false}};
            } else if (path.size() >= 11 && path.substr(path.size() - 11) == "/model-jobs") {
                ++model_posts; submitted = Json::parse(payload); failure = true; body = Json::object();
            }
            const auto text = body.dump();
            const std::string response = std::string("HTTP/1.1 ") + (failure ? "503 Service Unavailable" : "200 OK") +
                "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(text.size()) +
                "\r\nConnection: close\r\n\r\n" + text;
            connection->Write(response.data(), response.size()); REQUIRE(connection->LastCount() == response.size());
            connection->Close(); return true;
        };
        const auto until = [&](auto ready) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!ready() && std::chrono::steady_clock::now() < deadline) {
                reply(); wxTheApp->ProcessPendingEvents(); loop.DispatchTimeout(20);
            }
            return ready();
        };
        host.set_service_available(true);
        REQUIRE(until([&] { return host.eligibility().generate_model; }));
        REQUIRE(host.begin_model_selection());
        REQUIRE(host.session().toggle_candidate_view(ViewId::Left));
        std::string prepare_error;
        host.prepare_model_selection([&](Json plan) { prepared = std::move(plan); },
                                     [&](std::string reason) { prepare_error = std::move(reason); });
        REQUIRE(until([&] { return !prepared.is_null() || !prepare_error.empty(); }));
        CAPTURE(prepare_error);
        REQUIRE(prepare_error.empty()); REQUIRE_FALSE(prepared.is_null());
        const auto candidate = host.session().model_candidate(); REQUIRE(candidate);
        const auto blocked = receipt_failure ? temporary.path() / "advanced-cache" / "pending-submissions" /
            (prepared.at("job_id").get<std::string>() + ".json.part") :
            boost::filesystem::path(checkpoint.string() + ".part");
        boost::filesystem::create_directories(blocked.parent_path());
        REQUIRE(boost::filesystem::create_directory(blocked));
        CHECK_FALSE(host.confirm_model_selection(prepared));
        CHECK(host.session().model_selection() == preferences);
        CHECK(read_checkpoint() == original);
        CHECK(model_posts == 0);
        REQUIRE(host.session().model_candidate());
        CHECK(host.session().model_candidate()->id == candidate->id);
        CHECK(host.session().model_candidate()->revision == candidate->revision);
        CHECK(host.session().model_candidate()->selected == candidate->selected);
        // No POST was sent. An unsent checkpoint failure must not become an
        // unresolved model request or consume this plan through negative reconcile.
        CHECK_FALSE(host.has_pending_receipt());
        REQUIRE(boost::filesystem::remove(blocked));
        host.reconcile_submission();
        wxTheApp->ProcessPendingEvents();
        loop.DispatchTimeout(20);
        reply();
        CHECK_FALSE(host.has_pending_receipt());
        CHECK(reconciles == 0);
        CHECK(host.confirm_model_selection(prepared));
        REQUIRE(until([&] { return model_posts > 0; }));
        CHECK(model_posts == 1);
        CHECK(submitted.at("command_id") == prepared.at("id"));
        CHECK(host.session().model_selection() == candidate->selected);
        CHECK(read_checkpoint().at("model_selected") == Json(candidate->selected));
        host.shutdown();
    }
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

TEST_CASE("Model candidate responses preserve only the newest selection through reverse delivery",
          "[AdvancedCreation][AdvancedModelSelection][Regression]")
{
    using Json = nlohmann::json;
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-model-order"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    AdvancedHttpProbe probe;
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, probe.endpoint());
    const auto group = host_test_group(1);
    REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
    host.set_service_available(true);
    REQUIRE(probe.until([&] { return host.eligibility().generate_model; }));
    const auto original = host.session().model_selection();
    REQUIRE(host.begin_model_selection());
    REQUIRE(host.session().toggle_candidate_view(ViewId::Back));
    REQUIRE(host.session().toggle_candidate_view(ViewId::Right));
    Json ready; int ready_count = 0; std::vector<std::string> errors;
    const auto prepare = [&] {
        host.prepare_model_selection([&](Json plan) { ready = std::move(plan); ++ready_count; },
            [&](std::string reason) { errors.push_back(std::move(reason)); });
    };
    prepare(); REQUIRE(probe.until([&] { return probe.count("/model-plans") == 1; }));
    REQUIRE(host.session().toggle_candidate_view(ViewId::Left));
    prepare(); REQUIRE(probe.until([&] { return probe.count("/model-plans") == 2; }));
    REQUIRE(host.session().toggle_candidate_view(ViewId::Right));
    prepare(); REQUIRE(probe.until([&] { return probe.count("/model-plans") == 3; }));
    const auto first = probe.at("/model-plans", 0), second = probe.at("/model-plans", 1), third = probe.at("/model-plans", 2);
    CHECK(probe.requests[first].payload.at("selected_views") == Json::array({"front", "left"}));
    CHECK(probe.requests[second].payload.at("selected_views") == Json::array({"front"}));
    CHECK(probe.requests[third].payload.at("selected_views") == Json::array({"front", "right"}));
    const auto latest = probe.model_plan(third, group.id);
    probe.reply(third, {{"plan", latest}});
    REQUIRE(probe.until([&] { return ready_count == 1; }));
    probe.reply(second, {{"plan", probe.model_plan(second, group.id)}});
    probe.reply(first, {{"plan", probe.model_plan(first, group.id)}});
    probe.drain();
    CHECK(ready_count == 1); CHECK(errors.empty()); CHECK(ready == latest);
    CHECK(host.session().model_selection() == original);
    CHECK(probe.count("/model-jobs") == 0);
    REQUIRE(host.confirm_model_selection(ready));
    REQUIRE(probe.until([&] { return probe.count("/model-jobs") == 1; }));
    const auto submitted = probe.at("/model-jobs");
    CHECK(probe.requests[submitted].payload.at("plan_id") == latest.at("id"));
    CHECK(host.session().model_selection() == std::array<bool,4>{true,false,false,true});
    probe.reply(submitted, Json::object(), true);
    REQUIRE(probe.until([&] { return !host.command_pending(); }));
    CHECK(host.has_pending_receipt()); // A sent request is still reconciled conservatively.
    host.shutdown();
}

TEST_CASE("Accepted advanced plans provide waiting images independently of job state writes",
          "[AdvancedCreation][AdvancedModelSelection][GenerationParticles][Regression]")
{
    using Json = nlohmann::json;
    namespace GP = Slic3r::GUI::GenerationParticles;
    const bool multiview = GENERATE(false, true);
    const bool reconcile = GENERATE(false, true);
    wxInitializer init; REQUIRE(init.IsOk());
    // The application resolves its model root once per process, so all generated cases share this directory.
    static ScopedTemporaryDir directory("orca-advanced-visual-receipt");
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", directory.string());
    AdvancedHttpProbe probe;
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, probe.endpoint());
    auto group = host_test_group(244);
    group.views[index(ViewId::Back)] = {};
    group.views[index(ViewId::Right)] = {};
    if (!multiview) group.views[index(ViewId::Left)] = {};
    group.views[0].version_id = group.views[0].canonical_version_id = host_test_group(245).id;

    wxImage image(96, 96);
    image.SetRGB(wxRect(0, 0, 96, 96), 240, 240, 240);
    for (int y = 0; y < 96; ++y)
        for (int x = 0; x < 96; ++x)
            if ((x - 48) * (x - 48) + (y - 48) * (y - 48) < 24 * 24)
                image.SetRGB(x, y, 18, 54, 111);
    wxMemoryOutputStream png_bytes;
    wxPNGHandler png;
    REQUIRE(png.SaveFile(&image, png_bytes));
    std::vector<unsigned char> bytes(png_bytes.GetSize());
    REQUIRE(png_bytes.CopyTo(bytes.data(), bytes.size()) == bytes.size());
    unsigned char hash[EVP_MAX_MD_SIZE] {};
    unsigned int hash_size = 0;
    REQUIRE(EVP_Digest(bytes.data(), bytes.size(), hash, &hash_size, EVP_sha256(), nullptr) == 1);
    REQUIRE(hash_size == 32);
    constexpr char hex[] = "0123456789abcdef";
    std::string digest;
    for (unsigned i = 0; i < hash_size; ++i) {
        digest.push_back(hex[hash[i] >> 4]);
        digest.push_back(hex[hash[i] & 15]);
    }
    group.views[0].sha256 = digest;
    REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
    host.set_service_available(true);
    REQUIRE(probe.until([&] { return host.eligibility().generate_model; }));
    unsigned confirmations = 0;
    host.generate_model([&](const Json& value) {
        ++confirmations;
        CHECK(value.at("approved_multiview_snapshot").at("selected_views") ==
              (multiview ? Json::array({"front", "left"}) : Json::array({"front"})));
        return true;
    });
    REQUIRE(probe.until([&] { return probe.count("/model-plans") == 1; }));
    const auto planning = probe.at("/model-plans");
    const auto plan = probe.model_plan(planning, group.id);
    probe.reply(planning, {{"plan", plan}});
    REQUIRE(probe.until([&] { return probe.count("/model-jobs") == 1; }));
    CHECK(confirmations == 1);
    CHECK_FALSE(GP::usable_reference(host.model_state()));
    const auto job = plan.at("job_id").get<std::string>();
    const auto job_dir = directory.path() / job;
    boost::filesystem::create_directories(job_dir / "inputs");
    const auto frozen = job_dir / "inputs" / "front.png";
    {
        boost::filesystem::ofstream file(frozen, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        REQUIRE(file.good());
    }
    const auto submitted = probe.at("/model-jobs");
    const Json accepted = {{"id", job}, {"creation_kind", "advanced_multiview_v1"},
        {"advanced_group_id", group.id}, {"workflow_mode", "advanced"},
        {"state", "queued"}, {"phase", "queued"}, {"updated_at", 100.0}};
    if (reconcile) {
        probe.reply(submitted, Json::object(), true);
        REQUIRE(probe.until([&] { return !host.command_pending(); }));
        CHECK_FALSE(GP::usable_reference(host.model_state()));
        host.reconcile_submission();
        REQUIRE(probe.until([&] { return probe.count("/reconcile") == 1; }));
        probe.reply(probe.at("/reconcile"), {{"accepted", true}, {"job", accepted}, {"group", Json::object()}});
    } else {
        probe.reply(submitted, {{"job", accepted}});
    }
    REQUIRE(probe.until([&] { return GP::usable_reference(host.model_state()); }));
    const auto visual = host.model_state().visual_reference;
    CHECK(visual.owner_id == job);
    CHECK(visual.version == group.views[0].version_id);
    CHECK(visual.sha256 == digest);
    CHECK(visual.verification_manifest.empty());
    CHECK(boost::filesystem::path(wxString::FromUTF8(visual.path).ToStdWstring()) == frozen);

#ifdef _WIN32
    const auto state_path = job_dir / "job.json";
    const auto next = job_dir / "job.json.part";
    { boost::filesystem::ofstream file(state_path); file << "{\"state\":\"queued\"}"; }
    { boost::filesystem::ofstream file(next); file << "{\"state\":\"running\"}"; }
    {
        boost::filesystem::ifstream previous_reader(state_path, std::ios::binary);
        REQUIRE(previous_reader.good());
        const bool replaced_while_reading = ::MoveFileExW(next.c_str(), state_path.c_str(), MOVEFILE_REPLACE_EXISTING);
        const auto error = ::GetLastError();
        REQUIRE_FALSE(replaced_while_reading);
        CHECK(error == ERROR_ACCESS_DENIED);
    }
    bool replaced = false;
    const auto result = GP::load(visual, false, {}, [&](const GP::Image&, const auto&) {
        replaced = ::MoveFileExW(next.c_str(), state_path.c_str(), MOVEFILE_REPLACE_EXISTING);
        return GP::MaskResult {};
    });
    CHECK(replaced);
    CHECK(result);
#endif
    auto wrong_image = visual;
    wrong_image.sha256.assign(64, '0');
    CHECK_FALSE(GP::load(wrong_image, false, {}));
    CHECK(GP::load(visual, false, {}));
    CHECK(probe.count("/model-jobs") == 1);
    host.new_creation();
    CHECK_FALSE(GP::usable_reference(host.model_state()));
    host.shutdown();
}

TEST_CASE("Prepared model candidates recheck changed inputs capabilities expiry and editing guards before posting",
          "[AdvancedCreation][AdvancedModelSelection][Regression]")
{
    using Json = nlohmann::json;
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-model-final-guards"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    unsigned suffix = 10;
    for (const std::string change : {"version", "sha", "epoch", "creation", "draft", "model", "provider", "face-limit",
                                    "geometry-quality", "texture-quality", "format", "expired", "capability", "read-only", "cancel"}) {
        CAPTURE(change);
        AdvancedHttpProbe probe;
        Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, probe.endpoint());
        auto group = host_test_group(suffix++);
        REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
        host.set_service_available(true);
        REQUIRE(probe.until([&] { return host.eligibility().generate_model; }));
        const auto original = host.session().model_selection();
        REQUIRE(host.begin_model_selection());
        REQUIRE(host.session().toggle_candidate_view(ViewId::Left));
        Json ready; std::string error;
        host.prepare_model_selection([&](Json plan) { ready = std::move(plan); }, [&](std::string reason) { error = std::move(reason); });
        REQUIRE(probe.until([&] { return probe.count("/model-plans") == 1; }));
        const auto request = probe.at("/model-plans");
        auto plan = probe.model_plan(request, group.id);
        if (change == "expired") plan["expires_at"] = 1.0;
        probe.reply(request, {{"plan", plan}});
        REQUIRE(probe.until([&] { return !ready.is_null() || !error.empty(); }));
        REQUIRE(error.empty()); REQUIRE_FALSE(ready.is_null());
        if (change == "version" || change == "sha" || change == "epoch") {
            ++group.revision;
            if (change == "version") group.views[0].version_id = "front-v2";
            if (change == "sha") group.views[0].sha256 = std::string(64, 'f');
            if (change == "epoch") group.input_epoch = host_test_group(98).id;
            REQUIRE(host.session().apply_snapshot(group, host.session().context()));
        }
        if (change == "creation") host.new_creation();
        if (change == "draft") {
            auto draft = host.session().draft().design; draft.prompt += " changed";
            REQUIRE(host.session().set_design_draft(draft));
        }
        if (change == "model") {
            auto options = host.session().draft().geometry; options.model = "another-model";
            host.session().set_model_options(std::move(options));
        }
        if (change == "provider" || change == "face-limit" || change == "geometry-quality" ||
            change == "texture-quality" || change == "format") {
            auto options = host.session().draft().geometry;
            if (change == "provider") options.provider = "yoyoclaw";
            if (change == "face-limit") options.face_limit = 10000;
            if (change == "geometry-quality") options.geometry_quality = "detailed";
            if (change == "texture-quality") options.texture_quality = "detailed";
            if (change == "format") options.output_format = "obj";
            host.session().set_model_options(std::move(options));
        }
        if (change == "capability") {
            probe.capabilities["model_providers"]["tripo"]["supported_image_counts"] = Json::array();
            host.refresh_capabilities("geometry");
            REQUIRE(probe.until([&] { return !host.model_selection_eligibility().generate_model; }));
        }
        if (change == "read-only") host.set_editing_guard([] { return false; });
        if (change == "cancel") host.cancel_model_selection();
        std::string rejected;
        if (change == "draft" || change == "epoch") {
            REQUIRE(host.confirm_model_selection(ready, [&](std::string reason) { rejected = std::move(reason); }));
            REQUIRE(probe.until([&] { return probe.count("/model-jobs") == 1; }));
            CHECK(rejected.empty());
            host.shutdown();
            continue;
        }
        CHECK_FALSE(host.confirm_model_selection(ready, [&](std::string reason) { rejected = std::move(reason); }));
        CHECK_FALSE(rejected.empty());
        probe.drain(); CHECK(probe.count("/model-jobs") == 0); CHECK_FALSE(host.has_pending_receipt());
        if (change != "creation") CHECK(host.session().model_selection() == original);
        if (change == "version" || change == "sha" || change == "epoch" || change == "creation") {
            if (change == "creation") {
                group = host_test_group(suffix++);
                REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
            }
            REQUIRE(host.begin_model_selection());
            REQUIRE(host.session().toggle_candidate_view(ViewId::Left));
            Json renewed; std::string renew_error;
            host.prepare_model_selection([&](Json value) { renewed = std::move(value); },
                [&](std::string reason) { renew_error = std::move(reason); });
            REQUIRE(probe.until([&] { return probe.count("/model-plans") == 2; }));
            const auto next = probe.at("/model-plans", 1);
            const auto& payload = probe.requests[next].payload;
            CHECK(probe.requests[next].path == "/v1/orcaslicer/advanced-multiview/groups/" + group.id + "/model-plans");
            CHECK_FALSE(payload.contains("expected_input_epoch"));
            CHECK_FALSE(payload.contains("expected_group_revision"));
            CHECK_FALSE(payload.contains("draft"));
            CHECK(payload.at("visible_versions").at("front").at("version_id") == group.views[0].version_id);
            CHECK(payload.at("visible_versions").at("front").at("sha256") == group.views[0].sha256);
            probe.reply(next, {{"plan", probe.model_plan(next, group.id)}});
            REQUIRE(probe.until([&] { return !renewed.is_null() || !renew_error.empty(); }));
            REQUIRE(renew_error.empty()); REQUIRE_FALSE(renewed.is_null());
            CHECK(renewed.at("id") != ready.at("id"));
            const auto& frozen = renewed.at("approved_multiview_snapshot");
            CHECK(frozen.at("group_id") == group.id);
            CHECK(Slic3r::GUI::AdvancedMultiviewClient::valid_id(frozen.at("input_epoch").get<std::string>()));
            CHECK(frozen.at("views").at("front").at("version_id") == group.views[0].version_id);
            CHECK(frozen.at("views").at("front").at("sha256") == group.views[0].sha256);
            CHECK(probe.count("/model-jobs") == 0);
        }
        host.shutdown();
    }
}

TEST_CASE("Local generation rejects incompatible plans received through the real client without executing them",
          "[AdvancedCreation][Advanced2DPreview][Regression]")
{
    using Json = nlohmann::json;
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-local-plan-guards"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    unsigned suffix = 30;
    for (const std::string variant : {"legacy", "scope", "target", "text", "targets", "dependencies", "calls", "base", "revision", "group", "id"}) {
        CAPTURE(variant);
        AdvancedHttpProbe probe;
        Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, probe.endpoint());
        const auto group = host_test_group(suffix++);
        REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
        host.set_service_available(true);
        REQUIRE(probe.until([&] { return host.eligibility().generate_design; }));
        const auto original = host.session().local_draft();
        int callbacks = 0; bool accepted = true;
        host.generate_single_design(ViewId::Left, "only this left detail", [&](bool value) { accepted = value; ++callbacks; });
        REQUIRE(probe.until([&] { return probe.count("/plans") == 1; }));
        const auto request = probe.at("/plans");
        auto plan = probe.local_plan(request, group);
        if (variant == "legacy") plan.erase("design_request");
        if (variant == "scope") plan["design_request"]["scope"] = "all_inputs";
        if (variant == "target") plan["design_request"]["target_view"] = "right";
        if (variant == "text") plan["design_request"]["local_prompt"] = "another instruction";
        if (variant == "targets") plan["targets"].push_back("front");
        if (variant == "dependencies") plan["dependency_views"].push_back("front");
        if (variant == "calls") plan["max_new_image_calls"] = 2;
        if (variant == "base") plan["base_versions"]["left"] = "left-v2";
        if (variant == "revision") plan["expected_group_revision"] = group.revision + 1;
        if (variant == "group") plan["group_id"] = host_test_group(99).id;
        if (variant == "id") plan["id"] = host_test_group(98).id;
        probe.reply(request, {{"plan", plan}});
        REQUIRE(probe.until([&] { return callbacks == 1; }));
        CHECK_FALSE(accepted); CHECK_FALSE(host.command_pending()); CHECK_FALSE(host.has_pending_receipt());
        CHECK_FALSE(host.message().empty());
        probe.drain(); CHECK(probe.count("/operations") == 0);
        CHECK(host.session().draft().design == original.input.design);
        CHECK(host.session().model_selection() == original.model_selected);
        for (const auto view : AllViews) CHECK(host.session().group()->views[index(view)].version_id == group.views[index(view)].version_id);
        host.shutdown();
    }
}

TEST_CASE("Late local plans cannot execute changed inputs or release a newer creation command",
          "[AdvancedCreation][Advanced2DPreview][Regression]")
{
    using Json = nlohmann::json;
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-local-plan-order"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    unsigned suffix = 50;
    for (const std::string change : {"draft", "version", "creation"}) {
        CAPTURE(change);
        AdvancedHttpProbe probe;
        Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, probe.endpoint());
        auto group = host_test_group(suffix++);
        REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
        host.set_service_available(true);
        REQUIRE(probe.until([&] { return host.eligibility().generate_design; }));
        int old_callbacks = 0, new_callbacks = 0;
        host.generate_single_design(ViewId::Left, "old local", [&](bool accepted) { CHECK_FALSE(accepted); ++old_callbacks; });
        REQUIRE(probe.until([&] { return probe.count("/plans") == 1; }));
        const auto old_request = probe.at("/plans");
        const auto old_plan = probe.local_plan(old_request, group);
        if (change == "draft") {
            auto draft = host.session().draft().design; draft.prompt += " changed";
            REQUIRE(host.session().set_design_draft(draft));
        }
        if (change == "version") {
            ++group.revision; group.views[1].version_id = "left-v2";
            REQUIRE(host.session().apply_snapshot(group, host.session().context()));
        }
        if (change == "creation") {
            host.new_creation(); group = host_test_group(suffix++);
            REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
            host.generate_single_design(ViewId::Right, "new local", [&](bool accepted) { CHECK_FALSE(accepted); ++new_callbacks; });
            REQUIRE(probe.until([&] { return probe.count("/plans") == 2; }));
        }
        probe.reply(old_request, {{"plan", old_plan}});
        probe.drain(); CHECK(probe.count("/operations") == 0);
        if (change == "creation") {
            CHECK(old_callbacks == 0); CHECK(new_callbacks == 0); CHECK(host.command_pending());
            const auto current = probe.at("/plans", 1);
            const auto plan = probe.local_plan(current, group);
            probe.reply(current, {{"plan", plan}});
            REQUIRE(probe.until([&] { return probe.count("/operations") == 1; }));
            const auto submitted = probe.at("/operations");
            CHECK(probe.requests[submitted].path == "/v1/orcaslicer/advanced-multiview/groups/" + group.id + "/operations");
            CHECK(probe.requests[submitted].payload.at("plan_id") == plan.at("id"));
            probe.reply(submitted, Json::object(), true);
            REQUIRE(probe.until([&] { return new_callbacks == 1; }));
            CHECK(host.has_pending_receipt());
        } else {
            CHECK(old_callbacks == 1); CHECK_FALSE(host.command_pending()); CHECK_FALSE(host.has_pending_receipt());
        }
        host.shutdown();
    }
}

TEST_CASE("Late cache failures cannot change a newer local or model command and remain safe after host destruction",
          "[AdvancedCreation][Advanced2DPreview][AdvancedImageRefresh][Regression]")
{
    using Json = nlohmann::json;
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-cache-command-order"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    unsigned suffix = 200;
    // The output directory is process-scoped. The local branch deliberately
    // leaves an uncertain sent receipt, so exercise it after the other hosts.
    for (const std::string command : {"model", "destroy", "local"}) {
        CAPTURE(command);
        AdvancedHttpProbe probe;
        auto host = std::make_unique<Slic3r::GUI::AdvancedCreationFeatureHost>(nullptr, probe.endpoint());
        const auto old_group = host_test_group(suffix++);
        const auto asset = host_test_group(suffix++).id;
        const auto group_route = "/groups/" + old_group.id;
        const auto asset_route = "/assets/" + asset;
        probe.held_suffixes = {group_route, asset_route};
        host->set_service_available(true);
        host->open_group(old_group.id);
        REQUIRE(probe.until([&] { return probe.count(group_route) == 1; }));
        const auto draft = AdvancedMultiviewClient::design_json(old_group.saved_draft);
        Json snapshot {{"schema", "advanced-multiview-group-v1"}, {"workflow_kind", "advanced_multiview_v1"},
            {"id", old_group.id}, {"input_epoch", old_group.input_epoch}, {"revision", old_group.revision},
            {"initial_input", draft}, {"saved_draft", draft}, {"latest_submitted_draft", draft},
            {"active_operation_id", nullptr}, {"stop_intent", false}, {"published", true},
            {"uploads", Json::object()}, {"views", Json::object()}};
        for (const auto view : AllViews) {
            snapshot["uploads"][view_name(view)] = nullptr;
            snapshot["views"][view_name(view)] = {{"current_version_id", nullptr}, {"current", nullptr}, {"latest_attempt", nullptr}};
        }
        snapshot["views"]["front"]["current_version_id"] = asset;
        snapshot["views"]["front"]["current"] = {{"id", asset}, {"sha256", std::string(64, 'a')}};
        probe.reply(probe.at(group_route), {{"group", snapshot}});
        REQUIRE(probe.until([&] { return probe.count(asset_route) == 1; }));
        REQUIRE(host->session().group()->views[0].asset_pending);

        host->new_creation();
        const auto current = host_test_group(suffix++);
        REQUIRE(host->session().apply_snapshot(current, host->session().begin_open(current.id), true));
        REQUIRE(probe.until([&] { return host->eligibility().generate_design; }));
        const auto selection = host->session().model_selection();
        std::vector<bool> submissions;
        Json model_plan; std::string model_error;
        if (command == "model") {
            REQUIRE(host->begin_model_selection());
            host->prepare_model_selection([&](Json value) { model_plan = std::move(value); },
                [&](std::string error) { model_error = std::move(error); });
            REQUIRE(probe.until([&] { return probe.count("/model-plans") == 1; }));
        } else {
            host->generate_single_design(ViewId::Left, "new creation only", [&](bool accepted) { submissions.push_back(accepted); });
            REQUIRE(probe.until([&] { return probe.count("/plans") == 1; }));
        }
        const auto request = probe.at(command == "model" ? "/model-plans" : "/plans");
        if (command == "destroy") {
            int notifications = 0;
            host->set_listener([&] { ++notifications; });
            const auto before = notifications;
            host->shutdown(); host.reset();
            probe.drop(probe.at(asset_route)); probe.drop(request); probe.drain();
            CHECK(notifications == before); CHECK(submissions.empty());
            CHECK(probe.count("/operations") == 0); CHECK(probe.count("/model-jobs") == 0);
            continue;
        }
        probe.reply(probe.at(asset_route), {{"error", "old image unavailable"}}, true);
        probe.drain();
        REQUIRE(host->session().group());
        CHECK(host->session().group()->id == current.id);
        CHECK(host->session().model_selection() == selection);
        for (const auto view : AllViews) {
            const auto& image = host->session().group()->views[index(view)];
            CHECK(image.version_id == current.views[index(view)].version_id);
            CHECK(image.usable); CHECK_FALSE(image.display_error); CHECK_FALSE(image.asset_pending);
        }
        CHECK(submissions.empty()); CHECK(model_plan.is_null()); CHECK(model_error.empty());
        CHECK(probe.count("/operations") == 0); CHECK(probe.count("/model-jobs") == 0);
        if (command == "model") {
            REQUIRE(host->session().model_candidate().has_value());
            probe.reply(request, {{"plan", probe.model_plan(request, current.id)}});
            REQUIRE(probe.until([&] { return !model_plan.is_null() || !model_error.empty(); }));
            CHECK(model_error.empty()); CHECK(model_plan.at("group_id") == current.id);
            host->cancel_model_selection();
        } else {
            REQUIRE(host->command_pending());
            const auto plan = probe.local_plan(request, current);
            probe.reply(request, {{"plan", plan}});
            REQUIRE(probe.until([&] { return probe.count("/operations") == 1; }));
            const auto execute = probe.at("/operations");
            CHECK(probe.requests[execute].payload.at("plan_id") == plan.at("id"));
            CHECK(probe.requests[request].payload.at("design_request").at("target_view") == "left");
            probe.reply(execute, Json::object(), true);
            REQUIRE(probe.until([&] { return submissions == std::vector<bool>{false}; }));
            CHECK(host->has_pending_receipt());
        }
        host->shutdown();
    }
}

TEST_CASE("A late verified image preserves a new creation and its initial guidance",
          "[AdvancedCreation][Advanced2DPreview][AdvancedImageRefresh][AdvancedInitialGuidance][Regression]")
{
    using Json = nlohmann::json;
    wxInitializer init; REQUIRE(init.IsOk());
    wxInitAllImageHandlers();
    ScopedTemporaryDir temporary {"orca-advanced-late-image-guidance"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    const auto source = temporary.path() / "old-image.png";
    wxImage image(64, 64); image.SetRGB(wxRect(0, 0, 64, 64), 40, 140, 180);
    REQUIRE(image.SaveFile(wxString(source.wstring()), wxBITMAP_TYPE_PNG));
    std::ifstream input(source.string(), std::ios::binary);
    const std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest {}; unsigned length = 0;
    REQUIRE(EVP_Digest(body.data(), body.size(), digest.data(), &length, EVP_sha256(), nullptr) == 1);
    std::string hash;
    for (unsigned i = 0; i < length; ++i) {
        constexpr char hex[] = "0123456789abcdef";
        hash += hex[digest[i] >> 4]; hash += hex[digest[i] & 15];
    }
    unsigned suffix = 400;
    for (const bool old_image_first : {true, false}) for (const std::size_t count : {1u, 2u, 3u, 4u}) {
        CAPTURE(old_image_first, count);
        AdvancedHttpProbe probe;
        Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, probe.endpoint());
        const auto old_group = host_test_group(suffix++);
        const auto asset = host_test_group(suffix++).id;
        const auto group_route = "/groups/" + old_group.id;
        const auto asset_route = "/assets/" + asset;
        probe.held_suffixes = {group_route, asset_route};
        host.set_service_available(true);
        host.open_group(old_group.id);
        REQUIRE(probe.until([&] { return probe.count(group_route) == 1; }));
        const auto draft = AdvancedMultiviewClient::design_json(old_group.saved_draft);
        Json snapshot {{"schema", "advanced-multiview-group-v1"}, {"workflow_kind", "advanced_multiview_v1"},
            {"id", old_group.id}, {"input_epoch", old_group.input_epoch}, {"revision", old_group.revision},
            {"initial_input", draft}, {"saved_draft", draft}, {"latest_submitted_draft", draft},
            {"active_operation_id", nullptr}, {"stop_intent", false}, {"published", true},
            {"uploads", Json::object()}, {"views", Json::object()}};
        for (const auto view : AllViews) {
            snapshot["uploads"][view_name(view)] = nullptr;
            snapshot["views"][view_name(view)] = {{"current_version_id", nullptr}, {"current", nullptr}, {"latest_attempt", nullptr}};
        }
        snapshot["views"]["front"]["current_version_id"] = asset;
        snapshot["views"]["front"]["current"] = {{"id", asset}, {"sha256", hash}};
        probe.reply(probe.at(group_route), {{"group", snapshot}});
        REQUIRE(probe.until([&] { return probe.count(asset_route) == 1; }));
        REQUIRE(host.session().group()->views[0].asset_pending);

        host.new_creation();
        auto current = host_test_group(suffix++);
        current.design_targets.fill(false);
        std::vector<ViewId> targets;
        for (std::size_t i = 0; i < count; ++i) {
            targets.push_back(AllViews[i]); current.design_targets[i] = true;
            current.uploads[i] = std::string(view_name(AllViews[i])) + "-upload.png";
            current.views[i].canonical_version_id = current.views[i].version_id;
        }
        const auto last = index(targets.back());
        auto pending = current;
        pending.views[last].usable = false; pending.views[last].asset_pending = true;
        const auto context = host.session().begin_open(current.id);
        REQUIRE(host.session().apply_snapshot(pending, context, true));
        host.session().arm_initial_design_guidance(context, targets, current.input_epoch);
        CHECK_FALSE(host.session().consume_initial_design_guidance());
        const auto release_old_image = [&] {
            const auto preferences = host.session().model_selection();
            probe.reply_bytes(probe.at(asset_route), body, "image/png");
            const auto cached = AdvancedMultiviewClient::asset_path(temporary.path() / "advanced-cache", old_group.id, asset);
            REQUIRE(probe.until([&] { return AdvancedMultiviewClient::cached_asset_matches(cached, hash); }));
            probe.drain();
            REQUIRE(host.session().group());
            CHECK(host.session().group()->id == current.id);
            CHECK(host.session().group()->input_epoch == current.input_epoch);
            CHECK(host.session().context().epoch == context.epoch);
            CHECK(host.session().model_selection() == preferences);
            CHECK(probe.count("/operations") == 0); CHECK(probe.count("/model-jobs") == 0);
        };
        if (old_image_first) {
            release_old_image();
            CHECK(host.session().group()->views[last].asset_pending);
            CHECK_FALSE(host.session().group()->views[last].usable);
            CHECK_FALSE(host.session().consume_initial_design_guidance());
        }
        ++current.revision;
        REQUIRE(host.session().apply_snapshot(current, context));
        REQUIRE(host.session().consume_initial_design_guidance());
        CHECK_FALSE(host.session().consume_initial_design_guidance());
        if (!old_image_first) release_old_image();
        for (const auto view : targets) {
            CHECK(host.session().group()->views[index(view)].version_id == current.views[index(view)].version_id);
            CHECK(host.session().group()->views[index(view)].usable);
        }
        CHECK_FALSE(host.session().consume_initial_design_guidance());
        CHECK_FALSE(host.has_pending_receipt());
        host.shutdown();
    }
}

TEST_CASE("Serialized upload saves preserve a newer replacement and deletion while an older cache result arrives",
          "[AdvancedCreation][Advanced2DPreview][AdvancedDraftInput][AdvancedImageRefresh][Regression]")
{
    using Json = nlohmann::json;
    wxInitializer init; REQUIRE(init.IsOk());
    wxInitAllImageHandlers();
    ScopedTemporaryDir temporary {"orca-advanced-upload-save-order"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    const auto replacement = temporary.path() / "replacement.png";
    wxImage image(64, 64); image.SetRGB(wxRect(0, 0, 64, 64), 40, 140, 180);
    REQUIRE(image.SaveFile(wxString(replacement.wstring()), wxBITMAP_TYPE_PNG));
    std::ifstream image_input(replacement.string(), std::ios::binary);
    const std::string body((std::istreambuf_iterator<char>(image_input)), std::istreambuf_iterator<char>());
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest {}; unsigned length = 0;
    REQUIRE(EVP_Digest(body.data(), body.size(), digest.data(), &length, EVP_sha256(), nullptr) == 1);
    std::string hash;
    for (unsigned i = 0; i < length; ++i) {
        constexpr char hex[] = "0123456789abcdef";
        hash += hex[digest[i] >> 4]; hash += hex[digest[i] & 15];
    }
    auto group = host_test_group(240);
    const auto front = host_test_group(241).id, left = host_test_group(242).id;
    const auto first_upload = host_test_group(243).id;
    const auto cache = temporary.path() / "advanced-cache";
    boost::filesystem::create_directories(cache / group.id);
    for (const auto& asset : {front, left}) {
        std::ofstream stored(AdvancedMultiviewClient::asset_path(cache, group.id, asset).string(), std::ios::binary);
        stored.write(body.data(), static_cast<std::streamsize>(body.size())); stored.close(); REQUIRE(stored.good());
    }
    group.uploads = {AdvancedMultiviewClient::asset_path(cache, group.id, front).string(),
                     AdvancedMultiviewClient::asset_path(cache, group.id, left).string(), "", ""};
    group.design_targets = {true, true, false, false};
    for (std::size_t slot = 0; slot < 2; ++slot) {
        auto& view = group.views[slot];
        view.version_id = view.canonical_version_id = slot ? left : front;
        view.image_path = group.uploads[slot]; view.sha256 = hash;
    }
    group.views[2] = group.views[3] = {};
    const auto snapshot = [&](std::uint64_t revision, const std::string& upload) {
        const auto draft = AdvancedMultiviewClient::design_json(group.saved_draft);
        Json value {{"schema", "advanced-multiview-group-v1"}, {"workflow_kind", "advanced_multiview_v1"},
            {"id", group.id}, {"input_epoch", host_test_group(unsigned(250 + revision)).id}, {"revision", revision},
            {"initial_input", draft}, {"saved_draft", draft}, {"latest_submitted_draft", draft},
            {"active_operation_id", nullptr}, {"stop_intent", false}, {"published", true},
            {"design_targets", upload.empty() ? Json::array({"front"}) : Json::array({"front", "left"})},
            {"pending_views", upload.empty() ? Json::array() : Json::array({"left"})},
            {"uploads", Json::object()}, {"views", Json::object()}};
        for (const auto view : AllViews) {
            value["uploads"][view_name(view)] = nullptr;
            value["views"][view_name(view)] = {{"current_version_id", nullptr}, {"current", nullptr}, {"latest_attempt", nullptr}};
        }
        value["uploads"]["front"] = {{"asset_id", front}, {"sha256", hash}};
        if (!upload.empty()) value["uploads"]["left"] = {{"asset_id", upload}, {"sha256", hash}};
        value["views"]["front"]["current_version_id"] = front;
        value["views"]["front"]["current"] = {{"id", front}, {"sha256", hash}};
        return value;
    };
    AdvancedHttpProbe probe;
    const auto delayed_asset = "/assets/" + first_upload;
    const auto group_route = "/groups/" + group.id;
    probe.held_suffixes = {"/inputs", delayed_asset, group_route};
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, probe.endpoint());
    REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
    host.set_service_available(true);
    REQUIRE(probe.until([&] { return host.eligibility().generate_design; }));
    REQUIRE(host.session().set_upload(ViewId::Left, replacement.string(), true)); host.changed();
    REQUIRE(probe.until([&] { return probe.count("/inputs") == 1; }));
    CHECK_FALSE(host.session().group()->views[1].usable);
    const auto& multipart = probe.requests[probe.at("/inputs")].payload;
    REQUIRE(multipart.is_string());
    CHECK(multipart.get<std::string>().find("name=\"replaced_views\"") != std::string::npos);
    REQUIRE(host.session().set_upload(ViewId::Left, replacement.string(), true)); host.changed();
    REQUIRE(host.session().set_upload(ViewId::Left, "")); host.changed();
    const auto newest_input = host.session().draft();
    // Editing remains available, but background saves are serialized. An
    // inverse HTTP completion order cannot be manufactured through this host.
    const auto deferred_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (std::chrono::steady_clock::now() < deferred_until) probe.pump();
    CHECK(probe.count("/inputs") == 1); CHECK_FALSE(host.draft_editing_locked());
    probe.reply(probe.at("/inputs", 0), {{"group", snapshot(group.revision + 1, first_upload)}});
    REQUIRE(probe.until([&] { return probe.count(delayed_asset) == 1; }));
    CHECK(host.session().draft_dirty()); CHECK(host.session().draft().uploads[1].empty());
    CHECK(host.session().draft().upload_revisions[1] == newest_input.upload_revisions[1]);
    CHECK_FALSE(host.session().group()->views[1].usable);
    CHECK(probe.count("/inputs") == 1);
    probe.reply(probe.at(delayed_asset), {{"error", "obsolete upload unavailable"}}, true);
    REQUIRE(probe.until([&] { return probe.count("/inputs") == 2; }));
    CHECK(host.session().draft_dirty()); CHECK(host.session().draft().uploads[1].empty());
    probe.reply(probe.at("/inputs", 1), {{"group", snapshot(group.revision + 2, "")}});
    REQUIRE(probe.until([&] { return !host.session().draft_dirty() && host.eligibility().generate_design; }));
    CHECK_FALSE(host.session().group()->views[1].usable);
    CHECK(host.session().draft().upload_revisions[1] == newest_input.upload_revisions[1]);
    CHECK_FALSE(host.session().draft().replaced_uploads[1]);

    host.open_group(group.id);
    REQUIRE(probe.until([&] { return probe.count(group_route) == 1; }));
    probe.reply(probe.at(group_route), {{"group", snapshot(group.revision + 2, "")}});
    REQUIRE(probe.until([&] { return host.eligibility().generate_design; }));
    REQUIRE(host.session().group());
    CHECK(host.session().group()->revision == group.revision + 2);
    CHECK(host.session().draft().uploads[1].empty()); CHECK_FALSE(host.session().draft_dirty());
    CHECK_FALSE(host.session().group()->views[1].usable);
    CHECK(host.session().group()->views[0].version_id == front);
    CHECK_FALSE(host.session().draft().replaced_uploads[1]);
    CHECK(probe.count("/inputs") == 2); CHECK(probe.count("/plans") == 0);
    CHECK(probe.count("/operations") == 0); CHECK(probe.count("/model-jobs") == 0);
    host.shutdown();
}

TEST_CASE("Local submission closes only after its original acceptance is verified without another execute",
          "[AdvancedCreation][AdvancedLocalDesign][Regression]")
{
    using Json = nlohmann::json;
    const bool accepted = GENERATE(false, true);
    const bool mismatched_response = GENERATE(false, true);
    CAPTURE(accepted, mismatched_response);
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-local-acceptance"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    AdvancedHttpProbe probe;
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, probe.endpoint());
    const auto group = host_test_group(80 + unsigned(accepted) * 2 + unsigned(mismatched_response));
    REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
    host.set_service_available(true);
    REQUIRE(probe.until([&] { return host.eligibility().generate_design; }));
    std::vector<bool> callbacks;
    host.generate_single_design(ViewId::Left, "only this left detail", [&](bool value) { callbacks.push_back(value); });
    REQUIRE(probe.until([&] { return probe.count("/plans") == 1; }));
    const auto plan = probe.local_plan(probe.at("/plans"), group);
    probe.reply(probe.at("/plans"), {{"plan", plan}});
    REQUIRE(probe.until([&] { return probe.count("/operations") == 1; }));
    CHECK(callbacks.empty()); CHECK(host.has_pending_receipt());
    const auto execute = probe.at("/operations");
    CHECK(probe.requests[execute].payload.at("plan_id") == plan.at("id"));
    CHECK(probe.requests[execute].payload.at("confirmation_fingerprint") == plan.at("confirmation_fingerprint"));
    if (mismatched_response) {
        auto wrong = plan.at("design_request"); wrong["target_view"] = "right";
        probe.reply(execute, {{"operation", {{"id", plan.at("id")}, {"group_id", group.id}, {"design_request", wrong}}}});
    } else {
        probe.reply(execute, Json::object(), true);
    }
    REQUIRE(probe.until([&] { return !host.command_pending(); }));
    CHECK(callbacks == (mismatched_response ? std::vector<bool>{} : std::vector<bool>{false}));
    CHECK(host.has_pending_receipt());
    for (const auto view : AllViews) CHECK(host.session().group()->views[index(view)].version_id == group.views[index(view)].version_id);
    int blocked_callbacks = 0;
    host.generate_single_design(ViewId::Left, "only this left detail", [&](bool value) { CHECK_FALSE(value); ++blocked_callbacks; });
    CHECK(blocked_callbacks == 1); probe.drain(); CHECK(probe.count("/operations") == 1);
    host.reconcile_submission();
    REQUIRE(probe.until([&] { return probe.count("/reconcile") == 1; }));
    const auto reconcile = probe.at("/reconcile");
    CHECK(probe.requests[reconcile].path == "/v1/orcaslicer/advanced-multiview/groups/" + group.id +
          "/operations/" + plan.at("id").get<std::string>() + "/reconcile");
    const auto draft = AdvancedMultiviewClient::design_json(group.saved_draft);
    Json restored {{"schema", "advanced-multiview-group-v1"}, {"workflow_kind", "advanced_multiview_v1"},
        {"id", group.id}, {"input_epoch", group.input_epoch}, {"revision", group.revision + 1},
        {"initial_input", draft}, {"saved_draft", draft}, {"latest_submitted_draft", draft},
        {"active_operation_id", nullptr}, {"stop_intent", false}, {"published", true},
        {"uploads", Json::object()}, {"views", Json::object()}};
    // Receipt acceptance precedes image-cache synchronization; no supplier or
    // downloadable asset is needed to verify this callback and command identity.
    for (const auto view : AllViews) {
        restored["uploads"][view_name(view)] = nullptr;
        restored["views"][view_name(view)] = {{"current_version_id", nullptr}, {"current", nullptr}, {"latest_attempt", nullptr}};
    }
    probe.reply(reconcile, {{"accepted", accepted}, {"group", restored}});
    REQUIRE(probe.until([&] { return !host.has_pending_receipt(); }));
    const auto expected = accepted
        ? (mismatched_response ? std::vector<bool>{true} : std::vector<bool>{false, true})
        : (mismatched_response ? std::vector<bool>{} : std::vector<bool>{false});
    CHECK(callbacks == expected); CHECK(probe.count("/operations") == 1);
    if (!accepted) {
        host.generate_single_design(ViewId::Left, "explicit corrected retry", [&](bool value) { CHECK_FALSE(value); });
        REQUIRE(probe.until([&] { return probe.count("/plans") == 2; }));
        const auto retry = probe.at("/plans", 1);
        CHECK(probe.requests[retry].payload.at("command_id") != plan.at("id"));
        CHECK(probe.requests[retry].payload.at("design_request").at("target_view") == "left");
        CHECK(probe.requests[retry].payload.at("design_request").at("local_prompt") == "explicit corrected retry");
        probe.reply(retry, Json::object(), true);
        REQUIRE(probe.until([&] { return !host.command_pending(); }));
        CHECK_FALSE(host.has_pending_receipt()); CHECK(probe.count("/operations") == 1);
    }
    host.shutdown();
}

TEST_CASE("An unsent local receipt failure permits an explicit retry after storage recovers",
          "[AdvancedCreation][AdvancedLocalDesign][Regression]")
{
    using Json = nlohmann::json;
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-local-receipt-failure"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    AdvancedHttpProbe probe;
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, probe.endpoint());
    const auto group = host_test_group(88);
    REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
    host.set_service_available(true);
    REQUIRE(probe.until([&] { return host.eligibility().generate_design; }));
    int callbacks = 0;
    const auto submit = [&] {
        host.generate_single_design(ViewId::Left, "only this left detail", [&](bool value) {
            CHECK_FALSE(value); ++callbacks;
        });
    };
    submit(); REQUIRE(probe.until([&] { return probe.count("/plans") == 1; }));
    const auto plan = probe.local_plan(probe.at("/plans"), group);
    const auto blocked = temporary.path() / "advanced-cache" / "pending-submissions" /
        (plan.at("id").get<std::string>() + ".json.part");
    boost::filesystem::create_directories(blocked.parent_path());
    REQUIRE(boost::filesystem::create_directory(blocked));
    probe.reply(probe.at("/plans"), {{"plan", plan}});
    REQUIRE(probe.until([&] { return callbacks == 1; }));
    CHECK_FALSE(host.command_pending());
    CHECK(probe.count("/operations") == 0);
    CHECK(probe.count("/reconcile") == 0);
    CHECK(host.session().draft().design == group.saved_draft);
    for (const auto view : AllViews)
        CHECK(host.session().group()->views[index(view)].version_id == group.views[index(view)].version_id);
    REQUIRE_FALSE(host.has_pending_receipt());
    REQUIRE(boost::filesystem::remove(blocked));
    submit(); REQUIRE(probe.until([&] { return probe.count("/plans") == 2; }));
    const auto retry = probe.at("/plans", 1);
    const auto retry_plan = probe.local_plan(retry, group);
    CHECK(retry_plan.at("id") != plan.at("id"));
    CHECK(probe.requests[retry].payload.at("design_request").at("target_view") == "left");
    CHECK(probe.requests[retry].payload.at("design_request").at("local_prompt") == "only this left detail");
    probe.reply(retry, {{"plan", retry_plan}});
    REQUIRE(probe.until([&] { return probe.count("/operations") == 1; }));
    CHECK(probe.requests[probe.at("/operations")].payload.at("plan_id") == retry_plan.at("id"));
    probe.reply(probe.at("/operations"), Json::object(), true);
    REQUIRE(probe.until([&] { return callbacks == 2; }));
    CHECK(host.has_pending_receipt()); // This retry was sent, so preserve its uncertain receipt.
    CHECK(probe.count("/reconcile") == 0);
    host.shutdown();
}

TEST_CASE("A failed design stop keeps its operation stoppable and retries only the original stop",
          "[AdvancedCreation][AdvancedStopNavigation][Regression]")
{
    using Json = nlohmann::json;
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-local-stop-retry"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    AdvancedHttpProbe probe;
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, probe.endpoint());
    auto group = host_test_group(71);
    group.active_operation_id = host_test_group(72).id; group.execution_state = "running";
    group.views[1].operation_id = group.active_operation_id; group.views[1].attempt_state = "submitting";
    REQUIRE(host.session().apply_snapshot(group, host.session().begin_open(group.id), true));
    host.set_service_available(true);
    REQUIRE(probe.until([&] { return host.eligibility().stop; }));
    const auto versions = host.session().group()->views;
    host.stop(); REQUIRE(probe.until([&] { return probe.count("/stop") == 1; }));
    const auto first = probe.at("/stop");
    probe.reply(first, Json::object(), true);
    REQUIRE(probe.until([&] { return !host.command_pending(); }));
    CHECK(host.session().group()->active_operation_id == group.active_operation_id);
    CHECK_FALSE(host.session().group()->stop_intent); CHECK(host.eligibility().stop);
    host.stop(); REQUIRE(probe.until([&] { return probe.count("/stop") == 2; }));
    const auto second = probe.at("/stop", 1);
    CHECK(probe.requests[second].path == probe.requests[first].path);
    CHECK(probe.requests[second].payload == probe.requests[first].payload);
    CHECK(probe.count("/plans") == 0); CHECK(probe.count("/operations") == 0);
    for (const auto view : AllViews) CHECK(host.session().group()->views[index(view)].version_id == versions[index(view)].version_id);
    probe.reply(second, Json::object(), true);
    REQUIRE(probe.until([&] { return !host.command_pending(); }));
    CHECK(host.eligibility().stop); host.shutdown();
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
    group.input_epoch = "33333333-3333-4333-8333-333333333333";
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
    else {
        host.open_group(group.id);
        CHECK(host.session().matches(previous));
        REQUIRE(host.command_pending());
        connection.reset();
        REQUIRE(until([&] {
            if (!connection) connection.reset(server.Accept(false));
            return bool(connection);
        }));
        using Json = nlohmann::json;
        const auto draft = AdvancedMultiviewClient::design_json(group.saved_draft);
        Json snapshot {{"schema", "advanced-multiview-group-v1"}, {"workflow_kind", "advanced_multiview_v1"},
            {"id", group.id}, {"input_epoch", group.input_epoch}, {"revision", group.revision},
            {"initial_input", draft}, {"saved_draft", draft}, {"latest_submitted_draft", draft},
            {"active_operation_id", nullptr}, {"stop_intent", false}, {"published", true},
            {"uploads", Json::object()}, {"views", Json::object()}};
        for (const auto view : AllViews) {
            snapshot["uploads"][view_name(view)] = nullptr;
            snapshot["views"][view_name(view)] = {{"current_version_id", nullptr}, {"current", nullptr}, {"latest_attempt", nullptr}};
        }
        const auto body = Json({{"group", snapshot}}).dump();
        const auto reply = std::string("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ") +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        connection->Write(reply.data(), reply.size());
        REQUIRE(connection->LastCount() == reply.size());
        connection->Close();
        REQUIRE(until([&] { return !host.command_pending(); }));
        // Business restoration commits before asynchronous cache verification.
        // Generation eligibility remains guarded until verification completes.
        REQUIRE(until([&] { return host.eligibility().design_reason == "service_unavailable"; }));
    }
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
    auto uploaded = *session.group(); uploaded.uploads[0] = "front-upload.png";
    REQUIRE(session.apply_snapshot(uploaded, session.context(), true));
    CHECK(session.set_upload(ViewId::Right, "replacement.png"));
    CHECK(session.selection()[index(ViewId::Right)]);
    CHECK(session.group()->id == "group-a");
    Capabilities capabilities {true, true, true, true, true, {true, true, true, true}, true};
    HostGuards guards {true, true, true};
    auto eligibility = session.eligibility(capabilities, guards);
    CHECK(eligibility.generate_model);
    CHECK(session.effective_model_views().size() == 3);

    auto draft = session.draft().design;
    draft.style = "sculpture";
    CHECK(session.set_design_draft(draft));
    for (ViewId view : AllViews) CHECK(session.selection()[index(view)]);
}

TEST_CASE("Design pending views survive restore while verified versions remain available for modeling", "[AdvancedCreation]")
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
    CHECK(eligibility.generate_model);
    CHECK(session.effective_model_views().size() == 4);
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
    CHECK(session.eligibility(capabilities, guards).generate_model);
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
    CHECK_FALSE(session.eligibility(capabilities, guards).recover);
}

TEST_CASE("Advanced model generation ignores only unselected terminal unknown designs",
          "[AdvancedCreation][AdvancedModelSelection]")
{
    const auto problem = GENERATE(ViewId::Front, ViewId::Left, ViewId::Back, ViewId::Right);
    auto group = complete_group();
    group.views[index(problem)].attempt_state = "ambiguous";
    Session session;
    REQUIRE(session.apply_snapshot(group, session.begin_open(group.id), true));
    const Capabilities caps {true, true, true, true, true, {true, true, true, true}, true};
    const HostGuards guards {true, true, true};
    CHECK(session.eligibility(caps, guards).model_reason == "design_result_unknown");
    CHECK(session.eligibility(caps, guards).click_generate_model);
    CHECK(session.model_selection_entry_eligibility(caps, guards).generate_model == (problem != ViewId::Front));
    if (problem == ViewId::Front) {
        CHECK_FALSE(session.toggle_model_view(problem));
        return;
    }
    REQUIRE(session.toggle_model_view(problem));
    CHECK(session.eligibility(caps, guards).generate_model);
    CHECK(session.effective_model_views().size() == 3);
    for (const auto view : AllViews)
        if (view != ViewId::Front && view != problem) REQUIRE(session.toggle_model_view(view));
    CHECK(session.effective_model_views() == std::vector<ViewId>{ViewId::Front});
    CHECK(session.eligibility(caps, guards).generate_model);
    auto limited = caps;
    limited.model_image_counts = {false, false, false, true};
    CHECK(session.eligibility(limited, guards).model_reason == "unsupported_multiview");
    for (const std::string state : {"result_pending", "result_saved"}) {
        ++group.revision;
        group.views[index(problem)].attempt_state = state;
        REQUIRE(session.apply_snapshot(group, session.context()));
        CHECK(session.eligibility(caps, guards).generate_model == (problem != ViewId::Front));
        if (problem == ViewId::Front)
            CHECK(session.eligibility(caps, guards).model_reason == "recovery_required");
        CHECK(session.eligibility(caps, guards).click_generate_model);
    }
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
    const auto eligibility = session.eligibility(capabilities, guards);
    CHECK(eligibility.generate_design == (state != "result_pending" && state != "result_saved"));
    CHECK_FALSE(eligibility.generate_model);
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
    CHECK_FALSE(session.set_upload(ViewId::Left, "left.png"));
    CHECK(session.draft().uploads[1].empty());
    CHECK_FALSE(session.eligibility(capabilities, guards).generate_design);
    session.set_upload(ViewId::Front, "front.png");
    CHECK(session.set_upload(ViewId::Left, "left.png"));
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
    host.set_start_guard([] { return false; });
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
    REQUIRE_FALSE(host.draft_editing_locked());
    auto edited = original.design;
    edited.prompt = "Retain the workbench model and edit the creation draft";
    REQUIRE(host.session().set_design_draft(edited));
    CHECK(host.session().matches(context));
    CHECK(host.session().draft().design == edited);
    CHECK_FALSE(host.eligibility().click_generate_design);
    CHECK_FALSE(host.eligibility().click_generate_model);
    host.new_creation();
    CHECK_FALSE(host.session().matches(context));
    host.shutdown();
}

TEST_CASE("Model selection eligibility rejects creations without a loaded group while text generation remains available",
          "[AdvancedCreation][AdvancedModelSelection][Regression]")
{
    const std::string state = GENERATE("initial", "new-creation", "opening");
    Session session;
    if (state == "new-creation") {
        REQUIRE(session.apply_snapshot(complete_group(), session.begin_open("group-a"), true));
        session.new_creation();
    }
    if (state == "opening") session.begin_open("group-a");
    REQUIRE_FALSE(session.group());
    auto draft = session.draft().design;
    draft.prompt = "A friendly toy robot";
    REQUIRE(session.set_design_draft(draft));
    const Capabilities capabilities {true, true, true, true, true, {true, true, true, true}, true};
    const HostGuards guards {true, true, true};
    const auto entry = session.model_selection_entry_eligibility(capabilities, guards);
    CHECK(entry.generate_design);
    CHECK_FALSE(entry.generate_model);
    CHECK(entry.model_reason == "missing_current_view");
    session.begin_model_selection();
    const auto candidate = session.candidate_model_eligibility(capabilities, guards);
    CHECK_FALSE(candidate.generate_model);
    CHECK(candidate.model_reason == "missing_current_view");
    CHECK_FALSE(session.group());
}

TEST_CASE("Model selection entry rejects initial and reset creations without preparing requests or changing context",
          "[AdvancedCreation][AdvancedModelSelection][Regression]")
{
    const bool reset = GENERATE(false, true);
    wxInitializer init; REQUIRE(init.IsOk());
    ScopedTemporaryDir temporary {"orca-advanced-empty-model-selection"};
    ScopedEnvVar output("ORCASLICER_AI_OUTPUT_DIR", temporary.string());
    AdvancedHttpProbe probe;
    Slic3r::GUI::AdvancedCreationFeatureHost host(nullptr, probe.endpoint());
    if (reset) {
        REQUIRE(host.session().apply_snapshot(complete_group(), host.session().begin_open("group-a"), true));
        host.new_creation();
    }
    const auto context = host.session().context();
    std::string reason;
    CHECK_FALSE(host.begin_model_selection([&](const std::string& value) { reason = value; }));
    CHECK(reason == "missing_current_view");
    CHECK_FALSE(host.session().group());
    CHECK_FALSE(host.session().model_candidate());
    CHECK_FALSE(host.command_pending());
    CHECK_FALSE(host.has_pending_receipt());
    CHECK(host.session().matches(context));
    probe.drain();
    CHECK(probe.count("/model-plans") == 0);
    CHECK(probe.count("/model-jobs") == 0);
    host.shutdown();
}
