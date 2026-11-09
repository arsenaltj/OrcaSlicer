#include "AdvancedCreationFeatureHost.hpp"
#include "ModelGenerationPresentation.hpp"
#include "slic3r/GUI/AISidecarClient.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <map>
#include <limits>

namespace Slic3r::GUI {
using namespace AdvancedCreation;
namespace {
std::string new_identity() { return boost::uuids::to_string(boost::uuids::random_generator()()); }
}

AdvancedCreationFeatureHost::AdvancedCreationFeatureHost(wxWindow* owner)
    : m_owner(owner), m_client(AISidecarClient::default_endpoint()),
      m_cache(ModelGenerationPresentation::generated_models_root() / "advanced-cache"),
      m_poll_timer(this), m_save_timer(this)
{
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        const auto& group = m_session.group();
        // Idle snapshots are already current. Polling them also briefly blocks
        // explicit commands while their cached assets are verified.
        if (group && (!group->active_operation_id.empty() || group->model_busy)) poll();
    }, m_poll_timer.GetId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (!m_command_pending && !m_poll_pending) save_draft({});
        else m_save_timer.StartOnce(600);
    }, m_save_timer.GetId());
    try {
        const auto receipt = m_cache / "pending-submission.json";
        if (boost::filesystem::exists(receipt)) {
            if (!boost::filesystem::is_regular_file(receipt) || boost::filesystem::file_size(receipt) > 65536)
                throw std::runtime_error("invalid receipt");
            boost::filesystem::ifstream input(receipt);
            auto value = Json::parse(input, nullptr, false);
            if (value.is_object() && value.value("schema", std::string()) == "advanced-desktop-submission-v1" &&
                AdvancedMultiviewClient::valid_id(value.value("group_id", std::string())) &&
                AdvancedMultiviewClient::valid_id(value.value("operation_id", std::string()))) {
                m_pending_receipt = std::move(value);
                m_message = "上次确认的请求等待核对，请恢复原请求。";
            } else {
                // Preserve a damaged receipt and block new paid submissions.
                m_pending_receipt = Json::object(); m_message = "生成请求记录损坏，请检查本地记录后恢复。";
            }
        }
    } catch (const std::exception&) { m_pending_receipt = Json::object(); m_message = "生成请求记录暂时无法读取。"; }
}

AdvancedCreationFeatureHost::~AdvancedCreationFeatureHost() { shutdown(); }
void AdvancedCreationFeatureHost::shutdown()
{
    if (m_shutdown) return;
    m_shutdown = true;
    m_poll_timer.Stop(); m_save_timer.Stop(); m_listener = {}; m_model_job_listener = {};
    m_client.cancel();
}

AdvancedCreationFeatureHost::Complete AdvancedCreationFeatureHost::complete(std::function<void(Json)> callback, bool release_command, bool release_poll)
{
    wxWeakRef<AdvancedCreationFeatureHost> weak(this);
    return [weak, callback = std::move(callback), release_command, release_poll](Json value) {
        wxGetApp().CallAfter([weak, callback, release_command, release_poll, value = std::move(value)]() mutable {
            if (!weak || weak->m_shutdown) return;
            try { callback(std::move(value)); }
            catch (const Json::exception&) {
                if (release_command) weak->m_command_pending = false;
                if (release_poll) weak->m_poll_pending = false;
                weak->m_message = "高级服务返回的数据不完整，当前输入与结果已保留。"; weak->publish();
            }
        });
    };
}

AdvancedMultiviewClient::Error AdvancedCreationFeatureHost::error(bool release_command, bool release_poll)
{
    wxWeakRef<AdvancedCreationFeatureHost> weak(this);
    const auto context = m_session.context();
    return [weak, context, release_command, release_poll](std::string message) {
        wxGetApp().CallAfter([weak, context, release_command, release_poll, message = std::move(message)] {
            if (!weak || weak->m_shutdown || !weak->m_session.matches(context)) return;
            weak->m_message = message;
            if (release_command) weak->m_command_pending = false;
            if (release_poll) weak->m_poll_pending = false;
            weak->publish();
        });
    };
}

