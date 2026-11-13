#pragma once
#include "ModelLibraryApplication.hpp"
#include "ModelLibraryLoader.hpp"
#include "slic3r/Utils/PlatformLibrary/LibraryWorker.hpp"
#include <wx/timer.h>
#include <map>
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
        std::function<bool(const boost::filesystem::path&)> native_import;
    };
    LibraryWorkspaceService(std::shared_ptr<PlatformAccountService>, std::shared_ptr<ModelLibraryApplication>, Ports);
    ~LibraryWorkspaceService() override;
    void subscribe(std::function<void(const Json&)> callback) {m_emit=std::move(callback);}
    void visible(bool);
    void ready(bool value=true);
    void command(const Json&);
    void refresh_local();
    void set_project_available(bool value) {m_project=value;publish_account();}
private:
    using Client=PlatformLibrary::Client;
    using Value=PlatformLibrary::Worker::Value;
    void poll();
    void emit(Json);
    void publish_account();
    void account_changed();
    void invalidate();
    Client client() const;
    void list();
    void local_cards();
    void images(const Json&);
    void show_detail(const std::string& key);
    void start_download(const std::string& file, bool resumed=false);
    void local_action(const Json&);
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
    std::string m_scope {"library"},m_filter {"all"},m_detail_key,m_subject,m_visitor;
    std::string m_pending_download_file,m_pending_download_key;
    std::uint64_t m_epoch {1},m_generation {0},m_scan {0};
    std::shared_ptr<const std::string> m_geometry;
    std::size_t m_geometry_offset {0};
    bool m_visible {false},m_ready {false},m_project {false},m_downloading {false},m_mutating {false},m_resume_detail {false};
    bool m_geometry_pending {false};
    bool m_download_reauth_attempted {false};
};
}
