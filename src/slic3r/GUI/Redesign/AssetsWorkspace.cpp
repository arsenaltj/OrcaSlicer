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
#include <wx/uri.h>

namespace Slic3r::GUI {
namespace {
wxString installed_url() {
    return wxFileSystem::FileNameToURL(wxFileName(from_u8(resources_dir())+"/web/model_library/index.html"));
}
bool installed_document(const wxString& url) {
    const wxURI uri(url);
    if (uri.GetScheme() != "file" || !uri.GetServer().empty() || uri.HasQuery() || uri.HasFragment()) return false;
    auto actual = wxFileSystem::URLToFileName(url);
    auto expected = wxFileSystem::URLToFileName(installed_url());
    const int flags = wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE;
    return actual.Normalize(flags) && expected.Normalize(flags) && actual.SameAs(expected);
}
}
AssetsWorkspace::AssetsWorkspace(wxWindow* parent)
    :wxPanel(parent)
{
    SetBackgroundColour(RedesignTheme::background_colour());SetSizer(new wxBoxSizer(wxVERTICAL));
    m_messages.configure([this] { return m_loaded && m_web && IsShownOnScreen(); },
        [this](Presentation::Dirty) { flush_messages(); }, "library-html");
    Bind(wxEVT_SHOW,[this](wxShowEvent& event) {
        if(event.GetEventObject()==this) set_presented(event.IsShown());
        event.Skip();
    });
}
AssetsWorkspace::~AssetsWorkspace() {
    m_messages.stop();
    if(m_service) {m_service->subscribe({});m_service->visible(false);}
}
void AssetsWorkspace::attach(std::shared_ptr<LibraryWorkspaceService> service,std::shared_ptr<ModelLibraryApplication> local) {
    m_service=std::move(service);m_local=std::move(local);
    m_service->subscribe([this](const nlohmann::json& value){send(value);});
    m_service->set_project_available(m_project);
    if(IsShownOnScreen()) set_presented(true);
}
void AssetsWorkspace::set_presented(bool value) {
    Presentation::assert_gui_thread();
    m_presented=value;
    if(value) load();
    if(m_service && !m_fallback) m_service->visible(value);
    if(value) {
        if(m_loaded && !m_channel.ready()) send_initialization();
        m_messages.resume();
    }
}
void AssetsWorkspace::set_project_available(bool available) {
    m_project=available;if(m_service) m_service->set_project_available(available);
}
void AssetsWorkspace::send(const nlohmann::json& value) {
    Presentation::assert_gui_thread();
    if(!m_web || !m_loaded || !installed_document(m_web->GetCurrentURL())) return;
    m_channel.push(value);
    // Account/geometry/control messages keep their order. Inactive must reach
    // WebGL even after wx has hidden the host, to stop its animation loop.
    if (value.value("type",std::string()) == "inactive") {
        flush_messages();
        return;
    }
    m_messages.request(Presentation::Dirty::Content);
}
void AssetsWorkspace::flush_messages() {
    Presentation::assert_gui_thread();
    if (!m_loaded || !m_web || !installed_document(m_web->GetCurrentURL())) return;
    if (!m_channel.ready()) { send_initialization();return; }
    auto batch = m_channel.take();
    if (batch.empty()) return;
    // The shared adapter submits without waiting. wxWebView::RunScript pumps
    // wxYield and can deadlock when inactive is sent from a show/freeze event.
    if (!WebView::RunScript(m_web,"window.modelLibraryReceiveBatch("+wxString::FromUTF8(nlohmann::json(batch).dump())+");")) {
        m_channel.retry(std::move(batch));
        wxLogWarning("模型库页面消息提交失败，请重新进入模型库。");
    }
}
void AssetsWorkspace::send_initialization() {
    Presentation::assert_gui_thread();
    if(!m_loaded || !m_web || m_channel.ready() || !m_initialization.is_object() ||
        !installed_document(m_web->GetCurrentURL())) return;
    // Retain the same control message until its matching ready response. An
    // explicit re-presentation can recover a failed script submission without
    // creating a new document/account identity or reloading the whole page.
    if(!WebView::RunScript(m_web,"window.modelLibraryReceive("+wxString::FromUTF8(m_initialization.dump())+");"))
        wxLogWarning("模型库页面初始化失败，请重新进入模型库。");
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
        if(!installed_document(event.GetURL()) && event.GetURL()!="about:blank") event.Veto();
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
        if(!installed_document(event.GetURL()) || m_fallback) return;
        m_service->ready(false);m_loaded=true;
        m_initialization=m_channel.begin(HonorAccount::random_value());
        // Handshake control bypasses the account-data batch.
        send_initialization();
    });
    m_web->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED,[this](wxWebViewEvent& event) {
        if(!m_loaded || !installed_document(m_web->GetCurrentURL())) return;
        const auto text=std::string(event.GetString().utf8_string());if(text.size()>8192) return;
        auto value=nlohmann::json::parse(text,nullptr,false);
        if(!value.is_object()) return;
        // Commands carry opaque registered identities; the service never accepts a URL or local path.
        for(const auto& field:value.items())
            if(field.key()!="nonce" && field.key()!="command" && field.key()!="scope" && field.key()!="filter" &&
               field.key()!="query" && field.key()!="key" && field.key()!="revision" && field.key()!="action" &&
               field.key()!="file" && field.key()!="kind" && field.key()!="active" &&
               field.key()!="documentGeneration" && field.key()!="accountTag" &&
               field.key()!="accountGeneration" && field.key()!="requestGeneration") return;
        try {
            const auto accepted=m_channel.accept(value);
            if(accepted==LibraryPresentationChannel::Acceptance::Ready) {
                m_web->Show();Layout();m_service->ready();m_messages.resume();
            } else if(accepted==LibraryPresentationChannel::Acceptance::Command && m_presented && IsShownOnScreen())
                m_service->command(value);
        } catch(const nlohmann::json::exception&) { /* Reject malformed internal messages. */ }
    });
    WebView::LoadUrl(m_web,installed_url());
}
}