void AdvancedCreationFeatureHost::set_listener(std::function<void()> listener)
{ m_listener = std::move(listener); publish(); }
void AdvancedCreationFeatureHost::publish() { if (m_listener) m_listener(); }

void AdvancedCreationFeatureHost::set_service_available(bool available)
{
    const bool changed = available != m_service_available;
    m_service_available = available;
    if (available && changed) {
        refresh_capabilities(); refresh_history(); m_poll_timer.Start(1200);
    } else if (!available) { m_poll_timer.Stop(); }
    publish();
}

void AdvancedCreationFeatureHost::refresh_capabilities()
{
    if (!m_service_available) return;
    m_client.request("GET", "/capabilities", {}, complete([this](Json value) {
        if (value.value("schema", std::string()) != "advanced-multiview-capabilities-v1") {
            m_message = "高级模式需要匹配的本地服务版本。"; m_capabilities = {};
        } else { m_capabilities = std::move(value); }
        publish();
    }, false), error(false));
}

Capabilities AdvancedCreationFeatureHost::capabilities() const
{
    Capabilities result;
    result.service_available = m_service_available && m_capabilities.is_object();
    if (!result.service_available) return result;
    if (m_capabilities.contains("image_models")) for (const auto& model : m_capabilities["image_models"]) {
        if (model.value("provider", std::string()) != m_session.draft().design.provider ||
            (!m_session.draft().design.model.empty() && model.value("model", std::string()) != m_session.draft().design.model)) continue;
        result.initial_text = model.value("initial_text", std::string()) == "supported";
        result.initial_references = model.value("initial_named_references", std::string()) == "supported";
        result.edit_current = model.value("edit_current_with_context", std::string()) == "supported";
    }
    // D5 enables this only after the model-plan endpoint is present.
    if (m_capabilities.value("model_plans", false) && m_capabilities.contains("model_providers")) {
        const auto provider = m_capabilities["model_providers"].find(m_session.draft().geometry.provider);
        if (provider != m_capabilities["model_providers"].end())
            result.four_view_model = provider->value("named_four_view_model", std::string()) == "supported" &&
                                     provider->value("available", false);
    }
    return result;
}

Eligibility AdvancedCreationFeatureHost::eligibility() const
{
    HostGuards guards;
    guards.can_start = !m_command_pending && !m_poll_pending && !has_pending_receipt() && (!m_start_guard || m_start_guard());
    guards.model_service_available = m_service_available;
    return m_session.eligibility(capabilities(), guards);
}

void AdvancedCreationFeatureHost::changed()
{
    m_message.clear(); m_save_timer.StartOnce(650); publish();
}

void AdvancedCreationFeatureHost::refresh_history()
{
    if (!m_service_available) return;
    m_client.request("GET", "/groups?limit=100", {}, complete([this](Json value) {
        if (value.contains("groups") && value["groups"].is_array()) m_history = std::move(value["groups"]);
        publish();
    }, false), error(false));
}

