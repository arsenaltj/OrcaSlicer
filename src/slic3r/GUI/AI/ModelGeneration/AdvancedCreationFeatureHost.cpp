#include "AdvancedCreationFeatureHost.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelGenerationStatusText.hpp"
#include "slic3r/GUI/AISidecarClient.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/image.h>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <map>
#include <limits>
#include <algorithm>

namespace Slic3r::GUI {
using namespace AdvancedCreation;
namespace {
std::string new_identity() { return boost::uuids::to_string(boost::uuids::random_generator()()); }
constexpr const char* DefaultYoyoImageModel = "image-model-library/Qwen-Image-3.0-PC-YOYO";

AIModelGenerationClient::ProviderCatalog image_catalog_state(
    std::string state, std::string error_code = {}, std::string error_message = {},
    bool retryable = false, std::string connection_state = {})
{
    AIModelGenerationClient::ProviderCatalog catalog;
    catalog.role = "image";
    catalog.provider = "yoyoclaw";
    catalog.state = std::move(state);
    catalog.error_code = std::move(error_code);
    catalog.error_message = std::move(error_message);
    catalog.retryable = retryable;
    catalog.connection_state = std::move(connection_state);
    return catalog;
}
}

AdvancedCreationFeatureHost::AdvancedCreationFeatureHost(wxWindow* owner, std::string endpoint)
    : m_owner(owner), m_client(endpoint.empty() ? AISidecarClient::default_endpoint() : endpoint),
      m_catalog_client(endpoint.empty() ? AISidecarClient::default_endpoint() : endpoint),
      m_cache(ModelGenerationPresentation::generated_models_root() / "advanced-cache"),
      m_poll_timer(this), m_save_timer(this)
{
    m_model_monitor.set_listener([this](const auto& state) {
        if (!m_shutdown && m_session.matches(m_model_context) && m_model_state_listener)
            m_model_state_listener(state, false);
    });
    m_image_catalog = image_catalog_state("checking", {}, {}, false, "checking");
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        const auto& group = m_session.group();
        // Idle snapshots are already current. Polling them also briefly blocks
        // explicit commands while their cached assets are verified.
        if (group && (!group->active_operation_id.empty() || group->model_busy)) poll();
        std::vector<std::string> abandoned;
        for (const auto& [id, receipt] : m_pending_receipts)
            if (receipt.value("stop_intent", false)) abandoned.push_back(id);
        for (const auto& id : abandoned) settle_receipt(id, false);
    }, m_poll_timer.GetId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (!command_pending() && !m_poll_pending) save_draft({});
        else m_save_timer.StartOnce(600);
    }, m_save_timer.GetId());
    try {
        std::vector<boost::filesystem::path> receipts;
        const auto legacy = m_cache / "pending-submission.json";
        if (boost::filesystem::exists(legacy)) receipts.push_back(legacy);
        const auto folder = m_cache / "pending-submissions";
        if (boost::filesystem::is_directory(folder))
            for (const auto& entry : boost::filesystem::directory_iterator(folder))
                if (entry.path().extension() == ".json") receipts.push_back(entry.path());
        for (const auto& receipt : receipts) {
            if (!boost::filesystem::is_regular_file(receipt) || boost::filesystem::file_size(receipt) > 65536)
                throw std::runtime_error("invalid receipt");
            boost::filesystem::ifstream input(receipt);
            auto value = Json::parse(input, nullptr, false);
            if (value.is_object() && value.value("schema", std::string()) == "advanced-desktop-submission-v1" &&
                AdvancedMultiviewClient::valid_id(value.value("group_id", std::string())) &&
                AdvancedMultiviewClient::valid_id(value.value("operation_id", std::string()))) {
                value["_receipt_path"] = receipt.string();
                const auto operation = value.at("operation_id").get<std::string>();
                m_pending_receipts[operation] = std::move(value);
                m_message = "上次确认的请求等待核对，请恢复原请求。";
            } else {
                m_pending_receipts[receipt.string()] = Json::object();
                m_message = "生成请求记录损坏，请检查本地记录后恢复。";
            }
        }
    } catch (const std::exception&) { m_pending_receipts["unreadable"] = Json::object(); m_message = "生成请求记录暂时无法读取。"; }
}

AdvancedCreationFeatureHost::~AdvancedCreationFeatureHost() { shutdown(); }
void AdvancedCreationFeatureHost::shutdown()
{
    if (m_shutdown) return;
    m_shutdown = true;
    m_poll_timer.Stop(); m_save_timer.Stop(); m_listener = {}; m_model_state_listener = {};
    m_model_monitor.set_listener({}); m_model_monitor.reset(); m_catalog_client.cancel_current();
    m_cache_scope.cancel();
    m_model_job_listener = {};
    m_client.cancel();
}

