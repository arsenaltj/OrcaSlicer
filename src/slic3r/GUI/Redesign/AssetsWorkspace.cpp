#include "AssetsWorkspace.hpp"
#include "AssetsWorkspacePolicy.hpp"
#include "RedesignTheme.hpp"
#include "../AI/ModelGeneration/WorkbenchStyle.hpp"
#include "../AIModelOutputDirectory.hpp"
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
namespace {
const wxString gallery_url = "https://arsenaltj.github.io/";

wxString installed_gallery_url()
{
    return wxFileSystem::FileNameToURL(wxFileName(from_u8(resources_dir()) + "/web/model_gallery/index.html"));
}

GalleryNavigation navigation_for(const wxString& url)
{
    const wxURI uri(url);
    return gallery_navigation(into_u8(uri.GetScheme().Lower()), into_u8(uri.GetServer().Lower()), url == installed_gallery_url());
}
}

AssetsWorkspace::AssetsWorkspace(wxWindow* parent, std::function<void()> save_project) : wxPanel(parent)
{
    SetBackgroundColour(RedesignTheme::background_colour());
    auto* root = new wxBoxSizer(wxVERTICAL);
    SetSizer(root);
    auto* toolbar = new wxBoxSizer(wxHORIZONTAL);
    m_local_tab = workbench_button(this, _L("我的资产"));
    m_gallery_tab = workbench_button(this, _L("模型图库"));
    toolbar->Add(m_local_tab, 0, wxRIGHT, FromDIP(8));
    toolbar->Add(m_gallery_tab, 0, wxRIGHT, FromDIP(8));
    toolbar->AddStretchSpacer();
    auto* storage = workbench_button(this, _L("打开模型文件夹"));
    storage->SetToolTip(from_path(ai_model_output_directory().root()));
    toolbar->Add(storage, 0, wxRIGHT, FromDIP(8));
    m_save_project = workbench_button(this, _L("保存当前工程（3MF）"));
    m_save_project->SetToolTip(_L("保存已导入工程的模型、耗材颜色、摆放及打印参数。美颜草稿请先保存并导入；G-code 需切片后单独导出。"));
    toolbar->Add(m_save_project, 0);
    root->Add(toolbar, 0, wxEXPAND | wxALL, FromDIP(12));
    set_project_available(false);

    m_local = new wxPanel(this);
    m_local->SetBackgroundColour(RedesignTheme::background_colour());
    m_local->SetSizer(new wxBoxSizer(wxVERTICAL));
    root->Add(m_local, 1, wxEXPAND);
    m_gallery = new wxPanel(this);
    m_gallery->SetBackgroundColour(RedesignTheme::background_colour());
    auto* gallery = new wxBoxSizer(wxVERTICAL);
    m_gallery->SetSizer(gallery);
    auto* controls = new wxBoxSizer(wxHORIZONTAL);
    m_gallery_status = new wxStaticText(m_gallery, wxID_ANY,
        _L("预览可离线浏览；下载完成后，在“我的资产”导入模型。"));
    RedesignTheme::style_text(m_gallery_status, RedesignTheme::secondary_text_colour(), 9);
    controls->Add(m_gallery_status, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    auto* refresh = workbench_button(m_gallery, _L("刷新"));
    auto* browser = workbench_button(m_gallery, _L("浏览器打开"));
    controls->Add(refresh, 0, wxRIGHT, FromDIP(8));
    controls->Add(browser, 0);
    gallery->Add(controls, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(m_gallery, 1, wxEXPAND);
    m_gallery->Hide();

    m_local_tab->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { select_gallery(false); });
    m_gallery_tab->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { select_gallery(true); });
    storage->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_storage(); });
    m_save_project->Bind(wxEVT_BUTTON, [save_project = std::move(save_project)](wxCommandEvent&) {
        if (save_project) save_project();
    });
    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { load_gallery(); });
    browser->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_browser(gallery_url); });
    select_gallery(false);
}