void AdvancedCreationFeatureHost::ingest(Json group, CallbackContext context, bool restore, std::function<void()> done)
{
    auto parsed = AdvancedMultiviewClient::parse_group(group, m_cache);
    if (!parsed || !m_session.matches(context)) { m_poll_pending = false; return; }
    struct Pending {
        GroupSnapshot snapshot;
        std::size_t count {0};
        bool failed {false};
    };
    auto pending = std::make_shared<Pending>(); pending->snapshot = std::move(*parsed);
    struct Asset { std::string id, hash; };
    std::vector<Asset> assets;
    for (auto view : AllViews) {
        const auto name = view_name(view);
        const auto& upload = group.at("uploads").at(name);
        if (upload.is_object()) assets.push_back({upload.at("asset_id"), upload.at("sha256")});
        const auto& current = group.at("views").at(name).at("current");
        if (current.is_object()) assets.push_back({current.at("id"), current.at("sha256")});
    }
    auto finish = [this, pending, context, restore, done] {
        if (!m_session.matches(context)) return;
        m_poll_pending = false;
        if (!pending->failed) {
            m_session.apply_snapshot(std::move(pending->snapshot), context, restore);
            if (done) done();
        }
        publish();
    };
    for (const auto& asset : assets) {
        const auto path = AdvancedMultiviewClient::asset_path(m_cache, group.at("id"), asset.id);
        if (!AdvancedMultiviewClient::cached_asset_matches(path, asset.hash)) ++pending->count;
    }
    if (!pending->count) { finish(); return; }
    m_poll_pending = true;
    const auto group_id = group.at("id").get<std::string>();
    wxWeakRef<AdvancedCreationFeatureHost> weak(this);
    for (const auto& asset : assets) {
        const auto path = AdvancedMultiviewClient::asset_path(m_cache, group_id, asset.id);
        if (AdvancedMultiviewClient::cached_asset_matches(path, asset.hash)) continue;
        m_client.download_asset(group_id, asset.id, asset.hash, path,
            [weak, pending, finish] {
                wxGetApp().CallAfter([weak, pending, finish] {
                    if (weak && !weak->m_shutdown && --pending->count == 0) finish();
                });
            }, [weak, pending, finish](std::string message) {
                wxGetApp().CallAfter([weak, pending, finish, message = std::move(message)] {
                    if (!weak || weak->m_shutdown) return;
                    pending->failed = true; weak->m_message = message;
                    if (--pending->count == 0) finish();
                });
            });
    }
}

void AdvancedCreationFeatureHost::poll(bool restore)
{
    if (!m_service_available || m_shutdown || m_poll_pending || m_command_pending) return;
    auto context = m_session.context();
    if (context.group_id.empty()) return;
    m_poll_pending = true;
    m_client.request("GET", "/groups/" + context.group_id, {}, complete([this, context, restore](Json value) {
        if (!m_session.matches(context)) return;
        if (!value.contains("group")) { m_poll_pending = false; return; }
        ingest(std::move(value["group"]), context, restore, [this] {
            const auto& group = m_session.group();
            if (group && group->active_operation_id.empty() && !group->model_busy) refresh_history();
        });
    }, false, true), error(false, true));
}

void AdvancedCreationFeatureHost::save_draft(std::function<void()> done)
{
    if (!m_session.group() || !m_session.draft_dirty()) { if (done) done(); return; }
    if (!m_session.group()->active_operation_id.empty() || m_session.group()->model_busy) {
        m_save_timer.StartOnce(1000); if (done) done(); return;
    }
    const auto context = m_session.context();
    const auto input = m_session.draft();
    const auto draft = input.design;
    m_command_pending = true;
    if (input.uploads != m_session.group()->uploads) {
        m_client.update_group(context.group_id, m_session.group()->revision, input,
            complete([this, context, draft, done](Json value) {
                if (!m_session.matches(context)) { m_command_pending = false; return; }
                ingest(std::move(value.at("group")), context, false, [this, draft, done] {
                    m_session.mark_draft_saved(draft);
                    if (done) done(); else { m_command_pending = false; publish(); }
                });
            }), error());
        return;
    }
    m_client.request("PUT", "/groups/" + context.group_id + "/draft",
        {{"expected_group_revision", m_session.group()->revision}, {"draft", AdvancedMultiviewClient::design_json(draft)}},
        complete([this, context, draft, done](Json value) {
            if (!m_session.matches(context)) return;
            auto parsed = AdvancedMultiviewClient::parse_group(value.at("group"), m_cache);
            if (!parsed) { m_command_pending = false; return; }
            m_session.apply_snapshot(std::move(*parsed), context);
            m_session.mark_draft_saved(draft);
            if (done) done(); else { m_command_pending = false; publish(); }
        }), error());
}

void AdvancedCreationFeatureHost::open_group(const std::string& id)
{
    if (m_command_pending || m_poll_pending || has_pending_receipt() || (m_start_guard && !m_start_guard()) || !AdvancedMultiviewClient::valid_id(id)) return;
    m_command_pending = true;
    save_draft([this, id] {
        m_command_pending = false; m_session.begin_open(id); m_message.clear(); poll(true); publish();
    });
}

