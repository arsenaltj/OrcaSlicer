#include "AssetsWorkspace.hpp"
#include "RedesignTheme.hpp"
#include "../AI/ModelGeneration/LibraryWorkspaceService.hpp"
#include "../AI/ModelGeneration/ModelLibraryPanel.hpp"
#include "../Widgets/WebView.hpp"
#include "../GUI.hpp"
#include "slic3r/Utils/HonorAccount/HonorOAuth.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/filename.h>
#include <wx/filesys.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/webview.h>

namespace Slic3r::GUI {
namespace {
wxString installed_url() {
    return wxFileSystem::FileNameToURL(wxFileName(from_u8(resources_dir())+"/web/model_library/index.html"));
}
}
AssetsWorkspace::AssetsWorkspace(wxWindow* parent)
    :wxPanel(parent),m_nonce(HonorAccount::random_value())
{
    SetBackgroundColour(RedesignTheme::background_colour());SetSizer(new wxBoxSizer(wxVERTICAL));
    Bind(wxEVT_SHOW,[this](wxShowEvent& event) {
        if(event.IsShown()) load();
        if(m_service && !m_fallback) m_service->visible(event.IsShown());event.Skip();
    });
}
AssetsWorkspace::~AssetsWorkspace() {
    if(m_service) {m_service->subscribe({});m_service->visible(false);}
}
void AssetsWorkspace::attach(std::shared_ptr<LibraryWorkspaceService> service,std::shared_ptr<ModelLibraryApplication> local) {
    m_service=std::move(service);m_local=std::move(local);
    m_service->subscribe([this](const nlohmann::json& value){send(value);});
    m_service->set_project_available(m_project);
    if(IsShown()) {load();m_service->visible(true);}
}
void AssetsWorkspace::set_project_available(bool available) {
    m_project=available;if(m_service) m_service->set_project_available(available);
}
void AssetsWorkspace::send(const nlohmann::json& value) {
    if(!m_web || !m_loaded || m_web->GetCurrentURL()!=installed_url()) return;
    m_web->RunScript("window.modelLibraryReceive("+wxString::FromUTF8(value.dump())+");");
}
void AssetsWorkspace::load() {
    if(m_web || m_fallback || !m_service) return;
    m_web=WebView::CreateWebView(this,"about:blank");
    if(!m_web) {
        m_fallback=new ModelLibraryPanel(this,m_local);GetSizer()->Add(m_fallback,1,wxEXPAND);Layout();return;
    }
    m_web->Hide();m_web->SetBackgroundColour(RedesignTheme::background_colour());GetSizer()->Add(m_web,1,wxEXPAND);
    m_web->EnableContextMenu(false);
    m_web->Bind(wxEVT_WEBVIEW_NAVIGATING,[](wxWebViewEvent& event) {
        if(event.GetURL()!=installed_url() && event.GetURL()!="about:blank") event.Veto();
    });
    m_web->Bind(wxEVT_WEBVIEW_NEWWINDOW,[](wxWebViewEvent& event){event.Veto();});
    m_web->Bind(wxEVT_WEBVIEW_ERROR,[this](wxWebViewEvent&) {
        if (m_fallback) return;
        m_loaded=false;
        if (m_service) m_service->visible(false);
        m_web->Hide();
        m_fallback=new ModelLibraryPanel(this,m_local);
        GetSizer()->Add(m_fallback,1,wxEXPAND);Layout();
    });
    m_web->Bind(wxEVT_WEBVIEW_LOADED,[this](wxWebViewEvent& event) {
        if(event.GetURL()!=installed_url() || m_fallback) return;m_loaded=true;m_web->Show();Layout();
        send({{"type","init"},{"nonce",m_nonce}});m_service->ready();
    });
    m_web->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED,[this](wxWebViewEvent& event) {
        if(!m_loaded || !IsShownOnScreen() || m_web->GetCurrentURL()!=installed_url()) return;
        const auto text=std::string(event.GetString().utf8_string());if(text.size()>8192) return;
        auto value=nlohmann::json::parse(text,nullptr,false);
        if(!value.is_object() || !value.contains("nonce") || value["nonce"]!=m_nonce) return;
        // Commands carry opaque registered identities; the service never accepts a URL or local path.
        for(const auto& field:value.items())
            if(field.key()!="nonce" && field.key()!="command" && field.key()!="scope" && field.key()!="filter" &&
               field.key()!="query" && field.key()!="key" && field.key()!="revision" && field.key()!="action" &&
               field.key()!="file" && field.key()!="kind" && field.key()!="active") return;
        m_service->command(value);
    });
    WebView::LoadUrl(m_web,installed_url());
}
}
