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
#include <chrono>

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
        if (!command_pending() && !m_poll_pending && !m_session.model_candidate()) save_draft({});
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
                if (value.value("kind", std::string()) == "model") value["stop_intent"] = true;
                value["_receipt_path"] = receipt.string();
                const auto operation = value.at("operation_id").get<std::string>();
                const bool model_receipt = value.value("kind", std::string()) == "model";
                m_pending_receipts[operation] = std::move(value);
                m_message = model_receipt
                    ? "上次模型生成已结束，正在核对终止收尾。" : "上次确认的图片请求等待核对。";
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
    save_local_draft();
    m_shutdown = true;
    m_lifetime.reset();
    m_poll_timer.Stop(); m_save_timer.Stop(); m_listener = {}; m_model_state_listener = {};
    m_model_monitor.set_listener({}); m_model_monitor.reset(); m_catalog_client.cancel_current();
    m_cache_scope.cancel();
    m_model_job_listener = {};
    m_confirmed_model_visual.reset();
    m_client.cancel();
}

AdvancedCreationFeatureHost::Complete AdvancedCreationFeatureHost::complete(std::function<void(Json)> callback, bool release_command, bool release_poll, bool session_bound)
{
    auto* weak = this;
    const auto alive = std::weak_ptr<int>(m_lifetime);
    const auto context = m_session.context();
    return [weak, alive, context, callback = std::move(callback), release_command, release_poll, session_bound](Json value) {
        Presentation::post_gui([weak, alive, context, callback, release_command, release_poll, session_bound, value = std::move(value)]() mutable {
            if (alive.expired() || weak->m_shutdown) return;
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
    auto* weak = this;
    const auto alive = std::weak_ptr<int>(m_lifetime);
    const auto context = m_session.context();
    return [weak, alive, context, release_command, release_poll](std::string message) {
        Presentation::post_gui([weak, alive, context, release_command, release_poll, message = std::move(message)] {
            if (alive.expired() || weak->m_shutdown || !weak->m_session.matches(context)) return;
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
    auto* weak = this;
    const auto alive = std::weak_ptr<int>(m_lifetime);
    const auto key = role + ":" + provider;
    const auto sequence = ++m_feedback_catalog_sequences[key];
    m_feedback_catalogs[key] = {role, provider, "checking", "", {}, "", "", false};
    m_catalog_client.get_provider_catalog(role, provider, [weak, alive, complete, key, sequence](auto catalog) {
        Presentation::post_gui([weak, alive, complete, key, sequence, catalog = std::move(catalog)]() mutable {
            if (alive.expired() || weak->m_shutdown || weak->m_feedback_catalog_sequences[key] != sequence) return;
            weak->m_feedback_catalogs[key] = catalog;
            if (complete) complete(std::move(catalog));
        });
    }, [weak, alive, error, key, sequence, role, provider](auto message) {
        Presentation::post_gui([weak, alive, error, key, sequence, role, provider, message = std::move(message)]() mutable {
            if (alive.expired() || weak->m_shutdown || weak->m_feedback_catalog_sequences[key] != sequence) return;
            weak->m_feedback_catalogs[key] = {role, provider, "error", "", {}, "", "", false};
            if (error) error(std::move(message));
        });
    });
}
void AdvancedCreationFeatureHost::observe_model_job(const std::string& job, bool open_route, bool restart)
{
    const auto& group = m_session.group();
    const bool live = m_model_request && m_model_request->job_id == job && m_session.matches(m_model_request->context);
    if (!live && (!group || group->model_state != "ready")) return;
    if (m_stopping_model_job == job && m_model_monitor.snapshot().stage == ModelGenerationUIStage::Stopped) {
        // Explicitly viewing a committed asset starts only its local loading.
        // Group polling must never reopen a stopped generation by itself.
        if (!open_route || !group || group->model_job_id != job || group->model_state != "ready") return;
        restart = true;
        m_stopping_model_job.clear();
    }
    m_model_context = m_session.context();
    boost::filesystem::path reference;
    ModelGenerationVisualReference visual;
    if (live && m_model_request->phase == ModelJobEvent::Phase::Accepted && m_confirmed_model_visual &&
        m_confirmed_model_visual->reference.owner_id == job && m_session.matches(m_confirmed_model_visual->context))
        visual = m_confirmed_model_visual->reference;
    // Keep the existing artifact/library reference behavior independent of the
    // display-only frozen input.
    if (m_session.group()) reference = m_session.group()->views[0].image_path;
    m_model_monitor.observe(job, reference, restart, std::move(visual));
    if (open_route && m_model_state_listener) m_model_state_listener(m_model_monitor.snapshot(), true);
}
void AdvancedCreationFeatureHost::publish() { save_local_draft(); if (m_listener) m_listener(); }

bool AdvancedCreationFeatureHost::save_local_draft(const LocalDraft* candidate)
{
    const auto& group = m_session.group();
    if (!group || group->id != m_session.context().group_id ||
        !AdvancedMultiviewClient::valid_id(group->id) || m_unreadable_local_drafts.count(group->id)) return false;
    const auto local = candidate ? *candidate : m_session.local_draft();
    const auto& options = local.input.geometry;
    const Json value = {{"schema", "advanced-local-draft-v1"}, {"group_id", group->id},
        {"draft", AdvancedMultiviewClient::design_json(local.input.design)}, {"uploads", local.input.uploads},
        {"replaced_uploads", local.input.replaced_uploads}, {"upload_revisions", local.input.upload_revisions},
        {"invalidated_uploads", local.invalidated_uploads}, {"invalidated_versions", local.invalidated_versions},
        {"model_selected", local.model_selected}, {"model_seen", local.model_seen}, {"dirty", local.dirty},
        {"geometry", {{"provider", options.provider}, {"model", options.model}, {"face_limit", options.face_limit},
          {"geometry_quality", options.geometry_quality}, {"texture_quality", options.texture_quality}, {"output_format", options.output_format}}}};
    const auto previous = m_written_local_drafts.find(group->id);
    if (previous != m_written_local_drafts.end() && previous->second == value) return true;
    try {
        const auto folder = m_cache / "drafts";
        boost::filesystem::create_directories(folder);
        const auto path = folder / (group->id + ".json");
        const auto partial = boost::filesystem::path(path.string() + ".part");
        boost::filesystem::ofstream stream(partial, std::ios::binary | std::ios::trunc);
        stream << value.dump(); stream.close();
        if (!stream || Slic3r::rename_file(partial.string(), path.string())) throw std::runtime_error("write");
        m_written_local_drafts[group->id] = value;
        return true;
    } catch (const std::exception&) {
        m_message = "当前创作草稿暂时无法保存在本地，请检查用户模型目录的可用空间。";
        return false;
    }
}

void AdvancedCreationFeatureHost::restore_local_draft(const std::string& group)
{
    try {
        const auto path = m_cache / "drafts" / (group + ".json");
        if (!boost::filesystem::exists(path)) return;
        if (!boost::filesystem::is_regular_file(path) || boost::filesystem::file_size(path) > 65536)
            throw std::runtime_error("draft");
        boost::filesystem::ifstream stream(path, std::ios::binary);
        const auto value = Json::parse(stream);
        if (value.at("schema") != "advanced-local-draft-v1" || value.at("group_id") != group)
            throw std::runtime_error("identity");
        LocalDraft local;
        const auto& draft = value.at("draft");
        local.input.design = {draft.at("prompt"), draft.at("style"), draft.at("custom_style"),
                              draft.at("design_provider"), draft.at("design_model")};
        local.input.uploads = value.at("uploads").get<std::array<std::string,4>>();
        local.input.replaced_uploads = value.at("replaced_uploads").get<std::array<bool,4>>();
        local.input.upload_revisions = value.at("upload_revisions").get<std::array<std::uint64_t,4>>();
        local.invalidated_uploads = value.at("invalidated_uploads").get<std::array<bool,4>>();
        local.invalidated_versions = value.at("invalidated_versions").get<std::array<std::string,4>>();
        local.model_selected = value.at("model_selected").get<std::array<bool,4>>();
        local.model_seen = value.at("model_seen").get<std::array<bool,4>>();
        local.dirty = value.at("dirty").get<bool>();
        const auto& options = value.at("geometry");
        local.input.geometry = {options.at("provider"), options.at("model"), options.at("face_limit"),
                                options.at("geometry_quality"), options.at("texture_quality"), options.at("output_format")};
        m_session.restore_local_draft(local);
        m_written_local_drafts[group] = value;
        m_unreadable_local_drafts.erase(group);
        if (local.dirty) m_save_timer.StartOnce(650);
    } catch (const std::exception&) {
        m_unreadable_local_drafts.insert(group);
        m_message = "当前创作的本地草稿无法读取，已保留原记录，请检查后再生成。";
    }
}

void AdvancedCreationFeatureHost::set_service_available(bool available)
{
    const bool changed = available != m_service_available;
    m_service_available = available;
    m_model_monitor.set_available(available);
    if (available && changed) {
        refresh_capabilities(); refresh_image_catalog(); refresh_history(); m_poll_timer.Start(1200);
    } else if (!available) {
        ++m_image_catalog_sequence;
        ++m_image_capability_sequence;
        ++m_geometry_capability_sequence;
        m_image_capabilities = {};
        m_geometry_capabilities = {};
        m_image_catalog = image_catalog_state("checking", {}, {}, false, "checking");
        m_poll_timer.Stop();
    }
    publish();
}

void AdvancedCreationFeatureHost::refresh_capabilities(const std::string& scope)
{
    if (!m_service_available) return;
    const auto request = [this](bool geometry) {
        auto& sequence = geometry ? m_geometry_capability_sequence : m_image_capability_sequence;
        const auto id = ++sequence;
        auto fail = [this, id, geometry](std::string message) {
            if (!m_service_available || id != (geometry ? m_geometry_capability_sequence : m_image_capability_sequence)) return;
            (geometry ? m_geometry_capabilities : m_image_capabilities) = {};
            m_message = std::move(message);
            publish();
        };
        const auto alive = std::weak_ptr<int>(m_lifetime);
        const auto path = geometry ? std::string("/capabilities?scope=geometry&provider=") +
            (m_session.draft().geometry.provider == "yoyoclaw" ? "yoyoclaw" : "tripo") : "/capabilities?scope=image";
        m_client.request("GET", path, {},
            complete([this, id, geometry, fail](Json value) {
                if (!m_service_available || id != (geometry ? m_geometry_capability_sequence : m_image_capability_sequence)) return;
                bool valid = value.contains("schema") && value["schema"] == "advanced-multiview-capabilities-v1";
                if (geometry) {
                    valid = valid && value.contains("model_plans") && value["model_plans"].is_boolean() &&
                        value.contains("model_providers") && value["model_providers"].is_object();
                    if (valid) for (const auto& item : value["model_providers"])
                        valid = valid && item.is_object() && item.contains("available") && item["available"].is_boolean() &&
                            item.contains("named_four_view_model") && item["named_four_view_model"].is_string();
                } else {
                    valid = valid && value.contains("image_models") && value["image_models"].is_array();
                    if (valid) for (const auto& item : value["image_models"]) {
                        valid = valid && item.is_object();
                        for (const auto* key : {"provider", "model", "initial_text", "initial_named_references", "edit_current_with_context"})
                            valid = valid && item.contains(key) && item[key].is_string();
                    }
                }
                if (!valid) { fail("高级模式需要匹配的本地服务版本。"); return; }
                (geometry ? m_geometry_capabilities : m_image_capabilities) = std::move(value);
                publish();
            }, false, false, false), [alive, fail](std::string message) {
                Presentation::post_gui([alive, fail, message = std::move(message)]() mutable {
                    if (!alive.expired())
                        fail(ModelGenerationStatusText::localized_advanced_service_error(message).ToUTF8().data());
                });
            });
    };
    if (scope != "image") request(true);
    if (scope != "geometry") request(false);
}

void AdvancedCreationFeatureHost::refresh_image_catalog()
{
    const auto sequence = ++m_image_catalog_sequence;
    m_image_catalog = image_catalog_state("checking", {}, {}, false, "checking");
    if (!m_service_available) { publish(); return; }
    auto* weak = this;
    const auto alive = std::weak_ptr<int>(m_lifetime);
    get_provider_catalog("image", "yoyoclaw",
        [weak, alive, sequence](ProviderCatalog catalog) mutable {
            if (alive.expired() || weak->m_shutdown || sequence != weak->m_image_catalog_sequence) return;
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
            weak->refresh_capabilities("image");
            weak->publish();
        },
        [weak, alive, sequence](std::string message) {
            if (alive.expired() || weak->m_shutdown || sequence != weak->m_image_catalog_sequence) return;
            weak->m_image_catalog = image_catalog_state(
                "error", "catalog_unavailable", std::move(message), true, "error");
            weak->publish();
        });
}

Capabilities AdvancedCreationFeatureHost::capabilities() const
{
    Capabilities result;
    result.service_available = m_service_available;
    if (!result.service_available) return result;
    const bool image_selected = m_session.draft().design.provider != "yoyoclaw" ||
        m_image_catalog.supports_model(m_session.draft().design.model);
    if (image_selected && m_image_capabilities.contains("image_models")) for (const auto& model : m_image_capabilities["image_models"]) {
        const auto provider = model.value("provider", std::string());
        if (provider != m_session.draft().design.provider) continue;
        if ((provider == "yoyoclaw" || !m_session.draft().design.model.empty()) &&
            model.value("model", std::string()) != m_session.draft().design.model) continue;
        result.initial_text = model.value("initial_text", std::string()) == "supported";
        result.initial_references = model.value("initial_named_references", std::string()) == "supported";
        result.edit_current = model.value("edit_current_with_context", std::string()) == "supported";
    }
    if (m_geometry_capabilities.is_object() && m_geometry_capabilities.value("model_plans", false)) {
        const auto provider = m_geometry_capabilities["model_providers"].find(m_session.draft().geometry.provider);
        if (provider != m_geometry_capabilities["model_providers"].end()) {
            result.four_view_model = provider->value("named_four_view_model", std::string()) == "supported" &&
                                     provider->value("available", false);
            if (provider->value("available", false) && provider->contains("supported_image_counts") &&
                (*provider)["supported_image_counts"].is_array()) {
                result.model_image_counts_known = true;
                for (const auto& count : (*provider)["supported_image_counts"])
                    if (count.is_number_integer() && count.get<int>() >= 1 && count.get<int>() <= 4)
                        result.model_image_counts[count.get<int>() - 1] = true;
            }
        }
    }
    return result;
}

HostGuards AdvancedCreationFeatureHost::command_guards(bool preparing_candidate) const
{
    HostGuards guards;
    guards.can_start = !m_shutdown && (!command_pending() || (preparing_candidate && m_preparing_candidate)) &&
        !model_stop_pending() && !m_poll_pending && !has_pending_receipt() &&
        !m_unreadable_local_drafts.count(m_session.context().group_id) &&
        !m_model_monitor.snapshot().busy && (!m_start_guard || m_start_guard()) &&
        (!m_generation_guard || m_generation_guard());
    guards.model_service_available = m_service_available;
    if (m_editing_guard && !m_editing_guard()) guards.can_start = false;
    return guards;
}

Eligibility AdvancedCreationFeatureHost::eligibility() const
{
    if (m_shutdown) return {};
    const auto guards = command_guards();
    auto result = m_session.eligibility(capabilities(), guards);
    if (m_editing_guard && !m_editing_guard()) {
        result.click_generate_design = result.click_generate_model = false;
        result.generate_design = result.generate_model = result.retry = result.recover = false;
        result.design_reason = result.model_reason = "creation_read_only";
    }
    if (m_model_request && m_model_request->phase == ModelJobEvent::Phase::Requested && m_stopping_model_job.empty())
        result.stop = true;
    if (model_stop_pending()) {
        result.click_generate_design = result.click_generate_model = false;
        result.generate_design = result.generate_model = false;
        result.stop = !command_pending();
    }
    return result;
}

std::string AdvancedCreationFeatureHost::generation_rejection(bool model) const
{
    const auto allowed = eligibility();
    if (model ? allowed.generate_model : allowed.generate_design) return {};
    auto reason = model ? allowed.model_reason : allowed.design_reason;
    if (reason == "invalid_input") return m_session.input_rejection();
    if (reason != "unsupported_multiview" && reason != "service_unavailable") return reason;
    if (!m_service_available) return "service_unavailable";
    const auto& draft = m_session.draft();
    const auto& provider = model ? draft.geometry.provider : draft.design.provider;
    const auto& selected = model ? draft.geometry.model : draft.design.model;
    if (!model && provider == "yoyoclaw") {
        if (auto detail = generation_catalog_rejection(m_image_catalog, selected); !detail.empty()) return detail;
    }
    const auto catalog = m_feedback_catalogs.find(std::string(model ? "geometry:" : "image:") + provider);
    if (catalog != m_feedback_catalogs.end()) {
        if (auto detail = generation_catalog_rejection(catalog->second, selected); !detail.empty()) return detail;
    }
    return reason == "service_unavailable" ? reason : model ? "geometry_unsupported" : "image_unsupported";
}

void AdvancedCreationFeatureHost::changed()
{
    if (m_editing_guard && !m_editing_guard()) return;
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
                                        std::optional<InputDraft> saved_input, AdvancedMultiviewClient::Error failed,
                                        std::string history_target)
{
    auto parsed = AdvancedMultiviewClient::parse_group(group, m_cache);
    if (!m_session.matches(context)) return;
    if (!parsed || (!history_target.empty() && !m_session.can_open_snapshot(*parsed, history_target))) { m_poll_pending = false; m_command_state = CommandState::Idle;
        m_message = "高级服务返回的数据不完整，当前输入与结果已保留。";
        if (failed) failed("invalid_response"); publish(); return; }
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
    if (!history_target.empty()) {
        // All target metadata has been parsed and admitted while the old
        // session is still intact. Commit its identity and draft together.
        m_stopping_model_job.clear();
        m_model_request.reset();
        m_confirmed_model_visual.reset();
        m_candidate_plan.reset();
        ++m_candidate_plan_sequence;
        m_preparing_candidate = false;
        m_model_monitor.reset();
        context = m_session.begin_open(history_target);
    }
    if (!m_session.apply_snapshot(std::move(*parsed), context, restore, saved_input ? &*saved_input : nullptr)) {
        m_poll_pending = false; m_command_state = CommandState::Idle;
        if (failed) failed("draft_changed"); publish(); return;
    }
    if (restore) restore_local_draft(context.group_id);
    if (!history_target.empty()) m_command_state = CommandState::Idle;
    if (m_session.group() && !m_session.group()->model_job_id.empty())
        observe_model_job(m_session.group()->model_job_id, false);
    m_poll_pending = true;
    publish();
    const auto group_id = snapshot.id;
    const auto task = m_cache_scope.begin(group_id, Presentation::TaskLifetime::Session);
    const auto cache = m_cache;
    auto* weak = this;
    const auto alive = std::weak_ptr<int>(m_lifetime);
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
    auto finish = [weak, alive, task, context, done](Presentation::TaskResult<Verified> outcome) {
        if (alive.expired() || weak->m_shutdown || !weak->m_cache_scope.matches(task) || !weak->m_session.matches(context)) return;
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
        if (alive.expired() || weak->m_shutdown || !weak->m_cache_scope.matches(task) || !weak->m_session.matches(context)) return;
        if (cache_error) weak->m_message = "高级图片缓存暂时无法读取，当前输入与结果已保留。";
        weak->publish();
    };
    const bool accepted = Presentation::background_tasks().submit(task, verify,
        [weak, alive, task, context, group_id, verify, finish](Presentation::TaskResult<Verified> outcome) {
            if (alive.expired() || weak->m_shutdown || !weak->m_cache_scope.matches(task) || !weak->m_session.matches(context)) return;
            if (!outcome.value || outcome.error || outcome.value->missing.empty()) { finish(std::move(outcome)); return; }
            const auto missing = outcome.value->missing;
            weak->m_session.apply_snapshot(std::move(outcome.value->snapshot), context, false);
            weak->publish();
            auto count = std::make_shared<std::size_t>(missing.size());
            auto downloaded = [weak, alive, task, context, count, verify, finish](std::string error) {
                if (alive.expired() || weak->m_shutdown || !weak->m_cache_scope.matches(task) || !weak->m_session.matches(context)) return;
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

void AdvancedCreationFeatureHost::poll()
{
    if (!m_service_available || m_shutdown || m_poll_pending || command_pending()) return;
    auto context = m_session.context();
    if (context.group_id.empty()) return;
    m_poll_pending = true;
    m_client.request("GET", "/groups/" + context.group_id, {}, complete([this, context](Json value) {
        if (!m_session.matches(context)) return;
        if (!value.contains("group")) { m_poll_pending = false; return; }
        ingest(std::move(value["group"]), context, false, [this] {
            const auto& group = m_session.group();
            if (group && group->active_operation_id.empty() && !group->model_busy) refresh_history();
        });
    }, false, true), error(false, true));
}

void AdvancedCreationFeatureHost::save_draft(std::function<void()> done, AdvancedMultiviewClient::Error failed)
{
    if (!m_session.group() || !m_session.draft_dirty()) { if (done) done(); return; }
    if (!m_session.group()->active_operation_id.empty() || m_session.group()->model_busy) {
        m_save_timer.StartOnce(1000); if (done) done(); return;
    }
    const auto context = m_session.context();
    const auto input = m_session.draft();
    const auto draft = input.design;
    m_command_state = done ? CommandState::Exclusive : CommandState::BackgroundDraftSave;
    const auto saved = complete([this, context, input, draft, done, failed](Json value) {
        try {
            ingest(std::move(value.at("group")), context, false, [this, draft, done] {
                m_session.mark_draft_saved(draft);
                if (done) done(); else {
                    m_command_state = CommandState::Idle;
                    if (m_session.draft_dirty()) m_save_timer.StartOnce(650);
                    publish();
                }
            }, input, failed);
        } catch (const Json::exception&) {
            if (failed) failed("invalid_response");
            else throw;
        }
    });
    const auto save_failed = failed ? failed : error();
    if (input.uploads != m_session.group()->uploads ||
        std::any_of(input.replaced_uploads.begin(), input.replaced_uploads.end(), [](bool value) { return value; })) {
        m_client.update_group(context.group_id, m_session.group()->revision, input,
            saved, save_failed);
        return;
    }
    m_client.request("PUT", "/groups/" + context.group_id + "/draft",
        {{"expected_group_revision", m_session.group()->revision}, {"draft", AdvancedMultiviewClient::design_json(draft)}},
        saved, save_failed);
}

void AdvancedCreationFeatureHost::open_group(const std::string& id)
{
    if (m_editing_guard && !m_editing_guard()) return;
    if (command_pending() || m_poll_pending || has_pending_receipt() || (m_start_guard && !m_start_guard()) || !AdvancedMultiviewClient::valid_id(id)) return;
    m_command_state = CommandState::Exclusive;
    m_save_timer.Stop();
    m_message.clear();
    publish();
    save_draft([this, id] {
        // The exclusive history command owns this GET. Ordinary polling is
        // suspended, and the previous context stays installed until success.
        m_command_state = CommandState::Exclusive;
        const auto context = m_session.context();
        m_client.request("GET", "/groups/" + id, {}, complete([this, id, context](Json value) {
            if (!value.contains("group")) {
                m_command_state = CommandState::Idle;
                m_message = "历史套图数据不完整，当前创作已保留。";
                publish();
                return;
            }
            ingest(std::move(value["group"]), context, true, [this] { refresh_history(); }, {}, {}, id);
        }), error());
        publish();
    }, error());
}

void AdvancedCreationFeatureHost::rename_group(const std::string& id, std::uint64_t revision, std::string display_name)
{
    if (m_editing_guard && !m_editing_guard()) return;
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
    if (m_editing_guard && !m_editing_guard()) return;
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
    m_stopping_model_job.clear(); m_model_request.reset(); m_confirmed_model_visual.reset();
    m_candidate_plan.reset(); ++m_candidate_plan_sequence; m_preparing_candidate = false;
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

void AdvancedCreationFeatureHost::generate_design(Confirm confirm, GenerationRejected rejected)
{
    if (m_shutdown) return;
    if (m_editing_guard && !m_editing_guard()) { if (rejected) rejected("creation_read_only"); return; }
    m_session.cancel_pending_click();
    if (!eligibility().generate_design) { if (rejected) rejected(generation_rejection(false)); return; }
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
            prepare(id, {{"command_id", new_identity()}, {"kind", "initial"}, {"expected_group_revision", revision},
                         {"design_request", {{"version", 1}, {"scope", "all_inputs"}}}},
                    confirm, context, true);
        }), error());
    } else save_draft([this, confirm] {
        const auto& group = *m_session.group();
        auto request = AdvancedMultiviewClient::design_json(m_session.draft().design);
        request["command_id"] = new_identity(); request["expected_group_revision"] = group.revision;
        request["kind"] = group.published ? "edit" : "initial";
        request["design_request"] = {{"version", 1}, {"scope", "all_inputs"}};
        request["target_views"] = Json::array(); request["base_versions"] = Json::object();
        for (auto view : m_session.all_design_views()) request["target_views"].push_back(view_name(view));
        for (auto view : AllViews) {
            const auto& visible = group.views[index(view)];
            request["base_versions"][view_name(view)] = visible.usable ? Json(visible.version_id) : Json(nullptr);
        }
        prepare(group.id, std::move(request), confirm, m_session.command_context(), false);
    });
    publish();
}

AdvancedMultiviewClient::Error AdvancedCreationFeatureHost::single_design_error(std::function<void(bool)> submitted)
{
    const auto alive = std::weak_ptr<int>(m_lifetime);
    const auto context = m_session.context();
    return [this, alive, context, submitted](std::string message) {
        Presentation::post_gui([this, alive, context, submitted, message = std::move(message)] {
            if (alive.expired() || m_shutdown || !m_session.matches(context)) return;
            m_command_state = CommandState::Idle;
            m_message = ModelGenerationStatusText::localized_advanced_service_error(message).ToUTF8().data();
            if (submitted) submitted(false);
            publish();
        });
    };
}

void AdvancedCreationFeatureHost::generate_single_design(ViewId view, std::string text,
    std::function<void(bool)> submitted, GenerationRejected rejected)
{
    auto reason = m_session.single_design_rejection(view, text);
    if (reason.empty()) {
        const auto status = eligibility();
        // Use the shared command guard. The local plan validates the provider
        // against the actual target mode and its reference or description.
        if (!status.click_generate_design) reason = status.design_reason;
        if (reason.empty() && !m_service_available) reason = "service_unavailable";
    }
    if (!reason.empty() || m_shutdown) {
        if (rejected) rejected(reason);
        if (submitted) submitted(false);
        return;
    }
    m_session.cancel_pending_click();
    m_command_state = CommandState::Exclusive; m_message.clear(); m_save_timer.Stop();
    save_draft([this, view, text = std::move(text), submitted, rejected] {
        if (auto reason = m_session.single_design_rejection(view, text); !reason.empty()) {
            m_command_state = CommandState::Idle;
            if (rejected) rejected(reason);
            if (submitted) submitted(false);
            publish(); return;
        }
        const auto& group = *m_session.group();
        auto request = AdvancedMultiviewClient::design_json(m_session.draft().design);
        request["command_id"] = new_identity(); request["kind"] = "edit";
        request["expected_group_revision"] = group.revision;
        request["target_views"] = Json::array({view_name(view)});
        const auto& visible = group.views[index(view)];
        request["base_versions"] = {{view_name(view), visible.usable ? Json(visible.version_id) : Json(nullptr)}};
        request["design_request"] = {{"version", 1}, {"scope", "single_view"},
                                    {"target_view", view_name(view)}, {"local_prompt", text}};
        prepare(group.id, std::move(request), {}, m_session.command_context(), false, submitted);
    }, single_design_error(submitted));
    publish();
}

void AdvancedCreationFeatureHost::generate_model(Confirm confirm, GenerationRejected rejected)
{
    if (m_shutdown) return;
    if (m_editing_guard && !m_editing_guard()) { if (rejected) rejected("creation_read_only"); return; }
    m_session.flush_click(std::numeric_limits<std::uint64_t>::max());
    if (!eligibility().generate_model || !m_session.group()) { if (rejected) rejected(generation_rejection(true)); return; }
    m_session.begin_model_selection();
    prepare_model_selection([this, confirm, rejected](Json plan) {
        if (!confirm || !confirm(plan) || !confirm_model_selection(plan, rejected))
            cancel_model_selection();
    }, [this, rejected](std::string reason) {
        m_message = ModelGenerationStatusText::localized_advanced_service_error(reason).ToUTF8().data();
        if (rejected) rejected(reason);
        cancel_model_selection();
    });
}

void AdvancedCreationFeatureHost::submit_model_plan(const std::string& group, const Json& plan)
{
    m_library_foreground_job = plan.at("job_id").get<std::string>();
    m_confirmed_model_visual.reset();
    // The confirmed plan already binds these bytes to this task. Waiting
    // animation must not open job.json while the service is replacing it.
    const auto& front = plan.at("approved_multiview_snapshot").at("views").at("front");
    const auto version = front.value("version_id", std::string());
    const auto digest = front.value("sha256", std::string());
    const bool valid_digest = digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
    if (AdvancedMultiviewClient::valid_id(m_library_foreground_job) && AdvancedMultiviewClient::valid_id(version) &&
        valid_digest && front.value("path", std::string()) == "inputs/front.png") {
        const auto path = ModelGenerationPresentation::generated_models_root() / m_library_foreground_job / "inputs" / "front.png";
        m_confirmed_model_visual = ConfirmedModelVisual {m_session.context(), {m_library_foreground_job, version,
            wxString(path.wstring()).ToUTF8().data(), digest, true}};
    }
    const auto request = begin_model_request(m_library_foreground_job);
    m_client.request("POST", "/groups/" + group + "/model-jobs",
        {{"command_id", plan.at("id")}, {"plan_id", plan.at("id")},
         {"confirmation_fingerprint", plan.at("confirmation_fingerprint")}},
        complete([this, request](Json response) {
            if (!model_request_is_current(request)) return;
            m_command_state = CommandState::Idle;
            if (m_stopping_model_job == request.job_id) { settle_receipt(request.job_id, false); return; }
            const auto receipt = m_pending_receipts.find(request.job_id);
            if (receipt == m_pending_receipts.end() ||
                receipt->second.value("group_id", std::string()) != request.context.group_id ||
                !accept_model_job(response.at("job"), request)) {
                m_message = "任务受理身份暂时无法核对，请核对原请求，不要重复提交。";
                publish(); return;
            }
            reconcile_submission(false);
        }), model_request_error(request));
}

bool AdvancedCreationFeatureHost::begin_model_selection(GenerationRejected rejected)
{
    if (m_shutdown) return false;
    const auto eligible = m_session.model_selection_entry_eligibility(capabilities(), command_guards());
    if (!eligible.generate_model || !m_session.group()) {
        if (rejected) rejected(m_editing_guard && !m_editing_guard() ? "creation_read_only" : eligible.model_reason);
        return false;
    }
    m_session.begin_model_selection();
    m_candidate_plan.reset(); ++m_candidate_plan_sequence;
    m_message.clear();
    return true;
}

Eligibility AdvancedCreationFeatureHost::model_selection_eligibility() const
{
    return m_session.candidate_model_eligibility(capabilities(), command_guards(true));
}

void AdvancedCreationFeatureHost::prepare_model_selection(Complete ready, AdvancedMultiviewClient::Error failed)
{
    const auto sequence = ++m_candidate_plan_sequence;
    m_candidate_plan.reset();
    const auto eligible = model_selection_eligibility();
    const auto candidate = m_session.model_candidate();
    if (!eligible.generate_model || !candidate || !m_session.group()) {
        if (m_preparing_candidate) { m_preparing_candidate = false; m_command_state = CommandState::Idle; }
        if (failed) failed(eligible.model_reason);
        publish(); return;
    }
    const auto group = m_session.group()->id;
    const auto request = model_plan_request(new_identity());
    m_preparing_candidate = true; m_command_state = CommandState::Exclusive;
    const auto alive = std::weak_ptr<int>(m_lifetime);
    const auto fail = [this, alive, sequence, candidate, failed](std::string message) {
        Presentation::post_gui([this, alive, sequence, candidate, failed, message = std::move(message)] {
            if (alive.expired() || m_shutdown || sequence != m_candidate_plan_sequence ||
                !m_session.model_candidate() || m_session.model_candidate()->id != candidate->id) return;
            m_preparing_candidate = false; m_command_state = CommandState::Idle;
            if (failed) failed(message);
            publish();
        });
    };
    m_client.request("POST", "/groups/" + group + "/model-plans", request,
        complete([this, sequence, candidate, request, group, ready, failed](Json value) {
            if (sequence != m_candidate_plan_sequence || !m_session.model_candidate() ||
                m_session.model_candidate()->id != candidate->id || m_session.model_candidate()->revision != candidate->revision) return;
            m_preparing_candidate = false; m_command_state = CommandState::Idle;
            if (const auto reason = model_selection_eligibility().model_reason; !reason.empty()) {
                if (failed) failed(reason);
                publish(); return;
            }
            const auto plan = value.find("plan");
            if (plan == value.end() || request != model_plan_request(request.at("command_id").get<std::string>()) ||
                !AdvancedMultiviewClient::model_plan_matches(*plan, request, group)) {
                if (failed) failed("stale_confirmation"); publish(); return;
            }
            m_candidate_plan = CandidatePlan {candidate->id, candidate->revision, sequence, *plan};
            if (ready) ready(*plan);
            publish();
        }), fail);
    publish();
}

AdvancedCreationFeatureHost::Json AdvancedCreationFeatureHost::model_plan_request(const std::string& command) const
{
    auto options = m_session.draft().geometry;
    if (options.provider == "yoyoclaw") {
        options.face_limit = 0;
        options.geometry_quality = "standard";
        options.texture_quality = "standard";
    }
    auto selected = Json::array(), visible = Json::object();
    for (auto view : m_session.candidate_model_views()) {
        selected.push_back(view_name(view));
        const auto& image = m_session.group()->views[index(view)];
        visible[view_name(view)] = {{"version_id", image.version_id}, {"sha256", image.sha256}};
    }
    return {{"schema", AdvancedMultiviewClient::selected_plan_schema}, {"command_id", command},
            {"selected_views", selected}, {"visible_versions", visible}, {"provider", options.provider},
            {"geometry_model", options.model}, {"face_limit", options.face_limit},
            {"geometry_quality", options.geometry_quality}, {"texture_quality", options.texture_quality},
            {"output_format", options.output_format}};
}

bool AdvancedCreationFeatureHost::confirm_model_selection(const Json& plan, GenerationRejected rejected)
{
    const auto candidate = m_session.model_candidate();
    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto reason = model_selection_eligibility().model_reason;
    if (reason.empty() && (!candidate || !m_candidate_plan || m_preparing_candidate ||
        m_candidate_plan->id != candidate->id || m_candidate_plan->revision != candidate->revision ||
        m_candidate_plan->sequence != m_candidate_plan_sequence || m_candidate_plan->plan != plan ||
        plan.value("expires_at", 0.0) <= now ||
        !AdvancedMultiviewClient::model_plan_matches(plan, model_plan_request(plan.at("id").get<std::string>()),
                                                    m_session.context().group_id))) reason = "stale_confirmation";
    if (!reason.empty()) { if (rejected) rejected(reason); return false; }
    const auto local = m_session.candidate_local_draft();
    if (!local) { if (rejected) rejected("stale_confirmation"); return false; }
    const auto group = m_session.group()->id;
    const auto job = plan.at("job_id").get<std::string>();
    if (!save_pending_receipt({{"schema", "advanced-desktop-submission-v1"}, {"kind", "model"},
        {"group_id", group}, {"operation_id", job}, {"new_group", false},
        {"model_selected", candidate->selected}, {"plan_id", plan.at("id")},
        {"confirmation_fingerprint", plan.at("confirmation_fingerprint")}})) {
        clear_pending_receipt(job); publish(); return false;
    }
    // No model-jobs POST has been sent. Reconciliation would consume this
    // unused plan, so discard its local receipt and preserve the candidate.
    if (!save_local_draft(&*local)) { clear_pending_receipt(job); publish(); return false; }
    if (!m_session.commit_model_selection(candidate->id, candidate->revision)) {
        clear_pending_receipt(job); publish(); return false;
    }
    m_candidate_plan.reset(); ++m_candidate_plan_sequence;
    m_command_state = CommandState::Exclusive; m_message.clear();
    submit_model_plan(group, plan); publish();
    return true;
}

void AdvancedCreationFeatureHost::cancel_model_selection()
{
    ++m_candidate_plan_sequence; m_candidate_plan.reset(); m_session.cancel_model_selection();
    if (m_preparing_candidate) { m_preparing_candidate = false; m_command_state = CommandState::Idle; }
    publish();
}

ModelJobEvent AdvancedCreationFeatureHost::begin_model_request(const std::string& job)
{
    m_stopping_model_job.clear();
    ModelJobEvent request {m_session.context(), job, ++m_model_request_sequence};
    m_model_request = request;
    if (m_model_job_listener) m_model_job_listener(request);
    return request;
}

bool AdvancedCreationFeatureHost::model_request_is_current(const ModelJobEvent& request) const
{
    return m_model_request && m_model_request->same_request(request) && m_session.matches(request.context);
}

bool AdvancedCreationFeatureHost::model_stop_pending() const
{
    const auto& state = m_model_monitor.snapshot();
    return !m_stopping_model_job.empty() &&
        (state.job_id != m_stopping_model_job || state.stage != ModelGenerationUIStage::Stopped);
}

AdvancedMultiviewClient::Error AdvancedCreationFeatureHost::model_request_error(const ModelJobEvent& request)
{
    auto* weak = this;
    const auto alive = std::weak_ptr<int>(m_lifetime);
    return [weak, alive, request](std::string message) {
        Presentation::post_gui([weak, alive, request, message = std::move(message)] {
            if (alive.expired() || weak->m_shutdown || !weak->model_request_is_current(request)) return;
            weak->m_command_state = CommandState::Idle;
            weak->m_message = ModelGenerationStatusText::localized_advanced_service_error(message).ToUTF8().data();
            if (weak->m_stopping_model_job == request.job_id) weak->settle_receipt(request.job_id, false);
            weak->publish();
        });
    };
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

void AdvancedCreationFeatureHost::prepare(const std::string& group, Json request, Confirm confirm,
                                        CommandContext context, bool new_group, std::function<void(bool)> submitted)
{
    const auto intent = request.value("design_request", Json());
    const bool single = intent.is_object() && intent.value("scope", std::string()) == "single_view";
    auto expected = m_session.all_design_views();
    if (single) expected = {*parse_view(intent.at("target_view").get<std::string>())};
    m_client.request("POST", "/groups/" + group + "/plans", request,
        complete([this, group, confirm, context, new_group, intent, expected, single, submitted, request](Json value) {
            if ((m_editing_guard && !m_editing_guard()) || !m_session.matches_command(context, false)) {
                if (submitted) submitted(false);
                m_command_state = CommandState::Idle; publish(); return;
            }
            auto plan = value.at("plan");
            if (!intent.is_null() && !AdvancedMultiviewClient::design_plan_matches(plan,request,group,expected)) {
                m_command_state = CommandState::Idle;
                m_message = "设计图计划与本次生成范围不一致，请更新匹配的服务后重试。";
                if (submitted) submitted(false);
                publish(); return;
            }
            const bool accepted = single || (confirm && confirm(plan));
            if (!accepted || (m_editing_guard && !m_editing_guard()) || !m_session.matches_command(context, false)) {
                if (submitted) submitted(false);
                if (m_session.matches(context.callback)) { m_command_state = CommandState::Idle; publish(); }
                return;
            }
            auto draft = m_session.draft().design;
            if (!single && draft.model.empty() && draft.provider == plan.at("draft").at("design_provider")) {
                draft.model = plan.at("draft").at("design_model").get<std::string>();
                m_session.set_design_draft(std::move(draft));
            }
            run_plan(group, std::move(plan), new_group, submitted);
        }), single ? single_design_error(submitted) : error());
}

void AdvancedCreationFeatureHost::run_plan(const std::string& group, Json plan, bool new_group, std::function<void(bool)> submitted)
{
    const auto& frozen = plan.at("frozen");
    // Guidance follows the full creation target set, not this repaint's subset.
    const auto target_names = frozen.value("design_targets", plan.at("targets"));
    const auto input_epoch = frozen.at("input_epoch").get<std::string>();
    const auto intent = plan.value("design_request", Json());
    const bool single = intent.is_object() && intent.value("scope", std::string()) == "single_view";
    if (!save_pending_receipt({{"schema", "advanced-desktop-submission-v1"}, {"group_id", group},
        {"operation_id", plan.at("id")}, {"new_group", new_group},
        {"design_targets", target_names}, {"input_epoch", input_epoch}, {"design_request", intent}})) {
        // A local edit has not reached POST yet. Do not leave an unsent
        // request blocking an explicit retry after storage is repaired.
        if (single) clear_pending_receipt(plan.at("id").get<std::string>());
        if (submitted) submitted(false);
        m_command_state = CommandState::Idle; publish(); return;
    }
    const auto context = m_session.context();
    const auto operation = plan.at("id").get<std::string>();
    if (single && submitted) m_single_submissions[operation] = submitted;
    m_client.request("POST", "/groups/" + group + "/operations",
        {{"command_id", plan.at("id")}, {"plan_id", plan.at("id")},
         {"confirmation_fingerprint", plan.at("confirmation_fingerprint")}},
        complete([this, group, new_group, context, operation, target_names, input_epoch, single, intent](Json response) {
            if (!m_session.matches(context)) return;
            m_command_state = CommandState::Idle;
            if (single && (response.at("operation").value("id", std::string()) != operation ||
                response.at("operation").value("group_id", std::string()) != group ||
                response.at("operation").value("design_request", Json()) != intent)) {
                m_message = "任务受理身份暂时无法核对，请核对原请求，不要重复提交。"; publish(); return;
            }
            if (const auto found = m_single_submissions.find(operation); found != m_single_submissions.end()) {
                const auto callback = found->second; m_single_submissions.erase(found); callback(true);
            }
            if (new_group) m_session.begin_open(group);
            const auto callback = m_session.context();
            std::vector<ViewId> targets;
            for (const auto& name : target_names)
                if (auto view = parse_view(name.get<std::string>()); view) targets.push_back(*view);
            if (new_group) m_session.arm_initial_design_guidance(callback, targets, input_epoch);
            else if (!single) m_session.track_initial_design_plan(callback, targets, input_epoch);
            m_poll_pending = true;
            m_client.request("GET", "/groups/" + group, {}, complete([this, callback, new_group, operation](Json value) {
                if (!m_session.matches(callback)) return;
                ingest(std::move(value.at("group")), callback, new_group, [this, operation] { clear_pending_receipt(operation); refresh_history(); });
            }, false, true), error(false, true));
            publish();
        }), single ? single_design_error(submitted) : error());
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
    m_single_submissions.erase(operation);
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
    const bool model_receipt = receipt.value("kind", std::string("design")) == "model";
    const auto request = model_receipt && m_model_request && m_model_request->job_id == operation
        ? m_model_request : std::optional<ModelJobEvent>();
    const auto context = m_session.context();
    const bool owned = model_receipt ? request && model_request_is_current(*request) : receipt_is_current(receipt);
    const auto current = [this, context, request, owned] {
        return owned && m_session.matches(context) && (!request || model_request_is_current(*request));
    };
    if (current()) m_command_state = CommandState::Exclusive;
    m_reconciling.insert(operation);
    const auto collection = receipt.value("kind", std::string("design")) == "model" ? "/model-jobs/" : "/operations/";
    auto* weak = this;
    const auto alive = std::weak_ptr<int>(m_lifetime);
    auto failed = [weak, alive, operation, context, current](std::string message) {
        Presentation::post_gui([weak, alive, operation, context, current, message = std::move(message)] {
            if (alive.expired() || weak->m_shutdown) return;
            weak->m_reconciling.erase(operation);
            if (current()) {
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
                if (current() && collection == "/model-jobs/") {
                    m_model_context = context;
                    m_model_monitor.observe(operation, {});
                    m_model_monitor.request_stop();
                }
                m_client.request("POST", "/groups/" + group + collection + operation + "/stop", Json::object(),
                    complete([this, operation, context, current](Json value) {
                        clear_pending_receipt(operation);
                        if (current()) {
                            m_command_state = CommandState::Idle;
                            if (value.contains("job")) {
                                const auto state = value["job"].value("state", std::string());
                                if (state == "ready" || state == "failed" || state == "stopped" || state == "cancelled")
                                    m_model_monitor.finish_stop(operation);
                            }
                            poll(); publish();
                        }
                    }, false, false, false), failed);
                return;
            }
            if (abandoned) {
                clear_pending_receipt(operation);
                if (current()) {
                    m_command_state = CommandState::Idle;
                    if (collection == "/model-jobs/") {
                        m_model_context = context;
                        m_model_monitor.finish_stop(operation);
                    }
                    m_message = "已停止生成"; publish();
                }
                return;
            }
            if (!current()) { clear_pending_receipt(operation); return; }
            m_command_state = CommandState::Idle;
            if (!m_session.matches(context)) return;
            if (!value.at("accepted").get<bool>()) {
                clear_pending_receipt(operation); m_message = "原确认请求未受理，可以重新确认生成。"; publish(); return;
            }
            const bool model_request = receipt.value("kind", std::string("design")) == "model";
            if (!model_request) {
                const auto found = m_single_submissions.find(operation);
                if (found != m_single_submissions.end()) {
                    const auto submitted = found->second; m_single_submissions.erase(found); submitted(true);
                }
            }
            const bool restore = !m_session.group() || m_session.group()->id != group;
            auto callback = context;
            if (restore) callback = m_session.begin_open(group);
            // Only a request made in this live Session can introduce guidance.
            // Durable receipts restored at startup have no _session_epoch.
            if (!model_request && receipt.value("new_group", false) &&
                receipt.value("_session_epoch", std::uint64_t(0)) == context.epoch) {
                std::vector<ViewId> targets;
                for (const auto& name : receipt.value("design_targets", Json::array()))
                    if (auto view = parse_view(name.get<std::string>()); view) targets.push_back(*view);
                m_session.arm_initial_design_guidance(callback, targets, receipt.value("input_epoch", std::string()));
            }
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
            ingest(std::move(value.at("group")), callback, restore && !model_request, [this, operation, announce_recovery, current] {
                clear_pending_receipt(operation);
                if (!current()) return;
                if (!has_pending_receipt()) m_message = announce_recovery
                    ? "原请求已恢复，没有新建生成请求。" : std::string();
                refresh_history();
            });
        }, false, false, false), failed);
    if (current()) publish();
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
        m_stopping_model_job = m_model_monitor.snapshot().job_id;
        m_model_monitor.stop_local(); return;
    }
    if (m_model_request && m_model_request->phase == ModelJobEvent::Phase::Requested &&
        m_session.matches(m_model_request->context)) {
        const auto operation = m_model_request->job_id;
        const auto receipt = m_pending_receipts.find(operation);
        if (receipt != m_pending_receipts.end()) {
            m_stopping_model_job = operation;
            auto stopped = receipt->second; stopped["stop_intent"] = true;
            save_pending_receipt(std::move(stopped));
            m_command_state = CommandState::Idle; m_message = "正在停止生成…";
            settle_receipt(operation, false); publish(); return;
        }
    }
    if (!eligibility().stop || !m_session.group()) return;
    const auto& group = *m_session.group(); m_command_state = CommandState::Exclusive;
    const auto model_job = model_stop_pending() ? m_stopping_model_job : group.model_busy ? group.model_job_id : std::string();
    if (!model_job.empty()) {
        m_stopping_model_job = model_job;
        m_model_monitor.request_stop();
        const auto request = m_model_request;
        m_client.request("POST", "/groups/" + group.id + "/model-jobs/" + model_job + "/stop", Json::object(),
            complete([this, request](Json) {
                if (!request || !model_request_is_current(*request)) return;
                m_command_state = CommandState::Idle; poll(); publish();
            }), request ? model_request_error(*request) : error());
        return;
    }
    const auto route = "/operations/" + group.active_operation_id;
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
