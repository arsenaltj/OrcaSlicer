#pragma once

#include <wx/panel.h>
#include "ModelGalleryPolicy.hpp"
#include "RedesignTheme.hpp"
#include "../AI/ModelGeneration/WorkbenchStyle.hpp"
#include "../GUI.hpp"
#include "../I18N.hpp"
#include "../Widgets/WebView.hpp"

#include <wx/filename.h>
#include <wx/filesys.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/uri.h>
#include <wx/utils.h>

namespace Slic3r::GUI {

class ModelGalleryWorkspace final : public wxPanel
{
public:
    explicit ModelGalleryWorkspace(wxWindow* parent);

private:
    void load_gallery();
    void open_browser(const wxString& url);

    wxStaticText* m_status {nullptr};
    wxWebView* m_web {nullptr};
};

namespace ModelGalleryDetail {
inline const wxString gallery_url = "https://arsenaltj.github.io/";

inline wxString installed_gallery_url()
{
    return wxFileSystem::FileNameToURL(wxFileName(from_u8(resources_dir()) + "/web/model_gallery/index.html"));
}

inline GalleryNavigation navigation_for(const wxString& url)
{
    const wxURI uri(url);
    return gallery_navigation(into_u8(uri.GetScheme().Lower()), into_u8(uri.GetServer().Lower()), url == installed_gallery_url());
}
}

inline ModelGalleryWorkspace::ModelGalleryWorkspace(wxWindow* parent) : wxPanel(parent)
{
    SetBackgroundColour(RedesignTheme::background_colour());
    auto* root = new wxBoxSizer(wxVERTICAL);
    SetSizer(root);
    auto* controls = new wxBoxSizer(wxHORIZONTAL);
    m_status = new wxStaticText(this, wxID_ANY,
        _L("预览可离线浏览；下载后可通过“文件”菜单导入模型。"));
    RedesignTheme::style_text(m_status, RedesignTheme::secondary_text_colour(), 9);
    controls->Add(m_status, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    auto* refresh = workbench_button(this, _L("刷新"));
    auto* browser = workbench_button(this, _L("浏览器打开"));
    controls->Add(refresh, 0, wxRIGHT, FromDIP(8));
    controls->Add(browser, 0);
    root->Add(controls, 0, wxEXPAND | wxALL, FromDIP(12));

    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { load_gallery(); });
    browser->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_browser(ModelGalleryDetail::gallery_url); });
    // Delay WebView creation until the user opens Assets.
    Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        if (event.IsShown() && !m_web) load_gallery();
        event.Skip();
    });
}

inline void ModelGalleryWorkspace::load_gallery()
{
    m_status->SetLabel(_L("正在加载模型图库…"));
    if (!m_web) {
        m_web = WebView::CreateWebView(this, "about:blank");
        if (!m_web) {
            m_status->SetLabel(_L("网页组件不可用，请用“浏览器打开”。"));
            Layout();
            return;
        }
        GetSizer()->Add(m_web, 1, wxEXPAND);
        // Only the installed entry page stays embedded. External links have no
        // application command or filesystem bridge and open in the browser.
        m_web->Bind(wxEVT_WEBVIEW_NAVIGATING, [this](wxWebViewEvent& event) {
            if (event.GetURL() == "about:blank") return;
            const auto action = ModelGalleryDetail::navigation_for(event.GetURL());
            if (action == GalleryNavigation::Embedded) return;
            event.Veto();
            if (action == GalleryNavigation::Browser) {
                open_browser(event.GetURL());
                m_status->SetLabel(_L("下载已交给浏览器；完成后通过“文件”菜单导入模型。"));
            }
        });
        m_web->Bind(wxEVT_WEBVIEW_NEWWINDOW, [this](wxWebViewEvent& event) {
            event.Veto();
            if (ModelGalleryDetail::navigation_for(event.GetURL()) != GalleryNavigation::Blocked) open_browser(event.GetURL());
        });
        m_web->Bind(wxEVT_WEBVIEW_LOADED, [this](wxWebViewEvent&) {
            m_status->SetLabel(_L("预览可离线浏览；下载后可通过“文件”菜单导入模型。"));
            Layout();
        });
        m_web->Bind(wxEVT_WEBVIEW_ERROR, [this](wxWebViewEvent&) {
            m_status->SetLabel(_L("模型图库暂时无法加载。可刷新重试或在浏览器打开。"));
            Layout();
        });
        Layout();
    }
    WebView::LoadUrl(m_web, ModelGalleryDetail::installed_gallery_url());
}

inline void ModelGalleryWorkspace::open_browser(const wxString& url)
{
    if (ModelGalleryDetail::navigation_for(url) == GalleryNavigation::Blocked) return;
    if (!wxLaunchDefaultBrowser(url))
        wxMessageBox(_L("无法打开浏览器，请检查默认浏览器设置。"), _L("模型图库"), wxOK | wxICON_INFORMATION, this);
}

} // namespace Slic3r::GUI