void AssetsWorkspace::set_project_available(bool available)
{
    m_save_project->Enable(available);
}

void AssetsWorkspace::select_gallery(bool gallery)
{
    m_local->Show(!gallery);
    m_gallery->Show(gallery);
    // Disable the active tab so the selection is also visible to keyboard users.
    m_local_tab->Enable(gallery);
    m_gallery_tab->Enable(!gallery);
    Layout();
    if (gallery && !m_web) load_gallery();
}

void AssetsWorkspace::load_gallery()
{
    m_gallery_status->SetLabel(_L("正在加载模型图库…"));
    if (!m_web) {
        m_web = WebView::CreateWebView(m_gallery, "about:blank");
        if (!m_web) {
            m_gallery_status->SetLabel(_L("网页组件不可用，请用“浏览器打开”。"));
            m_gallery->Layout();
            return;
        }
        m_web->SetBackgroundColour(RedesignTheme::background_colour());
        // Keep the browser's initial white about:blank surface out of the UI.
        // The surrounding panel and loading status remain visible until ready.
        m_web->Hide();
        m_gallery->GetSizer()->Add(m_web, 1, wxEXPAND);
        // Only the installed entry page stays embedded. External links have no
        // application command or filesystem bridge and open in the browser.
        m_web->Bind(wxEVT_WEBVIEW_NAVIGATING, [this](wxWebViewEvent& event) {
            if (event.GetURL() == "about:blank") return;
            const auto action = navigation_for(event.GetURL());
            if (action == GalleryNavigation::Embedded) return;
            event.Veto();
            if (action == GalleryNavigation::Browser) {
                open_browser(event.GetURL());
                m_gallery_status->SetLabel(_L("下载已交给浏览器；完成后回“我的资产”导入模型。"));
            }
        });
        m_web->Bind(wxEVT_WEBVIEW_NEWWINDOW, [this](wxWebViewEvent& event) {
            event.Veto();
            if (navigation_for(event.GetURL()) != GalleryNavigation::Blocked) open_browser(event.GetURL());
        });
        m_web->Bind(wxEVT_WEBVIEW_LOADED, [this](wxWebViewEvent& event) {
            if (event.GetURL() == "about:blank" ||
                navigation_for(event.GetURL()) != GalleryNavigation::Embedded) return;
            m_gallery_status->SetLabel(_L("预览可离线浏览；下载完成后，在“我的资产”导入模型。"));
            m_web->Show();
            m_gallery->Layout();
        });
        m_web->Bind(wxEVT_WEBVIEW_ERROR, [this](wxWebViewEvent&) {
            m_web->Hide();
            m_gallery_status->SetLabel(_L("模型图库暂时无法加载。可刷新重试或在浏览器打开；本地资产仍可使用。"));
            m_gallery->Layout();
        });
        m_gallery->Layout();
    }
    m_web->Hide();
    m_gallery->Layout();
    WebView::LoadUrl(m_web, installed_gallery_url());
}

void AssetsWorkspace::open_browser(const wxString& url)
{
    if (navigation_for(url) == GalleryNavigation::Blocked) return;
    if (!wxLaunchDefaultBrowser(url))
        wxMessageBox(_L("无法打开浏览器，请检查默认浏览器设置。"), _L("模型图库"), wxOK | wxICON_INFORMATION, this);
}

void AssetsWorkspace::open_storage()
{
    const auto& root = ai_model_output_directory().root();
    boost::system::error_code ec;
    boost::filesystem::create_directories(root, ec);
    wxString path = from_path(root);
    if (!path.empty() && !wxFileName::IsPathSeparator(path.Last())) path += wxFileName::GetPathSeparator();
    if (ec || !wxLaunchDefaultApplication(path))
        wxMessageBox(_L("无法打开模型文件夹：") + from_path(root), _L("模型文件夹"), wxOK | wxICON_ERROR, this);
}

} // namespace Slic3r::GUI
