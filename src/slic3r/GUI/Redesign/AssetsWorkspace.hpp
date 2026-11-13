#pragma once
#include <wx/panel.h>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>
#include "../Presentation/UiRuntime.hpp"
#include "LibraryPresentationChannel.hpp"
class wxWebView;
namespace Slic3r::GUI {
class LibraryWorkspaceService;
class ModelLibraryApplication;
class AssetsWorkspace final : public wxPanel {
public:
    explicit AssetsWorkspace(wxWindow*);
    ~AssetsWorkspace() override;
    void attach(std::shared_ptr<LibraryWorkspaceService>, std::shared_ptr<ModelLibraryApplication>);
    void set_project_available(bool);
    void set_presented(bool);
private:
    void load();
    void send(const nlohmann::json&);
    void send_initialization();
    void flush_messages();
    Presentation::RefreshScheduler m_messages;
    LibraryPresentationChannel m_channel;
    nlohmann::json m_initialization;
    std::shared_ptr<LibraryWorkspaceService> m_service;
    std::shared_ptr<ModelLibraryApplication> m_local;
    wxWebView* m_web {nullptr};
    wxWindow* m_fallback {nullptr};
    bool m_loaded {false}, m_project {false}, m_presented {false};
};
}
