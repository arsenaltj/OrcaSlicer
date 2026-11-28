#pragma once

#include "AdvancedCreationSession.hpp"
#include "AdvancedMultiviewClient.hpp"
#include "AdvancedModelTaskMonitor.hpp"
#include "GenerationActionFeedback.hpp"
#include "slic3r/GUI/AIModelGenerationClient.hpp"
#include "slic3r/GUI/Presentation/UiRuntime.hpp"
#include <wx/event.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <wx/window.h>
#include <map>
#include <memory>
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
    std::string generation_rejection(bool model) const;
    const std::string& message() const { return m_message; }
    const Json& history() const { return m_history; }
    bool command_pending() const { return m_command_state != CommandState::Idle; }
    bool draft_editing_locked() const { return (m_editing_guard && !m_editing_guard()) || m_command_state == CommandState::Exclusive || model_stop_pending(); }
    bool has_pending_receipt() const;
    const ProviderCatalog& image_catalog() const { return m_image_catalog; }
    void reconcile_submission();
    void set_listener(std::function<void()> listener);
    // The flag identifies an accepted new generation, never a navigation request.
    void set_model_state_listener(std::function<void(const ModelGenerationUIState&, bool)> listener)
    { m_model_state_listener = std::move(listener); }
    void set_library_commit_listener(std::function<void(ModelLibraryCommit)> listener)
    { m_model_monitor.set_library_commit_listener([this, listener = std::move(listener)](ModelLibraryCommit event) {
        if (m_shutdown || !listener) return;
        event.foreground = event.foreground && event.entry.job_id == m_library_foreground_job && m_session.matches(m_model_context);
        if (event.entry.job_id == m_library_foreground_job) m_library_foreground_job.clear();
        listener(std::move(event));
    }); }
    const ModelGenerationUIState& model_state() const { return m_model_monitor.snapshot(); }
    const std::optional<ModelLibraryEntry>& model_artifact() const { return m_model_monitor.artifact(); }
    void retry_local_model() { m_model_monitor.retry_local(); }
    void get_provider_catalog(const std::string& role, const std::string& provider,
        AIModelGenerationClient::CatalogFn complete, AIModelGenerationClient::ErrorFn error);
    void set_model_job_listener(std::function<void(const AdvancedCreation::ModelJobEvent&)> listener)
    { m_model_job_listener = std::move(listener); }
    void set_start_guard(std::function<bool()> guard) { m_start_guard = std::move(guard); }
    void set_generation_guard(std::function<bool()> guard) { m_generation_guard = std::move(guard); }
    void set_editing_guard(std::function<bool()> guard) { m_editing_guard = std::move(guard); }
    void set_service_available(bool available);
    void refresh_capabilities(const std::string& scope = {});
    void refresh_image_catalog();
    void refresh_history();
    void changed();
    void open_group(const std::string& id);
    void rename_group(const std::string& id, std::uint64_t revision, std::string display_name);
    void new_creation();
    void generate_design(Confirm confirm, GenerationRejected rejected = {});
    void generate_single_design(AdvancedCreation::ViewId view, std::string text,
                                std::function<void(bool)> submitted, GenerationRejected rejected = {});
    void generate_model(Confirm confirm, GenerationRejected rejected = {});
    bool begin_model_selection(GenerationRejected rejected = {});
    void prepare_model_selection(AdvancedMultiviewClient::Complete ready, AdvancedMultiviewClient::Error failed);
    bool confirm_model_selection(const Json& plan, GenerationRejected rejected = {});
    void cancel_model_selection();
    AdvancedCreation::Eligibility model_selection_eligibility() const;
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
    void observe_model_job(const std::string& job, bool new_generation, bool restart = false);
    void poll();
    void ingest(Json group, AdvancedCreation::CallbackContext context, bool restore,
                std::function<void()> done = {}, std::optional<AdvancedCreation::InputDraft> saved_input = {},
                AdvancedMultiviewClient::Error failed = {}, std::string history_target = {});
    void save_draft(std::function<void()> done, AdvancedMultiviewClient::Error failed = {});
    bool save_local_draft(const AdvancedCreation::LocalDraft* candidate = nullptr);
    void restore_local_draft(const std::string& group);
    void prepare(const std::string& group, Json request, Confirm confirm,
                 AdvancedCreation::CommandContext context, bool new_group, std::function<void(bool)> submitted = {});
    void run_plan(const std::string& group, Json plan, bool new_group, std::function<void(bool)> submitted = {});
    void submit_model_plan(const std::string& group, const Json& plan);
    AdvancedCreation::HostGuards command_guards(bool preparing_candidate = false) const;
    Json model_plan_request(const std::string& command) const;
    AdvancedMultiviewClient::Error single_design_error(std::function<void(bool)> submitted);
    bool save_pending_receipt(Json receipt);
    void clear_pending_receipt(const std::string& operation);
    bool receipt_is_current(const Json& receipt) const;
    void settle_receipt(const std::string& operation, bool announce);
    void reconcile_submission(bool announce_recovery);
    AdvancedCreation::ModelJobEvent begin_model_request(const std::string& job);
    bool model_request_is_current(const AdvancedCreation::ModelJobEvent& request) const;
    bool model_stop_pending() const;
    AdvancedMultiviewClient::Error model_request_error(const AdvancedCreation::ModelJobEvent& request);
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
    std::string m_library_foreground_job;
    std::function<void(const AdvancedCreation::ModelJobEvent&)> m_model_job_listener;
    std::optional<AdvancedCreation::ModelJobEvent> m_model_request;
    struct ConfirmedModelVisual {
        AdvancedCreation::CallbackContext context;
        ModelGenerationVisualReference reference;
    };
    std::optional<ConfirmedModelVisual> m_confirmed_model_visual;
    std::uint64_t m_model_request_sequence {0};
    std::string m_stopping_model_job;
    std::function<bool()> m_start_guard;
    std::function<bool()> m_generation_guard;
    std::function<bool()> m_editing_guard;
    ProviderCatalog m_image_catalog;
    std::map<std::string, ProviderCatalog> m_feedback_catalogs;
    std::map<std::string, std::uint64_t> m_feedback_catalog_sequences;
    std::uint64_t m_image_catalog_sequence {0};
    Json m_image_capabilities;
    Json m_geometry_capabilities;
    std::uint64_t m_image_capability_sequence {0};
    std::uint64_t m_geometry_capability_sequence {0};
    Json m_history = Json::array();
    std::map<std::string, Json> m_pending_receipts;
    std::map<std::string, Json> m_written_local_drafts;
    std::set<std::string> m_unreadable_local_drafts;
    std::set<std::string> m_reconciling;
    std::map<std::string, std::function<void(bool)>> m_single_submissions;
    struct CandidatePlan { std::uint64_t id, revision, sequence; Json plan; };
    std::optional<CandidatePlan> m_candidate_plan;
    std::uint64_t m_candidate_plan_sequence {0};
    bool m_preparing_candidate {false};
    bool m_restore_receipts {true};
    std::string m_message;
    bool m_service_available {false};
    // Auto-save serializes requests without disabling the editor or its IME.
    enum class CommandState { Idle, BackgroundDraftSave, Exclusive };
    CommandState m_command_state {CommandState::Idle};
    bool m_poll_pending {false};
    bool m_shutdown {false};
    // Only checked on the GUI thread; worker callbacks copy the weak token.
    std::shared_ptr<int> m_lifetime {std::make_shared<int>(0)};
    Presentation::TaskScope m_cache_scope;
};
}
