#pragma once

#include "AdvancedCreationSession.hpp"
#include "AdvancedMultiviewClient.hpp"
#include "AdvancedModelTaskMonitor.hpp"
#include "slic3r/GUI/AIModelGenerationClient.hpp"
#include "slic3r/GUI/Presentation/UiRuntime.hpp"
#include <wx/event.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <wx/window.h>
#include <map>
#include <set>

namespace Slic3r::GUI {

// Owns only advanced state and requests. All public commands run on the GUI thread.
class AdvancedCreationFeatureHost final : public wxEvtHandler {
public:
    using Json = nlohmann::json;
    using Confirm = std::function<bool(const Json&)>;
    using ProviderCatalog = AIModelGenerationClient::ProviderCatalog;
    explicit AdvancedCreationFeatureHost(wxWindow* owner, std::string endpoint = {});
    ~AdvancedCreationFeatureHost() override;
    AdvancedCreation::Session& session() { return m_session; }
    const AdvancedCreation::Session& session() const { return m_session; }
    AdvancedCreation::Eligibility eligibility() const;
    const std::string& message() const { return m_message; }
    const Json& history() const { return m_history; }
    bool command_pending() const { return m_command_state != CommandState::Idle; }
    bool draft_editing_locked() const { return m_command_state == CommandState::Exclusive; }
    bool has_pending_receipt() const;
    const ProviderCatalog& image_catalog() const { return m_image_catalog; }
    void reconcile_submission();
    void set_listener(std::function<void()> listener);
    void set_model_state_listener(std::function<void(const ModelGenerationUIState&, bool)> listener)
    { m_model_state_listener = std::move(listener); }
    const ModelGenerationUIState& model_state() const { return m_model_monitor.snapshot(); }
    const std::optional<ModelLibraryEntry>& model_artifact() const { return m_model_monitor.artifact(); }
    void show_model();
    void retry_local_model() { m_model_monitor.retry_local(); }
    void get_provider_catalog(const std::string& role, const std::string& provider,
        AIModelGenerationClient::CatalogFn complete, AIModelGenerationClient::ErrorFn error);
    void set_model_job_listener(std::function<void(const AdvancedCreation::ModelJobEvent&)> listener)
    { m_model_job_listener = std::move(listener); }
    void set_start_guard(std::function<bool()> guard) { m_start_guard = std::move(guard); }
    void set_service_available(bool available);
    void refresh_capabilities();
    void refresh_image_catalog();
    void refresh_history();
    void changed();
    void open_group(const std::string& id);
    void rename_group(const std::string& id, std::uint64_t revision, std::string display_name);
    void new_creation();
    void generate_design(Confirm confirm);
    void generate_model(Confirm confirm);
    void resume_model();
    void retry(AdvancedCreation::ViewId view, Confirm confirm);
    void stop();
    void recover(AdvancedCreation::ViewId view);
    void shutdown();

private:
    using Complete = AdvancedMultiviewClient::Complete;
    Complete complete(std::function<void(Json)> callback, bool release_command = true, bool release_poll = false,
                      bool session_bound = true);
    AdvancedMultiviewClient::Error error(bool release_command = true, bool release_poll = false);
    void publish();
    void observe_model_job(const std::string& job, bool open_route, bool restart = false);
    void poll(bool restore = false);
    void ingest(Json group, AdvancedCreation::CallbackContext context, bool restore,
                std::function<void()> done = {}, std::optional<AdvancedCreation::InputDraft> saved_input = {});
    void save_draft(std::function<void()> done);
    void prepare(const std::string& group, Json request, Confirm confirm,
                 AdvancedCreation::CommandContext context, bool new_group);
    void run_plan(const std::string& group, Json plan, bool new_group);
    bool save_pending_receipt(Json receipt);
    void clear_pending_receipt(const std::string& operation);
    bool receipt_is_current(const Json& receipt) const;
    void settle_receipt(const std::string& operation, bool announce);
    void reconcile_submission(bool announce_recovery);
    AdvancedCreation::ModelJobEvent begin_model_request(const std::string& job);
    bool accept_model_job(const Json& job, const AdvancedCreation::ModelJobEvent& request);
    AdvancedCreation::Capabilities capabilities() const;
    wxWeakRef<wxWindow> m_owner;
    AdvancedCreation::Session m_session;
    AdvancedMultiviewClient m_client;
    AIModelGenerationClient m_catalog_client;
    AdvancedModelTaskMonitor m_model_monitor;
    AdvancedCreation::CallbackContext m_model_context;
    boost::filesystem::path m_cache;
    wxTimer m_poll_timer;
    wxTimer m_save_timer;
    std::function<void()> m_listener;
    std::function<void(const ModelGenerationUIState&, bool)> m_model_state_listener;
    std::function<void(const AdvancedCreation::ModelJobEvent&)> m_model_job_listener;
    std::optional<AdvancedCreation::ModelJobEvent> m_model_request;
    std::uint64_t m_model_request_sequence {0};
    std::function<bool()> m_start_guard;
    ProviderCatalog m_image_catalog;
    std::uint64_t m_image_catalog_sequence {0};
    Json m_capabilities;
    Json m_history = Json::array();
    std::map<std::string, Json> m_pending_receipts;
    std::set<std::string> m_reconciling;
    bool m_restore_receipts {true};
    std::string m_message;
    bool m_service_available {false};
    // Auto-save serializes requests without disabling the editor or its IME.
    enum class CommandState { Idle, BackgroundDraftSave, Exclusive };
    CommandState m_command_state {CommandState::Idle};
    bool m_poll_pending {false};
    bool m_shutdown {false};
    Presentation::TaskScope m_cache_scope;
};
}
