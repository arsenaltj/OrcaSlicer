#pragma once

#include <boost/filesystem/path.hpp>

#include <array>
#include <cstdint>
#include <string>

#include <wx/image.h>

#include <wx/panel.h>
#include <wx/timer.h>

class wxBoxSizer;
class wxButton;
class wxPanel;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;
class wxWindow;

namespace Slic3r::GUI {

class UploadThumbnail;
class ImagePreview;

class RedesignShell final : public wxPanel
{
public:
    enum class Page { Assets, Image, Model, Print };

    explicit RedesignShell(wxWindow* parent);

    bool navigate_to(Page page);
    bool navigate_to_tab(const wxString& id);
    wxString active_tab_id() const;
    void set_service_status(bool compatible, bool model_generation_available);

private:
    enum class ImageState { Empty, Loading, Ready, Failed };

    void build_image_workspace();
    wxPanel* create_placeholder_page(const wxString& title, const wxString& body);
    void choose_image();
    void accept_image(const wxString& path);
    void clear_image();
    void update_image_state();
    void update_preview_bitmap();
    void bind_upload_click(wxWindow* window);

    wxBoxSizer* m_sizer { nullptr };
    wxPanel* m_content_host { nullptr };
    wxPanel* m_image_page { nullptr };
    wxPanel* m_image_settings_panel { nullptr };
    wxPanel* m_upload_surface { nullptr };
    wxStaticText* m_upload_icon { nullptr };
    wxButton* m_generate_button { nullptr };
    wxStaticText* m_sidecar_status { nullptr };
    wxPanel* m_style_choice { nullptr };
    std::string m_selected_style_id { "sculpture" };
    wxStaticText* m_upload_hint { nullptr };
    wxStaticText* m_upload_status { nullptr };
    wxStaticText* m_upload_filename { nullptr };
    UploadThumbnail* m_upload_thumbnail { nullptr };
    wxTextCtrl* m_prompt { nullptr };
    wxPanel* m_guide_panel { nullptr };
    wxPanel* m_preview_host { nullptr };
    ImagePreview* m_preview { nullptr };
    std::array<wxPanel*, 4> m_nav_markers { nullptr, nullptr, nullptr, nullptr };
    std::array<wxStaticText*, 4> m_nav_labels { nullptr, nullptr, nullptr, nullptr };
    std::array<wxPanel*, 4> m_pages { nullptr, nullptr, nullptr, nullptr };
    wxImage m_selected_image;
    boost::filesystem::path m_selected_image_path;
    wxSize m_last_preview_bounds;
    wxTimer m_preview_resize_timer;
    std::uint64_t m_image_request_generation { 0 };
    Page m_active_page { Page::Image };
    // Keep the semantic workspace request, not only the visual host. Several
    // legacy requests (Prepare, Preview, Monitor and Multi-device) currently
    // share the Print host while their business meaning still differs.
    wxString m_active_tab_id;
    ImageState m_image_state { ImageState::Empty };
};

}
