#pragma once
#include "LibraryDownloadRequest.hpp"
#include "ModelLibraryApplication.hpp"
#include "ModelLibraryLoader.hpp"
#include "slic3r/Utils/PlatformLibrary/LibraryWorker.hpp"
#include <wx/timer.h>
#include <map>
#include <optional>
#include <set>

namespace Slic3r::GUI {
class PlatformAccountService;
// Sole owner of directory/account-record/download state for the HTML asset workspace.
// Presentation adapters may pass typed commands, never URLs, credentials or filesystem paths.
class LibraryWorkspaceService final : public wxEvtHandler {
public:
    using Json=PlatformLibrary::Json;
    struct Ports {
        std::function<void()> login, storage, save_project;
        std::function<bool(const boost::filesystem::path&, const std::function<bool(int)>&)> native_import;
        std::function<bool()> upload;
        std::function<bool(const std::string&)> share;
    };
    LibraryWorkspaceService(std::shared_ptr<PlatformAccountService>, std::shared_ptr<ModelLibraryApplication>, Ports);
    ~LibraryWorkspaceService() override;
    void subscribe(std::function<void(const Json&)> callback) {m_emit=std::move(callback);}
    void visible(bool);
    void ready(bool value=true);
    void activated();
    void command(const Json&);
    void refresh_local();
    void set_project_available(bool value) {m_project=value;publish_account();}
private:
    using Client=PlatformLibrary::Client;
    using Value=PlatformLibrary::Worker::Value;
    void poll();
    void emit(Json);
    void publish_account();
    void account_changed(bool publish = false);
    void invalidate_remote();
    void invalidate();
    Client client() const;
    void list();
    void local_cards();
    void images(const Json&);
    void show_detail(const std::string& key);
    void request_detail();
    void start_download(const Json&);
    void queue_download();
    void download_result(PlatformLibrary::Worker::Result&);
    void publish_download();
    bool download_active() const { return m_download && m_download->active(); }
    const ModelLibraryEntry* registered_file(const std::string& file) const;
    Json file_actions(const ModelLibraryEntry&) const;
    void publish_file_actions();
    void finish_registration(bool scanned);
    void detail_action(const Json&);
    void request_detail_content(bool recovery = false);
    void request_community(const std::string& kind, std::size_t offset=0, bool recovery=false);
    void change_community(const std::string& kind, const Json&);
    void request_geometry(bool interactive);
    void local_action(const Json&);
    void run_native_import();
    void geometry_chunks();
    bool require_login();
    static std::string key(const Json&);
    std::shared_ptr<PlatformAccountService> m_account;
    std::shared_ptr<ModelLibraryApplication> m_local;
    Ports m_ports;
    PlatformLibrary::Worker m_worker;
    std::unique_ptr<ModelLibraryLoader> m_loader;
    std::function<void(const Json&)> m_emit;
    wxTimer m_timer;
    Json m_query=Json::object(),m_detail,m_detail_selection;
    Json m_personal=Json::object();
    std::map<std::string,Json> m_models;
    std::set<std::string> m_image_requests;
    std::string m_scope {"library"},m_filter {"all"},m_detail_key,m_subject,m_api,m_visitor;
    PlatformLibrary::Cancellation m_access_lifetime;
    std::optional<LibraryDownloadRequest> m_download;
    std::optional<ModelLibraryActionToken> m_native_import;
    std::string m_comment_body,m_comment_token;
    std::uint64_t m_epoch {1},m_account_generation {0},m_logout_revision {0},m_scan {0};
    std::uint64_t m_scan_removal_revision {0},m_scan_commit_revision {0},m_seen_removal_revision {0},m_seen_local_revision {0};
    std::uint64_t m_thumbnail_local_revision {0};
    bool m_local_scan_running {false},m_local_rescan_pending {false},m_local_scan_completed {false};
    std::shared_ptr<const std::string> m_geometry;
    std::size_t m_geometry_offset {0};
    bool m_visible {false},m_ready {false},m_project {false},m_mutating {false},m_resume_detail {false};
    bool m_geometry_pending {false};
    bool m_account_refreshing {false},m_resume_geometry {false};
    bool m_upload_pending {false};
    bool m_import_running {false},m_import_cancelled {false},m_import_publishing {false};
};
}