void AdvancedCreationFeatureHost::rename_group(const std::string& id, std::uint64_t revision, std::string display_name)
{
    if (m_command_pending || m_poll_pending || has_pending_receipt() ||
        (m_start_guard && !m_start_guard()) || !AdvancedMultiviewClient::valid_id(id)) return;
    m_command_pending = true;
    publish();
    auto rename = [this, id, display_name = std::move(display_name)](std::uint64_t expected_revision) {
        const bool current = m_session.group() && m_session.group()->id == id;
        const auto context = m_session.context();
        m_client.request("PUT", "/groups/" + id + "/name",
            {{"expected_group_revision", expected_revision}, {"display_name", display_name}},
            complete([this, current, context](Json value) {
                if (!current) {
                    m_command_pending = false;
                    refresh_history();
                    publish();
                    return;
                }
                ingest(std::move(value.at("group")), context, false, [this] {
                    m_command_pending = false;
                    refresh_history();
                });
            }), error());
    };
    if (m_session.group() && m_session.group()->id == id) {
        save_draft([this, rename = std::move(rename)] {
            if (!m_session.group()) { m_command_pending = false; publish(); return; }
            rename(m_session.group()->revision);
        });
    } else {
        rename(revision);
    }
}

void AdvancedCreationFeatureHost::new_creation()
{
    if (m_command_pending || m_poll_pending || has_pending_receipt() || (m_start_guard && !m_start_guard())) return;
    m_command_pending = true;
    save_draft([this] { m_command_pending = false; m_session.new_creation(); m_message.clear(); publish(); });
}

void AdvancedCreationFeatureHost::generate_design(Confirm confirm)
{
    m_session.flush_click(std::numeric_limits<std::uint64_t>::max());
    if (!eligibility().generate_design) return;
    m_command_pending = true; m_message.clear(); m_save_timer.Stop();
    if (!m_session.group()) {
        const auto context = m_session.command_context();
        const auto id = new_identity();
        m_client.create_group(id, m_session.draft(), complete([this, context, id, confirm](Json value) {
            if (!m_session.matches_command(context)) { m_command_pending = false; publish(); return; }
            const auto revision = value.at("group").at("revision");
            prepare(id, {{"command_id", new_identity()}, {"kind", "initial"}, {"expected_group_revision", revision}},
                    confirm, context, true);
        }), error());
    } else save_draft([this, confirm] {
        const auto& group = *m_session.group();
        auto request = AdvancedMultiviewClient::design_json(m_session.draft().design);
        request["command_id"] = new_identity(); request["expected_group_revision"] = group.revision;
        request["kind"] = group.published ? "edit" : "initial";
        request["target_views"] = Json::array(); request["base_versions"] = Json::object();
        for (auto view : m_session.effective_views()) {
            request["target_views"].push_back(view_name(view));
            request["base_versions"][view_name(view)] = group.views[index(view)].version_id;
        }
        prepare(group.id, std::move(request), confirm, m_session.command_context(), false);
    });
    publish();
}

