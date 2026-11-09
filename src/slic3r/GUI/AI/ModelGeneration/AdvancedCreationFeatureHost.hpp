#pragma once

#include "AdvancedCreationSession.hpp"
#include "AdvancedMultiviewClient.hpp"
#include <wx/event.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <wx/window.h>

namespace Slic3r::GUI {

// Owns only advanced state and requests. All public commands run on the GUI thread.
class AdvancedCreationFeatureHost final : public wxEvtHandler {
public:
    using Json = nlohmann::json;
    using Confirm = std::function<bool(const Json&)>;
    explicit AdvancedCreationFeatureHost(wxWindow* owner);
    ~AdvancedCreationFeatureHost() override;
    AdvancedCreation::Session& session() { return m_session; }
    const AdvancedCreation::Session& session() const { return m_session; }
    AdvancedCreation::Eligibility eligibility() const;
    const std::string& message() const { return m_message; }
    const Json& history() const { return m_history; }
    bool command_pending() const { return m_command_pending; }
    bool has_pending_receipt() const { return !m_pending_receipt.is_null(); }
    void reconcile_submission();
    void set_listener(std::function<void()> listener);
    void set_model_job_listener(std::function<void(const std::string&)> listener)
    { m_model_job_listener = std::move(listener); }
    void set_start_guard(std::function<bool()> guard) { m_start_guard = std::move(guard); }
    void set_service_available(bool available);
    void refresh_capabilities();
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
    Complete complete(std::function<void(Json)> callback, bool release_command = true, bool release_poll = false);
    AdvancedMultiviewClient::Error error(bool release_command = true, bool release_poll = false);
    void publish();
    void poll(bool restore = false);
    void ingest(Json group, AdvancedCreation::CallbackContext context, bool restore,
                std::function<void()> done = {});
    void save_draft(std::function<void()> done);
    void prepare(const std::string& group, Json request, Confirm confirm,
                 AdvancedCreation::CommandContext context, bool new_group);
    void run_plan(const std::string& group, Json plan, bool new_group);
    bool save_pending_receipt(Json receipt);
    void clear_pending_receipt();
    void reconcile_submission(bool announce_recovery);
    AdvancedCreation::Capabilities capabilities() const;
    wxWeakRef<wxWindow> m_owner;
    AdvancedCreation::Session m_session;
    AdvancedMultiviewClient m_client;
    boost::filesystem::path m_cache;
    wxTimer m_poll_timer;
    wxTimer m_save_timer;
    std::function<void()> m_listener;
    std::function<void(const std::string&)> m_model_job_listener;
    std::function<bool()> m_start_guard;
    Json m_capabilities;
    Json m_history = Json::array();
    Json m_pending_receipt;
    std::string m_message;
    bool m_service_available {false};
    bool m_command_pending {false};
    bool m_poll_pending {false};
    bool m_shutdown {false};
};
}