AdvancedCreationFeatureHost::Complete AdvancedCreationFeatureHost::complete(std::function<void(Json)> callback, bool release_command, bool release_poll, bool session_bound)
{
    wxWeakRef<AdvancedCreationFeatureHost> weak(this);
    const auto context = m_session.context();
    return [weak, context, callback = std::move(callback), release_command, release_poll, session_bound](Json value) {
        Presentation::post_gui([weak, context, callback, release_command, release_poll, session_bound, value = std::move(value)]() mutable {
            if (!weak || weak->m_shutdown) return;
            if (session_bound && !weak->m_session.matches(context)) return;
            try { callback(std::move(value)); }
            catch (const Json::exception&) {
                if (!weak->m_session.matches(context)) return;
                if (release_command) weak->m_command_state = CommandState::Idle;
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
        Presentation::post_gui([weak, context, release_command, release_poll, message = std::move(message)] {
            if (!weak || weak->m_shutdown || !weak->m_session.matches(context)) return;
            weak->m_message = ModelGenerationStatusText::localized_advanced_service_error(message).ToUTF8().data();
            if (release_command) weak->m_command_state = CommandState::Idle;
            if (release_poll) weak->m_poll_pending = false;
            weak->publish();
        });
    };
}

void AdvancedCreationFeatureHost::set_listener(std::function<void()> listener)
{ m_listener = std::move(listener); publish(); }
void AdvancedCreationFeatureHost::get_provider_catalog(const std::string& role, const std::string& provider,
    AIModelGenerationClient::CatalogFn complete, AIModelGenerationClient::ErrorFn error)
{
    wxWeakRef<AdvancedCreationFeatureHost> weak(this);
    m_catalog_client.get_provider_catalog(role, provider, [weak, complete](auto catalog) {
        Presentation::post_gui([weak, complete, catalog = std::move(catalog)]() mutable {
            if (weak && !weak->m_shutdown && complete) complete(std::move(catalog));
        });
    }, [weak, error](auto message) {
        Presentation::post_gui([weak, error, message = std::move(message)]() mutable {
            if (weak && !weak->m_shutdown && error) error(std::move(message));
        });
    });
}
void AdvancedCreationFeatureHost::observe_model_job(const std::string& job, bool open_route, bool restart)
{
    m_model_context = m_session.context();
    boost::filesystem::path reference;
    if (m_session.group()) reference = m_session.group()->views[0].image_path;
    m_model_monitor.observe(job, reference, restart);
    if (open_route && m_model_state_listener) m_model_state_listener(m_model_monitor.snapshot(), true);
}
void AdvancedCreationFeatureHost::show_model()
{
    if (!m_session.group() || m_session.group()->model_job_id.empty()) return;
    observe_model_job(m_session.group()->model_job_id, true);
}

void AdvancedCreationFeatureHost::publish() { if (m_listener) m_listener(); }

void AdvancedCreationFeatureHost::set_service_available(bool available)
{
    const bool changed = available != m_service_available;
    m_service_available = available;
    m_model_monitor.set_available(available);
    if (available && changed) {
        refresh_capabilities(); refresh_image_catalog(); refresh_history(); m_poll_timer.Start(1200);
    } else if (!available) {
        ++m_image_catalog_sequence;
        m_image_catalog = image_catalog_state("checking", {}, {}, false, "checking");
        m_poll_timer.Stop();
    }
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

void AdvancedCreationFeatureHost::refresh_image_catalog()
{
    const auto sequence = ++m_image_catalog_sequence;
    m_image_catalog = image_catalog_state("checking", {}, {}, false, "checking");
    if (!m_service_available) { publish(); return; }
    wxWeakRef<AdvancedCreationFeatureHost> weak(this);
    get_provider_catalog("image", "yoyoclaw",
        [weak, sequence](ProviderCatalog catalog) mutable {
            if (!weak || weak->m_shutdown || sequence != weak->m_image_catalog_sequence) return;
            if (catalog.role != "image" || catalog.provider != "yoyoclaw") {
                catalog = image_catalog_state("error", "invalid_catalog",
                                              "YOYOClaw image catalog is invalid.", false, "error");
            }
            weak->m_image_catalog = std::move(catalog);
            auto draft = weak->m_session.draft().design;
            if (draft.provider == "yoyoclaw" && draft.model.empty() &&
                weak->m_image_catalog.default_model == DefaultYoyoImageModel &&
                weak->m_image_catalog.supports_model(DefaultYoyoImageModel)) {
                draft.model = DefaultYoyoImageModel;
                weak->m_session.set_design_draft(std::move(draft));
                weak->m_save_timer.StartOnce(650);
            }
            weak->refresh_capabilities();
            weak->publish();
        },
        [weak, sequence](std::string message) {
            if (!weak || weak->m_shutdown || sequence != weak->m_image_catalog_sequence) return;
            weak->m_image_catalog = image_catalog_state(
                "error", "catalog_unavailable", std::move(message), true, "error");
            weak->publish();
        });
}

Capabilities AdvancedCreationFeatureHost::capabilities() const
{
    Capabilities result;
    result.service_available = m_service_available && m_capabilities.is_object();
    if (!result.service_available) return result;
    if (m_session.draft().design.provider == "yoyoclaw" &&
        !m_image_catalog.supports_model(m_session.draft().design.model)) return result;
    if (m_capabilities.contains("image_models")) for (const auto& model : m_capabilities["image_models"]) {
        const auto provider = model.value("provider", std::string());
        if (provider != m_session.draft().design.provider) continue;
        if ((provider == "yoyoclaw" || !m_session.draft().design.model.empty()) &&
            model.value("model", std::string()) != m_session.draft().design.model) continue;
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
    guards.can_start = !command_pending() && !m_poll_pending && !has_pending_receipt() && (!m_start_guard || m_start_guard());
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

void AdvancedCreationFeatureHost::ingest(Json group, CallbackContext context, bool restore, std::function<void()> done,
                                        std::optional<InputDraft> saved_input)
{
    auto parsed = AdvancedMultiviewClient::parse_group(group, m_cache);
    if (!m_session.matches(context)) return;
    if (!parsed) { m_poll_pending = false; m_command_state = CommandState::Idle;
        m_message = "高级服务返回的数据不完整，当前输入与结果已保留。"; publish(); return; }
    struct Asset { std::string id, hash; };
    struct Verified { GroupSnapshot snapshot; std::vector<Asset> missing; };
    std::vector<Asset> assets;
    for (auto view : AllViews) {
        const auto name = view_name(view);
        const auto& upload = group.at("uploads").at(name);
        if (upload.is_object()) assets.push_back({upload.at("asset_id"), upload.at("sha256")});
        const auto& current = group.at("views").at(name).at("current");
        if (current.is_object()) assets.push_back({current.at("id"), current.at("sha256")});
    }
    const auto snapshot = *parsed;
    std::map<std::string,std::string> decoded;
    if (m_session.group() && m_session.group()->id == snapshot.id)
        for (const auto& visible : m_session.group()->views)
            if (visible.usable && !visible.display_error) decoded[visible.image_path] = visible.sha256;
    // Publish real task/revision state immediately. Unverified images are never
    // installed; Session keeps the last verified version while cache I/O runs.
    for (auto& design : parsed->views) {
        design.usable = false;
        design.asset_pending = !design.canonical_version_id.empty();
    }
    if (!m_session.apply_snapshot(std::move(*parsed), context, restore, saved_input ? &*saved_input : nullptr)) {
        m_poll_pending = false; m_command_state = CommandState::Idle; publish(); return;
    }
    if (m_session.group() && !m_session.group()->model_job_id.empty())
        observe_model_job(m_session.group()->model_job_id, false);
    m_poll_pending = true;
    publish();
    const auto group_id = snapshot.id;
    const auto task = m_cache_scope.begin(group_id, Presentation::TaskLifetime::Session);
    const auto cache = m_cache;
    wxWeakRef<AdvancedCreationFeatureHost> weak(this);
    auto verify = [snapshot, assets, cache, group_id, decoded](const Presentation::TaskContext& worker) {
        Verified result; result.snapshot = snapshot;
        std::map<std::pair<std::string,std::string>,bool> checked;
        const auto matches = [&](const boost::filesystem::path& path,const std::string& hash) {
            const auto key=std::make_pair(path.generic_string(),hash);
            const auto found=checked.find(key);
            if(found!=checked.end()) return found->second;
            return checked.emplace(key,AdvancedMultiviewClient::cached_asset_matches(path,hash)).first->second;
        };
        for (const auto& asset : assets) {
            if (worker.canceled()) return result;
            if (!matches(AdvancedMultiviewClient::asset_path(cache, group_id, asset.id), asset.hash))
                result.missing.push_back(asset);
        }
        for (auto& design : result.snapshot.views) {
            if (worker.canceled()) return result;
            if (design.canonical_version_id.empty()) continue;
            design.usable = matches(boost::filesystem::path(design.image_path), design.sha256);
            const auto prior=decoded.find(design.image_path);
            if (design.usable && (prior==decoded.end() || prior->second!=design.sha256)) {
                wxImage image(wxString(boost::filesystem::path(design.image_path).wstring()));
                design.usable = image.IsOk();
            }
            design.asset_pending = !design.usable;
        }
        return result;
    };
    auto finish = [weak, task, context, done](Presentation::TaskResult<Verified> outcome) {
        if (!weak || weak->m_shutdown || !weak->m_cache_scope.matches(task) || !weak->m_session.matches(context)) return;
        weak->m_poll_pending = false;
        bool cache_error = outcome.canceled || outcome.error || !outcome.value;
        if (!cache_error) {
            auto result = std::move(*outcome.value);
            cache_error = !result.missing.empty();
            for (auto& design : result.snapshot.views) {
                design.asset_pending = false;
                design.display_error = !design.usable && !design.canonical_version_id.empty();
            }
            weak->m_session.apply_snapshot(std::move(result.snapshot), context, false);
        } else if (weak->m_session.group()) {
            auto snapshot = *weak->m_session.group();
            for (auto& design : snapshot.views) {
                if (design.asset_pending) design.display_error = true;
                design.asset_pending = false;
            }
            weak->m_session.apply_snapshot(std::move(snapshot), context, false);
        }
        if (weak->m_session.group() && !weak->m_session.group()->model_job_id.empty())
            weak->observe_model_job(weak->m_session.group()->model_job_id, false);
        if (done) done();
        if (!weak || weak->m_shutdown || !weak->m_cache_scope.matches(task) || !weak->m_session.matches(context)) return;
        if (cache_error) weak->m_message = "高级图片缓存暂时无法读取，当前输入与结果已保留。";
        weak->publish();
    };
    const bool accepted = Presentation::background_tasks().submit(task, verify,
        [weak, task, context, group_id, verify, finish](Presentation::TaskResult<Verified> outcome) {
            if (!weak || weak->m_shutdown || !weak->m_cache_scope.matches(task) || !weak->m_session.matches(context)) return;
            if (!outcome.value || outcome.error || outcome.value->missing.empty()) { finish(std::move(outcome)); return; }
            const auto missing = outcome.value->missing;
            weak->m_session.apply_snapshot(std::move(outcome.value->snapshot), context, false);
            weak->publish();
            auto count = std::make_shared<std::size_t>(missing.size());
            auto downloaded = [weak, task, context, count, verify, finish](std::string error) {
                if (!weak || weak->m_shutdown || !weak->m_cache_scope.matches(task) || !weak->m_session.matches(context)) return;
                if (!error.empty()) weak->m_message = ModelGenerationStatusText::localized_advanced_service_error(error).ToUTF8().data();
                if (--*count != 0) return;
                if (!Presentation::background_tasks().submit(task, verify, finish)) finish({});
            };
            for (const auto& asset : missing) {
                const auto path = AdvancedMultiviewClient::asset_path(weak->m_cache, group_id, asset.id);
                weak->m_client.download_asset(group_id, asset.id, asset.hash, path,
                    [downloaded] { Presentation::post_gui([downloaded] { downloaded({}); }); },
                    [downloaded](std::string error) { Presentation::post_gui([downloaded, error = std::move(error)] { downloaded(error); }); });
            }
        });
    if (!accepted) finish({});
}

void AdvancedCreationFeatureHost::poll(bool restore)
{
    if (!m_service_available || m_shutdown || m_poll_pending || command_pending()) return;
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
    m_command_state = done ? CommandState::Exclusive : CommandState::BackgroundDraftSave;
    if (input.uploads != m_session.group()->uploads) {
        m_client.update_group(context.group_id, m_session.group()->revision, input,
            complete([this, context, input, draft, done](Json value) {
                if (!m_session.matches(context)) return;
                ingest(std::move(value.at("group")), context, false, [this, draft, done] {
                    m_session.mark_draft_saved(draft);
                    if (done) done(); else {
                        m_command_state = CommandState::Idle;
                        if (m_session.draft_dirty()) m_save_timer.StartOnce(650);
                        publish();
                    }
                }, input);
            }), error());
        return;
    }
    m_client.request("PUT", "/groups/" + context.group_id + "/draft",
        {{"expected_group_revision", m_session.group()->revision}, {"draft", AdvancedMultiviewClient::design_json(draft)}},
        complete([this, context, input, draft, done](Json value) {
            if (!m_session.matches(context)) return;
            ingest(std::move(value.at("group")), context, false, [this, draft, done] {
                m_session.mark_draft_saved(draft);
                if (done) done(); else {
                    m_command_state = CommandState::Idle;
                    if (m_session.draft_dirty()) m_save_timer.StartOnce(650);
                    publish();
                }
            }, input);
        }), error());
}

void AdvancedCreationFeatureHost::open_group(const std::string& id)
{
    if (command_pending() || m_poll_pending || has_pending_receipt() || (m_start_guard && !m_start_guard()) || !AdvancedMultiviewClient::valid_id(id)) return;
    m_command_state = CommandState::Exclusive;
    save_draft([this, id] {
        m_command_state = CommandState::Idle; m_model_monitor.reset(); m_session.begin_open(id); m_message.clear(); poll(true); publish();
    });
}

void AdvancedCreationFeatureHost::rename_group(const std::string& id, std::uint64_t revision, std::string display_name)
{
    if (command_pending() || m_poll_pending || has_pending_receipt() ||
        (m_start_guard && !m_start_guard()) || !AdvancedMultiviewClient::valid_id(id)) return;
    m_command_state = CommandState::Exclusive;
    publish();
    auto rename = [this, id, display_name = std::move(display_name)](std::uint64_t expected_revision) {
        const bool current = m_session.group() && m_session.group()->id == id;
        const auto context = m_session.context();
        m_client.request("PUT", "/groups/" + id + "/name",
            {{"expected_group_revision", expected_revision}, {"display_name", display_name}},
            complete([this, current, context](Json value) {
                if (!current) {
                    m_command_state = CommandState::Idle;
                    refresh_history();
                    publish();
                    return;
                }
                ingest(std::move(value.at("group")), context, false, [this] {
                    m_command_state = CommandState::Idle;
                    refresh_history();
                });
            }), error());
    };
    if (m_session.group() && m_session.group()->id == id) {
        save_draft([this, rename = std::move(rename)] {
            if (!m_session.group()) { m_command_state = CommandState::Idle; publish(); return; }
            rename(m_session.group()->revision);
        });
    } else {
        rename(revision);
    }
}

void AdvancedCreationFeatureHost::new_creation()
{
    std::map<std::string, Json> old;
    for (const auto& [id, receipt] : m_pending_receipts)
        if (receipt.contains("group_id") && receipt_is_current(receipt)) old[id] = receipt;
    if (m_session.group()) {
        const auto& group = *m_session.group();
        const auto operation = group.model_busy ? group.model_job_id : group.active_operation_id;
        if (!operation.empty()) old[operation] = {{"schema", "advanced-desktop-submission-v1"},
            {"group_id", group.id}, {"operation_id", operation}, {"kind", group.model_busy ? "model" : "design"}};
    }
    m_save_timer.Stop();
    m_cache_scope.cancel();
    m_model_monitor.reset();
    m_session.new_creation();
    m_restore_receipts = false;
    m_command_state = CommandState::Idle; m_poll_pending = false; m_message.clear();
    publish();
    // Reset is synchronous. Only bookkeeping/stop runs asynchronously afterwards.
    for (auto& [id, receipt] : old) {
        receipt["stop_intent"] = true;
        save_pending_receipt(receipt);
        m_reconciling.erase(id); // A stop must not wait for an earlier read/reconcile.
        settle_receipt(id, false);
    }
    refresh_capabilities();
}

void AdvancedCreationFeatureHost::generate_design(Confirm confirm)
{
    m_session.flush_click(std::numeric_limits<std::uint64_t>::max());
    if (!eligibility().generate_design) return;
    m_command_state = CommandState::Exclusive; m_message.clear(); m_save_timer.Stop();
    if (!m_session.group()) {
        const auto context = m_session.command_context();
        const auto id = new_identity();
        m_client.create_group(id, m_session.draft(), complete([this, context, id, confirm](Json value) {
            if (!m_session.matches_command(context)) {
                if (m_session.matches(context.callback)) { m_command_state = CommandState::Idle; publish(); }
                return;
            }
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
        for (auto view : m_session.effective_views()) request["target_views"].push_back(view_name(view));
        for (auto view : AllViews) {
            const auto& visible = group.views[index(view)];
            request["base_versions"][view_name(view)] = visible.usable ? Json(visible.version_id) : Json(nullptr);
        }
        prepare(group.id, std::move(request), confirm, m_session.command_context(), false);
    });
    publish();
}

void AdvancedCreationFeatureHost::generate_model(Confirm confirm)
{
    m_session.flush_click(std::numeric_limits<std::uint64_t>::max());
    if (!eligibility().generate_model || !m_session.group()) return;
    m_command_state = CommandState::Exclusive; m_message.clear(); m_save_timer.Stop();
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
                if (!m_session.matches_command(context, false)) { m_command_state = CommandState::Idle; publish(); return; }
                const auto& plan = value.at("plan");
                if (!confirm || !confirm(plan) || !m_session.matches_command(context, false)) {
                    if (m_session.matches(context.callback)) { m_command_state = CommandState::Idle; publish(); }
                    return;
                }
                if (!save_pending_receipt({{"schema", "advanced-desktop-submission-v1"}, {"kind", "model"},
                    {"group_id", group}, {"operation_id", plan.at("job_id")}, {"new_group", false}})) {
                    m_command_state = CommandState::Idle; publish(); return;
                }
                const auto request = begin_model_request(plan.at("job_id").get<std::string>());
                m_client.request("POST", "/groups/" + group + "/model-jobs",
                    {{"command_id", plan.at("id")}, {"plan_id", plan.at("id")},
                     {"confirmation_fingerprint", plan.at("confirmation_fingerprint")}},
                    complete([this, request](Json response) {
                        m_command_state = CommandState::Idle;
                        if (!m_session.matches(request.context)) return;
                        const auto receipt = m_pending_receipts.find(request.job_id);
                        if (receipt == m_pending_receipts.end() ||
                            receipt->second.value("group_id", std::string()) != request.context.group_id ||
                            !accept_model_job(response.at("job"), request)) {
                            m_message = "任务受理身份暂时无法核对，请核对原请求，不要重复提交。";
                            publish(); return;
                        }
                        reconcile_submission(false);
                    }), error());
            }), error());
    });
    publish();
}

ModelJobEvent AdvancedCreationFeatureHost::begin_model_request(const std::string& job)
{
    ModelJobEvent request {m_session.context(), job, ++m_model_request_sequence};
    m_model_request = request;
    if (m_model_job_listener) m_model_job_listener(request);
    return request;
}

bool AdvancedCreationFeatureHost::accept_model_job(const Json& job, const ModelJobEvent& request)
{
    const auto projection = AdvancedMultiviewClient::parse_model_job(job, request.context.group_id, request.job_id);
    if (!projection || !m_model_request || !m_model_request->same_request(request) ||
        !m_session.apply_model_job(*projection, request.context)) return false;
    // Publish the real identity before the adapter/Shell can inspect the Session.
    auto accepted = request; accepted.phase = ModelJobEvent::Phase::Accepted;
    m_model_request = accepted;
    if (m_model_job_listener) m_model_job_listener(accepted);
    const auto& observed = m_model_monitor.snapshot();
    const bool resumed = observed.job_id == accepted.job_id &&
        (observed.stage == ModelGenerationUIStage::Failed || observed.stage == ModelGenerationUIStage::Stopped) &&
        (projection->state == "queued" || projection->state == "running" || projection->state == "stopping");
    observe_model_job(accepted.job_id, false, resumed);
    publish();
    return true;
}

void AdvancedCreationFeatureHost::resume_model()
{
    if (command_pending() || has_pending_receipt() || !m_session.group()) return;
    const auto& group = *m_session.group();
    if (group.model_busy || !group.active_operation_id.empty() || !group.model_resumable ||
        (group.model_state != "stopped" && group.model_state != "failed")) return;
    const auto request = begin_model_request(group.model_job_id);
    m_command_state = CommandState::Exclusive;
    m_client.request("POST", "/groups/" + group.id + "/model-jobs/" + group.model_job_id + "/resume", Json::object(),
        complete([this, request](Json value) {
            m_command_state = CommandState::Idle;
            if (!m_session.matches(request.context)) return;
            if (!accept_model_job(value.at("job"), request)) {
                m_message = "恢复任务的身份暂时无法核对，当前任务和输入已保留。";
            }
            poll();
            publish();
        }), error());
    publish();
}

void AdvancedCreationFeatureHost::prepare(const std::string& group, Json request, Confirm confirm,
                                        CommandContext context, bool new_group)
{
    m_client.request("POST", "/groups/" + group + "/plans", request,
        complete([this, group, confirm, context, new_group](Json value) {
            if (!m_session.matches_command(context)) { m_command_state = CommandState::Idle; publish(); return; }
            auto plan = value.at("plan");
            const bool accepted = confirm && confirm(plan);
            if (!accepted || !m_session.matches_command(context)) {
                if (m_session.matches(context.callback)) { m_command_state = CommandState::Idle; publish(); }
                return;
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
        m_command_state = CommandState::Idle; publish(); return;
    }
    const auto context = m_session.context();
    const auto operation = plan.at("id").get<std::string>();
    m_client.request("POST", "/groups/" + group + "/operations",
        {{"command_id", plan.at("id")}, {"plan_id", plan.at("id")},
         {"confirmation_fingerprint", plan.at("confirmation_fingerprint")}},
        complete([this, group, new_group, context, operation](Json) {
            if (!m_session.matches(context)) return;
            m_command_state = CommandState::Idle;
            if (new_group) m_session.begin_open(group);
            const auto callback = m_session.context();
            m_poll_pending = true;
            m_client.request("GET", "/groups/" + group, {}, complete([this, callback, new_group, operation](Json value) {
                if (!m_session.matches(callback)) return;
                ingest(std::move(value.at("group")), callback, new_group, [this, operation] { clear_pending_receipt(operation); refresh_history(); });
            }, false, true), error(false, true));
            publish();
        }), error());
}

bool AdvancedCreationFeatureHost::save_pending_receipt(Json receipt)
{
    const auto operation = receipt.value("operation_id", std::string());
    if (!AdvancedMultiviewClient::valid_id(operation)) return false;
    if (const auto found = m_pending_receipts.find(operation); found != m_pending_receipts.end()) {
        if (found->second.contains("_receipt_path")) receipt["_receipt_path"] = found->second.at("_receipt_path");
    }
    receipt["_session_epoch"] = m_session.context().epoch;
    // Keep even an unwritable stop intent in memory for this process to retry.
    m_pending_receipts[operation] = receipt;
    try {
        const auto folder = m_cache / "pending-submissions";
        boost::filesystem::create_directories(folder);
        const auto destination = receipt.contains("_receipt_path") ? boost::filesystem::path(receipt.at("_receipt_path").get<std::string>()) :
            folder / (operation + ".json");
        const auto partial = boost::filesystem::path(destination.string() + ".part");
        boost::filesystem::ofstream output(partial, std::ios::binary | std::ios::trunc);
        auto durable = receipt; durable.erase("_receipt_path"); durable.erase("_session_epoch");
        output << durable.dump(); output.close();
        if (!output) throw std::runtime_error("write");
        if (Slic3r::rename_file(partial.string(), destination.string())) throw std::runtime_error("rename");
        m_pending_receipts[operation]["_receipt_path"] = destination.string();
        return true;
    } catch (const std::exception&) { m_message = "无法保存生成请求，请检查用户模型目录的可用空间。"; return false; }
}

bool AdvancedCreationFeatureHost::receipt_is_current(const Json& receipt) const
{
    if (receipt.value("stop_intent", false)) return false;
    const auto context = m_session.context();
    if (!context.group_id.empty()) return receipt.value("group_id", std::string()) == context.group_id;
    return m_restore_receipts || receipt.value("_session_epoch", std::uint64_t(0)) == context.epoch;
}

bool AdvancedCreationFeatureHost::has_pending_receipt() const
{
    return std::any_of(m_pending_receipts.begin(), m_pending_receipts.end(),
        [this](const auto& item) { return receipt_is_current(item.second); });
}

void AdvancedCreationFeatureHost::clear_pending_receipt(const std::string& operation)
{
    const auto found = m_pending_receipts.find(operation);
    if (found == m_pending_receipts.end()) return;
    boost::system::error_code error;
    const auto path = found->second.value("_receipt_path", std::string());
    if (!path.empty()) boost::filesystem::remove(boost::filesystem::path(path), error);
    if (error) {
        if (receipt_is_current(found->second)) m_message = "原请求已恢复，但本地请求记录暂时无法归档。";
        return;
    }
    m_pending_receipts.erase(found);
}

void AdvancedCreationFeatureHost::reconcile_submission()
{
    reconcile_submission(true);
}

void AdvancedCreationFeatureHost::reconcile_submission(bool announce_recovery)
{
    if (!m_service_available || command_pending() || m_poll_pending) return;
    for (const auto& [id, receipt] : m_pending_receipts) if (receipt_is_current(receipt)) {
        settle_receipt(id, announce_recovery); return;
    }
}

void AdvancedCreationFeatureHost::settle_receipt(const std::string& operation, bool announce_recovery)
{
    const auto found = m_pending_receipts.find(operation);
    if (!m_service_available || found == m_pending_receipts.end() || m_reconciling.count(operation)) return;
    const auto receipt = found->second;
    if (!receipt.contains("group_id")) return;
    const auto group = receipt.at("group_id").get<std::string>();
    const bool abandoned = receipt.value("stop_intent", false);
    const bool current = receipt_is_current(receipt);
    const auto context = m_session.context();
    if (current) m_command_state = CommandState::Exclusive;
    m_reconciling.insert(operation);
    const auto collection = receipt.value("kind", std::string("design")) == "model" ? "/model-jobs/" : "/operations/";
    wxWeakRef<AdvancedCreationFeatureHost> weak(this);
    auto failed = [weak, operation, context, current](std::string message) {
        Presentation::post_gui([weak, operation, context, current, message = std::move(message)] {
            if (!weak || weak->m_shutdown) return;
            weak->m_reconciling.erase(operation);
            if (current && weak->m_session.matches(context)) {
                weak->m_command_state = CommandState::Idle;
                weak->m_message = ModelGenerationStatusText::localized_advanced_service_error(message).ToUTF8().data();
                weak->publish();
            }
        });
    };
    if (abandoned && receipt.value("kind", std::string("design")) != "model") {
        m_client.request("POST", "/groups/" + group + collection + operation + "/stop", Json::object(),
            complete([this, operation](Json) {
                m_reconciling.erase(operation); clear_pending_receipt(operation);
            }, false, false, false), failed);
        return;
    }
    m_client.request("POST", "/groups/" + group + collection + operation + "/reconcile", Json::object(),
        complete([this, group, operation, collection, context, current, abandoned, announce_recovery, failed, receipt](Json value) {
            m_reconciling.erase(operation);
            const auto live = m_pending_receipts.find(operation);
            if (!abandoned && live != m_pending_receipts.end() && live->second.value("stop_intent", false)) {
                settle_receipt(operation, false); return;
            }
            if (abandoned && value.at("accepted").get<bool>()) {
                m_client.request("POST", "/groups/" + group + collection + operation + "/stop", Json::object(),
                    complete([this, operation](Json) { clear_pending_receipt(operation); }, false, false, false), failed);
                return;
            }
            if (!current || !m_session.matches(context)) { clear_pending_receipt(operation); return; }
            m_command_state = CommandState::Idle;
            if (!m_session.matches(context)) return;
            if (!value.at("accepted").get<bool>()) {
                clear_pending_receipt(operation); m_message = "原确认请求未受理，可以重新确认生成。"; publish(); return;
            }
            const bool model_request = receipt.value("kind", std::string("design")) == "model";
            const bool restore = !m_session.group() || m_session.group()->id != group;
            auto callback = context;
            if (restore) callback = m_session.begin_open(group);
            // Restart recovery has no installed group yet. Register the real
            // group/task before acceptance; image validation remains asynchronous.
            if (restore && model_request) {
                auto snapshot = AdvancedMultiviewClient::parse_group(value.at("group"), m_cache);
                if (snapshot) for (auto& design : snapshot->views) {
                    design.usable = false;
                    design.asset_pending = !design.canonical_version_id.empty();
                }
                if (!snapshot || !m_session.apply_snapshot(std::move(*snapshot), callback, true)) {
                    m_message = "原任务所属套图暂时无法核对，当前记录已保留。"; publish(); return;
                }
            }
            if (model_request) {
                if (announce_recovery || !m_model_request || m_model_request->job_id != operation ||
                    (m_model_request->context.epoch != callback.epoch || m_model_request->context.group_id != callback.group_id))
                    begin_model_request(operation);
                if (!m_model_request || !accept_model_job(value.at("job"), *m_model_request)) {
                    m_message = "原任务身份与请求记录不一致，请保留记录后重试核对。"; publish(); return;
                }
            }
            // Reconcile the current group in place. Image synchronization must
            // not reset its selection or delay the accepted task projection.
            ingest(std::move(value.at("group")), callback, restore && !model_request, [this, operation, announce_recovery] {
                clear_pending_receipt(operation);
                if (!has_pending_receipt()) m_message = announce_recovery
                    ? "原请求已恢复，没有新建生成请求。" : std::string();
                refresh_history();
            });
        }, false, false, false), failed);
    if (current) publish();
}

void AdvancedCreationFeatureHost::retry(ViewId view, Confirm confirm)
{
    if (!eligibility().retry || !m_session.group()) return;
    const auto& group = *m_session.group(); const auto& design = group.views[index(view)];
    if (design.operation_id.empty() || (design.attempt_state != "failed" && design.attempt_state != "stopped" &&
                                      design.attempt_state != "blocked")) return;
    m_command_state = CommandState::Exclusive;
    prepare(group.id, {{"command_id", new_identity()}, {"kind", "retry"},
            {"expected_group_revision", group.revision}, {"operation_id", design.operation_id},
            {"target_views", Json::array({view_name(view)})}}, confirm, m_session.command_context(), false);
    publish();
}

void AdvancedCreationFeatureHost::stop()
{
    if (m_model_monitor.snapshot().stage == ModelGenerationUIStage::LoadingModel &&
        m_session.matches(m_model_context)) {
        m_model_monitor.stop_local(); return;
    }
    if (!eligibility().stop || !m_session.group()) return;
    const auto& group = *m_session.group(); m_command_state = CommandState::Exclusive;
    const auto route = group.model_busy ? "/model-jobs/" + group.model_job_id : "/operations/" + group.active_operation_id;
    m_client.request("POST", "/groups/" + group.id + route + "/stop", Json::object(),
        complete([this](Json) { m_command_state = CommandState::Idle; poll(); publish(); }), error());
}

void AdvancedCreationFeatureHost::recover(ViewId view)
{
    if (!eligibility().recover || !m_session.group()) return;
    const auto& group = *m_session.group();
    const auto operation = group.views[index(view)].operation_id;
    if (operation.empty()) return;
    m_command_state = CommandState::Exclusive;
    m_client.request("POST", "/groups/" + group.id + "/operations/" + operation + "/recover", Json::object(),
        complete([this](Json) { m_command_state = CommandState::Idle; poll(); publish(); }), error());
}
}