void AdvancedCreationFeatureHost::generate_model(Confirm confirm)
{
    m_session.flush_click(std::numeric_limits<std::uint64_t>::max());
    if (!eligibility().generate_model || !m_session.group()) return;
    m_command_pending = true; m_message.clear(); m_save_timer.Stop();
    save_draft([this, confirm] {
        const auto context = m_session.command_context();
        const auto& options = m_session.draft().geometry;
        const auto group = m_session.group()->id;
        m_client.request("POST", "/groups/" + group + "/model-plans",
            {{"command_id", new_identity()}, {"expected_group_revision", m_session.group()->revision},
             {"draft", AdvancedMultiviewClient::design_json(m_session.draft().design)},
             {"provider", options.provider}, {"geometry_model", options.model}, {"face_limit", options.face_limit},
             {"geometry_quality", options.geometry_quality}, {"texture_quality", options.texture_quality}, {"output_format", options.output_format}},
            complete([this, group, context, confirm](Json value) {
                if (!m_session.matches_command(context, false)) { m_command_pending = false; publish(); return; }
                const auto& plan = value.at("plan");
                if (!confirm || !confirm(plan) || !m_session.matches_command(context, false)) {
                    m_command_pending = false; publish(); return;
                }
                if (!save_pending_receipt({{"schema", "advanced-desktop-submission-v1"}, {"kind", "model"},
                    {"group_id", group}, {"operation_id", plan.at("job_id")}, {"new_group", false}})) {
                    m_command_pending = false; publish(); return;
                }
                m_client.request("POST", "/groups/" + group + "/model-jobs",
                    {{"command_id", plan.at("id")}, {"plan_id", plan.at("id")},
                     {"confirmation_fingerprint", plan.at("confirmation_fingerprint")}},
                    complete([this](Json) {
                        m_command_pending = false;
                        const auto job = m_pending_receipt.value("operation_id", std::string());
                        if (!job.empty() && m_model_job_listener) m_model_job_listener(job);
                        reconcile_submission(false);
                    }), error());
            }), error());
    });
    publish();
}

void AdvancedCreationFeatureHost::resume_model()
{
    if (m_command_pending || has_pending_receipt() || !m_session.group()) return;
    const auto& group = *m_session.group();
    if (group.model_busy || !group.active_operation_id.empty() || !group.model_resumable ||
        (group.model_state != "stopped" && group.model_state != "failed")) return;
    const auto job = group.model_job_id;
    m_command_pending = true;
    m_client.request("POST", "/groups/" + group.id + "/model-jobs/" + group.model_job_id + "/resume", Json::object(),
        complete([this, job](Json) {
            m_command_pending = false; poll();
            if (!job.empty() && m_model_job_listener) m_model_job_listener(job);
            publish();
        }), error());
    publish();
}

void AdvancedCreationFeatureHost::prepare(const std::string& group, Json request, Confirm confirm,
                                        CommandContext context, bool new_group)
{
    m_client.request("POST", "/groups/" + group + "/plans", request,
        complete([this, group, confirm, context, new_group](Json value) {
            if (!m_session.matches_command(context)) { m_command_pending = false; publish(); return; }
            auto plan = value.at("plan");
            const bool accepted = confirm && confirm(plan);
            if (!accepted || !m_session.matches_command(context)) {
                m_command_pending = false; publish(); return;
            }
            auto draft = m_session.draft().design;
            if (draft.model.empty() && draft.provider == plan.at("draft").at("design_provider")) {
                draft.model = plan.at("draft").at("design_model").get<std::string>();
                m_session.set_design_draft(std::move(draft));
            }
            run_plan(group, std::move(plan), new_group);
        }), error());
}

void AdvancedCreationFeatureHost::run_plan(const std::string& group, Json plan, bool new_group)
{
    if (!save_pending_receipt({{"schema", "advanced-desktop-submission-v1"}, {"group_id", group},
        {"operation_id", plan.at("id")}, {"new_group", new_group}})) {
        m_command_pending = false; publish(); return;
    }
    const auto context = m_session.context();
    m_client.request("POST", "/groups/" + group + "/operations",
        {{"command_id", plan.at("id")}, {"plan_id", plan.at("id")},
         {"confirmation_fingerprint", plan.at("confirmation_fingerprint")}},
        complete([this, group, new_group, context](Json) {
            if (!m_session.matches(context)) return;
            m_command_pending = false;
            if (new_group) m_session.begin_open(group);
            const auto callback = m_session.context();
            m_poll_pending = true;
            m_client.request("GET", "/groups/" + group, {}, complete([this, callback, new_group](Json value) {
                if (!m_session.matches(callback)) return;
                ingest(std::move(value.at("group")), callback, new_group, [this] { clear_pending_receipt(); refresh_history(); });
            }, false, true), error(false, true));
            publish();
        }), error());
}

