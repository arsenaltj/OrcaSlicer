#pragma once

#include <wx/panel.h>
#include <functional>

class wxStaticText;
class wxWebView;

namespace Slic3r::GUI {

class AssetsWorkspace final : public wxPanel
{
public:
    AssetsWorkspace(wxWindow* parent, std::function<void()> save_project);
    wxWindow* local_assets_host() const { return m_local; }
    void set_project_available(bool available);

private:
    void select_gallery(bool gallery);
    void load_gallery();
    void open_storage();
    void open_browser(const wxString& url);

    wxPanel* m_local {nullptr};
    wxPanel* m_gallery {nullptr};
    wxWindow* m_local_tab {nullptr};
    wxWindow* m_gallery_tab {nullptr};
    wxWindow* m_save_project {nullptr};
    wxStaticText* m_gallery_status {nullptr};
    wxWebView* m_web {nullptr};
};

} // namespace Slic3r::GUI