bool AdvancedCreationFeatureHost::save_pending_receipt(Json receipt)
{
    try {
        boost::filesystem::create_directories(m_cache);
        const auto partial = m_cache / "pending-submission.json.part";
        boost::filesystem::ofstream output(partial, std::ios::binary | std::ios::trunc);
        output << receipt.dump(); output.close();
        if (!output) throw std::runtime_error("write");
        const auto destination = m_cache / "pending-submission.json";
        if (boost::filesystem::exists(destination)) throw std::runtime_error("unresolved");
        boost::filesystem::rename(partial, destination);
        m_pending_receipt = std::move(receipt); return true;
    } catch (const std::exception&) { m_message = "无法保存生成请求，请检查用户模型目录的可用空间。"; return false; }
}

void AdvancedCreationFeatureHost::clear_pending_receipt()
{
    boost::system::error_code error;
    boost::filesystem::remove(m_cache / "pending-submission.json", error);
    if (error) { m_message = "原请求已恢复，但本地请求记录暂时无法归档。"; return; }
    m_pending_receipt = nullptr;
}

void AdvancedCreationFeatureHost::reconcile_submission()
{
    reconcile_submission(true);
}

void AdvancedCreationFeatureHost::reconcile_submission(bool announce_recovery)
{
    if (!has_pending_receipt() || !m_service_available || m_command_pending || m_poll_pending ||
        !m_pending_receipt.contains("group_id") || !m_pending_receipt.contains("operation_id")) return;
    const auto group = m_pending_receipt.at("group_id").get<std::string>();
    const auto operation = m_pending_receipt.at("operation_id").get<std::string>();
    m_command_pending = true;
    const auto collection = m_pending_receipt.value("kind", std::string("design")) == "model" ? "/model-jobs/" : "/operations/";
    m_client.request("POST", "/groups/" + group + collection + operation + "/reconcile", Json::object(),
        complete([this, group, announce_recovery](Json value) {
            m_command_pending = false;
            if (!value.at("accepted").get<bool>()) {
                clear_pending_receipt(); m_message = "原确认请求未受理，可以重新确认生成。"; publish(); return;
            }
            const auto callback = m_session.begin_open(group);
            ingest(std::move(value.at("group")), callback, true, [this, announce_recovery] {
                clear_pending_receipt();
                if (!has_pending_receipt()) {
                    m_message = announce_recovery ? "原请求已恢复，没有新建生成请求。" :
                        std::string();
                }
                refresh_history();
            });
        }), error());
    publish();
}

void AdvancedCreationFeatureHost::retry(ViewId view, Confirm confirm)
{
    if (!eligibility().retry || !m_session.group()) return;
    const auto& group = *m_session.group(); const auto& design = group.views[index(view)];
    if (design.operation_id.empty() || (design.attempt_state != "failed" && design.attempt_state != "stopped" &&
                                      design.attempt_state != "blocked")) return;
    m_command_pending = true;
    prepare(group.id, {{"command_id", new_identity()}, {"kind", "retry"},
            {"expected_group_revision", group.revision}, {"operation_id", design.operation_id},
            {"target_views", Json::array({view_name(view)})}}, confirm, m_session.command_context(), false);
    publish();
}

void AdvancedCreationFeatureHost::stop()
{
    if (!eligibility().stop || !m_session.group()) return;
    const auto& group = *m_session.group(); m_command_pending = true;
    const auto route = group.model_busy ? "/model-jobs/" + group.model_job_id : "/operations/" + group.active_operation_id;
    m_client.request("POST", "/groups/" + group.id + route + "/stop", Json::object(),
        complete([this](Json) { m_command_pending = false; poll(); publish(); }), error());
}

void AdvancedCreationFeatureHost::recover(ViewId view)
{
    if (!eligibility().recover || !m_session.group()) return;
    const auto& group = *m_session.group();
    const auto operation = group.views[index(view)].operation_id;
    if (operation.empty()) return;
    m_command_pending = true;
    m_client.request("POST", "/groups/" + group.id + "/operations/" + operation + "/recover", Json::object(),
        complete([this](Json) { m_command_pending = false; poll(); publish(); }), error());
}
}
