#include "RedesignShell.hpp"
#include "../MainFrame.hpp"
#include "../GUI_App.hpp"
#include "../AI/ModelGeneration/ModelGenerationPresentation.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <boost/log/trivial.hpp>

#include <wx/dnd.h>
#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/colour.h>
#include <wx/font.h>
#include <wx/fontenum.h>
#include <wx/image.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/window.h>

#include "libslic3r/Utils.hpp"

namespace Slic3r::GUI {
namespace {

wxColour background_colour()
{
    return wxColour(49, 49, 53);
}

wxColour panel_colour()
{
    return wxColour(35, 35, 38);
}

wxColour control_colour()
{
    return wxColour(25, 25, 27);
}

wxColour primary_text_colour()
{
    return wxColour(255, 255, 255, 220);
}

wxColour secondary_text_colour()
{
    return wxColour(255, 255, 255, 150);
}

wxString text(const char* value)
{
    return wxString::FromUTF8(value);
}

constexpr const char* kRedesignAssetsTabId = "REDESIGN_ASSETS";

void style_text(wxWindow* window, const wxColour& colour, int point_size, bool bold = false)
{
    window->SetForegroundColour(colour);
    const wxString family = wxFontEnumerator::IsValidFacename("HONOR Sans Design") ? "HONOR Sans Design" :
                            wxFontEnumerator::IsValidFacename("HarmonyOS Sans SC") ? "HarmonyOS Sans SC" :
                            wxFontEnumerator::IsValidFacename("Microsoft YaHei UI") ? "Microsoft YaHei UI" :
                            wxString();
    wxFontInfo font(point_size);
    font.Family(wxFONTFAMILY_SWISS).Bold(bold);
    if (!family.empty())
        font.FaceName(family);
    window->SetFont(wxFont(font));
}

wxBitmap scaled_bitmap(const wxImage& image, const wxSize& bounds)
{
    if (!image.IsOk() || bounds.x <= 0 || bounds.y <= 0)
        return wxNullBitmap;
    const double scale = std::min(1.0, std::min(double(bounds.x) / image.GetWidth(), double(bounds.y) / image.GetHeight()));
    return wxBitmap(image.Scale(std::max(1, int(std::round(image.GetWidth() * scale))),
                                std::max(1, int(std::round(image.GetHeight() * scale))), wxIMAGE_QUALITY_HIGH));
}

wxBitmap resource_bitmap(const char* name, const wxSize& bounds)
{
    return scaled_bitmap(wxImage(wxString::FromUTF8((Slic3r::resources_dir() + "/images/" + name).c_str())), bounds);
}

// Paint the surrounding colour in the corners; native wxPanels are square.
class RoundedPanel final : public wxPanel {
public:
    RoundedPanel(wxWindow* parent, const wxSize& size, const wxColour& face,
                 const wxColour& surrounding, int radius)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , m_face(face), m_surrounding(surrounding), m_radius(radius)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(surrounding);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(m_surrounding));
            dc.Clear();
            const wxSize size = GetClientSize();
            if (size.x <= 0 || size.y <= 0) return;
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
            if (!gc) return;
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(m_face));
            gc->DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(m_radius));
        });
    }
private:
    wxColour m_face, m_surrounding;
    int m_radius;
};

class ImageDropTarget final : public wxFileDropTarget
{
public:
    explicit ImageDropTarget(std::function<void(const wxString&)> accept) : m_accept(std::move(accept)) {}

    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& files) override
    {
        if (files.size() != 1)
            return false;
        m_accept(files[0]);
        return true;
    }

private:
    std::function<void(const wxString&)> m_accept;
};

wxStaticText* label(wxWindow* parent, const char* value, int size, bool bold = false)
{
    auto* result = new wxStaticText(parent, wxID_ANY, text(value));
    style_text(result, primary_text_colour(), size, bold);
    return result;
}

}

RedesignShell::RedesignShell(wxWindow* parent)
    : wxPanel(parent)
    , m_sizer(new wxBoxSizer(wxVERTICAL))
    , m_preview_resize_timer(this)
{
    SetBackgroundColour(background_colour());
    SetSizer(m_sizer);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { update_preview_bitmap(); }, m_preview_resize_timer.GetId());
    build_image_workspace();
}

void RedesignShell::build_image_workspace()
{
    auto* workspace = new wxBoxSizer(wxHORIZONTAL);
    m_sizer->Add(workspace, 1, wxEXPAND | wxALL, FromDIP(16));

    auto* navigation = new RoundedPanel(this, wxDefaultSize, wxColour(33, 33, 35), background_colour(), 12);
    navigation->SetMinSize(wxSize(FromDIP(92), -1));
    auto* navigation_sizer = new wxBoxSizer(wxVERTICAL);
    navigation->SetSizer(navigation_sizer);
    workspace->Add(navigation, 0, wxEXPAND | wxRIGHT, FromDIP(12));

    auto* logo = new wxStaticBitmap(navigation, wxID_ANY,
                                    resource_bitmap("redesign_logo.png", wxSize(FromDIP(48), FromDIP(48))));
    navigation_sizer->Add(logo, 0, wxALIGN_CENTER | wxTOP, FromDIP(16));
    navigation_sizer->AddSpacer(FromDIP(78));

    const std::array<const char*, 4> navigation_items = {"资产", "图像", "3D模型", "打印"};
    const std::array<const char*, 4> navigation_icons = {"redesign_nav_assets.png", "redesign_nav_image.png",
                                                          "redesign_nav_model.png", "redesign_nav_print.png"};
    for (std::size_t index = 0; index < navigation_items.size(); ++index) {
        auto* item = new wxPanel(navigation, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(70)));
        item->SetMinSize(wxSize(-1, FromDIP(70)));
        item->SetBackgroundColour(wxColour(33, 33, 35));
        auto* item_sizer = new wxBoxSizer(wxVERTICAL);
        item->SetSizer(item_sizer);

        m_nav_markers[index] = new wxPanel(item, wxID_ANY, wxPoint(0, 0), wxSize(FromDIP(3), FromDIP(70)));
        m_nav_markers[index]->SetBackgroundColour(wxColour(255, 194, 39));
        m_nav_markers[index]->Hide();

        m_nav_labels[index] = new wxStaticText(item, wxID_ANY, text(navigation_items[index]),
                                               wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
        auto* item_icon = new wxStaticBitmap(item, wxID_ANY,
                                             resource_bitmap(navigation_icons[index], wxSize(FromDIP(24), FromDIP(24))));
        item_sizer->Add(item_icon, 0, wxALIGN_CENTER | wxTOP, FromDIP(12));
        style_text(m_nav_labels[index], secondary_text_colour(), 9);
        item_sizer->Add(m_nav_labels[index], 0, wxALIGN_CENTER | wxTOP, FromDIP(4));
        navigation_sizer->Add(item, 0, wxEXPAND | wxBOTTOM, FromDIP(24));

        const Page page = static_cast<Page>(index);
        auto navigate = [this, page](wxMouseEvent&) { navigate_to(page); };
        item->Bind(wxEVT_LEFT_UP, navigate);
        m_nav_labels[index]->Bind(wxEVT_LEFT_UP, navigate);
        item_icon->Bind(wxEVT_LEFT_UP, navigate);
        m_nav_markers[index]->Bind(wxEVT_LEFT_UP, navigate);
    }
    navigation_sizer->AddStretchSpacer(1);
    const std::array<const char*, 4> footer_icons = {"redesign_avatar.png", "redesign_nav_notification.png",
                                                      "redesign_nav_settings.png", "redesign_nav_help.png"};
    for (std::size_t index = 0; index < footer_icons.size(); ++index) {
        auto* item = new wxStaticBitmap(navigation, wxID_ANY,
                                        resource_bitmap(footer_icons[index], wxSize(FromDIP(32), FromDIP(32))));
        navigation_sizer->Add(item, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(index == 3 ? 16 : 38));
    }

    m_image_settings_panel = new RoundedPanel(this, wxDefaultSize, panel_colour(), background_colour(), 12);
    auto* settings_panel = m_image_settings_panel;
    settings_panel->SetBackgroundColour(panel_colour());
    settings_panel->SetMinSize(wxSize(FromDIP(373), -1));
    auto* settings_sizer = new wxBoxSizer(wxVERTICAL);
    settings_panel->SetSizer(settings_sizer);
    workspace->Add(settings_panel, 0, wxEXPAND | wxRIGHT, FromDIP(16));

    auto* heading = new wxStaticText(settings_panel, wxID_ANY, text("上传图片"));
    style_text(heading, primary_text_colour(), 13, true);
    settings_sizer->Add(heading, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(21));

    m_upload_surface = new RoundedPanel(settings_panel, wxSize(-1, FromDIP(160)), control_colour(), panel_colour(), 8);
    m_upload_surface->SetMinSize(wxSize(-1, FromDIP(160)));
    m_upload_surface->SetBackgroundColour(control_colour());
    auto* upload_sizer = new wxBoxSizer(wxVERTICAL);
    m_upload_surface->SetSizer(upload_sizer);
    upload_sizer->AddStretchSpacer(1);
    auto* upload_tile = new RoundedPanel(m_upload_surface, wxSize(FromDIP(68), FromDIP(68)),
                                         background_colour(), control_colour(), 6);
    upload_tile->SetMinSize(wxSize(FromDIP(68), FromDIP(68)));
    auto* upload_tile_sizer = new wxBoxSizer(wxVERTICAL);
    upload_tile->SetSizer(upload_tile_sizer);
    m_upload_icon = new wxStaticText(upload_tile, wxID_ANY, text("+"), wxDefaultPosition,
                                     wxSize(FromDIP(52), FromDIP(52)), wxALIGN_CENTER);
    m_upload_icon->SetBackgroundColour(background_colour());
    style_text(m_upload_icon, primary_text_colour(), 26);
    upload_tile_sizer->Add(m_upload_icon, 1, wxALIGN_CENTER | wxALL, FromDIP(8));
    upload_sizer->Add(upload_tile, 0, wxALIGN_CENTER);
    m_upload_thumbnail = new wxStaticBitmap(m_upload_surface, wxID_ANY, wxNullBitmap);
    upload_sizer->Add(m_upload_thumbnail, 0, wxALIGN_CENTER);
    m_remove_image = new wxButton(m_upload_surface, wxID_ANY, text("×"), wxDefaultPosition,
                                  wxSize(FromDIP(28), FromDIP(24)), wxBORDER_NONE);
    m_remove_image->SetBackgroundColour(wxColour(75, 75, 78));
    style_text(m_remove_image, primary_text_colour(), 12);
    m_remove_image->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { clear_image(); });
    m_upload_status = label(m_upload_surface, "点击、拖拽选择图片", 10);
    upload_sizer->Add(m_upload_status, 0, wxALIGN_CENTER | wxTOP, FromDIP(7));
    m_upload_filename = label(m_upload_surface, "", 9);
    style_text(m_upload_filename, secondary_text_colour(), 9);
    upload_sizer->Add(m_upload_filename, 0, wxALIGN_CENTER | wxTOP, FromDIP(2));
    m_upload_hint = label(m_upload_surface, "支持：PNG、JPG、JPEG，最大 20MB", 8);
    style_text(m_upload_hint, secondary_text_colour(), 8);
    upload_sizer->Add(m_upload_hint, 0, wxALIGN_CENTER | wxBOTTOM | wxTOP, FromDIP(2));
    upload_sizer->AddStretchSpacer(1);
    settings_sizer->Add(m_upload_surface, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    const std::array<wxWindow*, 7> upload_targets = {m_upload_surface, upload_tile, m_upload_icon, m_upload_status,
                                                      m_upload_hint, m_upload_filename, m_upload_thumbnail};
    for (wxWindow* target : upload_targets) {
        target->SetDropTarget(new ImageDropTarget([this](const wxString& path) { accept_image(path); }));
        bind_upload_click(target);
    }

    auto* prompt_label = new wxStaticText(settings_panel, wxID_ANY, text("描述"));
    style_text(prompt_label, primary_text_colour(), 13, true);
    settings_sizer->Add(prompt_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    auto* prompt_surface = new RoundedPanel(settings_panel, wxDefaultSize, control_colour(), panel_colour(), 8);
    prompt_surface->SetBackgroundColour(control_colour());
    auto* prompt_sizer = new wxBoxSizer(wxVERTICAL);
    prompt_surface->SetSizer(prompt_sizer);
    m_prompt = new wxTextCtrl(prompt_surface, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              wxSize(-1, FromDIP(118)), wxTE_MULTILINE | wxBORDER_NONE);
    m_prompt->SetBackgroundColour(control_colour());
    m_prompt->SetMaxLength(800);
    style_text(m_prompt, wxColour(230, 230, 233), 10);
    // wxWidgets emulates hints for multiline controls and remembers the current text colour.
    m_prompt->SetHint(text("描述你想创作的内容，例如：一只可爱的小猫。"));
    prompt_sizer->Add(m_prompt, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    auto* prompt_count = label(prompt_surface, "0/800", 9);
    style_text(prompt_count, secondary_text_colour(), 9);
    prompt_sizer->Add(prompt_count, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, FromDIP(12));
    m_prompt->Bind(wxEVT_TEXT, [this, prompt_count](wxCommandEvent& event) {
        prompt_count->SetLabel(wxString::Format("%lu/800", static_cast<unsigned long>(m_prompt->GetValue().length())));
        event.Skip();
    });
    settings_sizer->Add(prompt_surface, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));

    auto* model_label = new wxStaticText(settings_panel, wxID_ANY, text("模型选择"));
    style_text(model_label, primary_text_colour(), 13, true);
    settings_sizer->Add(model_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    auto* model_surface = new RoundedPanel(settings_panel, wxSize(-1, FromDIP(56)), control_colour(), panel_colour(), 8);
    model_surface->SetBackgroundColour(control_colour());
    auto* model_sizer = new wxBoxSizer(wxHORIZONTAL);
    model_surface->SetSizer(model_sizer);
    auto* model_icon = new wxStaticBitmap(model_surface, wxID_ANY,
                                          resource_bitmap("redesign_gpt_image.png", wxSize(FromDIP(45), FromDIP(45))));
    model_sizer->Add(model_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    model_sizer->Add(label(model_surface, "GPT image", 10), 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    auto* model_arrow = label(model_surface, "⌄", 14);
    model_sizer->Add(model_arrow, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    settings_sizer->Add(model_surface, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));

    auto* skills_label = new wxStaticText(settings_panel, wxID_ANY, text("技能"));
    style_text(skills_label, primary_text_colour(), 13, true);
    settings_sizer->Add(skills_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    auto* skills_grid = new wxGridSizer(2, FromDIP(14), FromDIP(12));
    for (const char* skill : {"动漫手办", "建筑物模型", "Q版手办", "小组件", "能力5", "能力6"}) {
        auto* skill_card = new RoundedPanel(settings_panel, wxSize(-1, FromDIP(56)), control_colour(), panel_colour(), 8);
        skill_card->SetBackgroundColour(control_colour());
        auto* skill_sizer = new wxBoxSizer(wxHORIZONTAL);
        skill_card->SetSizer(skill_sizer);
        auto* skill_tile = new RoundedPanel(skill_card, wxSize(FromDIP(44), FromDIP(44)),
                                            background_colour(), control_colour(), 6);
        skill_tile->SetMinSize(wxSize(FromDIP(44), FromDIP(44)));
        auto* skill_tile_sizer = new wxBoxSizer(wxVERTICAL);
        skill_tile->SetSizer(skill_tile_sizer);
        auto* skill_icon = new wxStaticBitmap(skill_tile, wxID_ANY,
                                              resource_bitmap("redesign_skill_emoji.png", wxSize(FromDIP(24), FromDIP(24))));
        skill_tile_sizer->Add(skill_icon, 1, wxALIGN_CENTER | wxALL, FromDIP(10));
        skill_sizer->Add(skill_tile, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        skill_sizer->Add(label(skill_card, skill, 9), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
        skills_grid->Add(skill_card, 1, wxEXPAND);
    }
    settings_sizer->Add(skills_grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    settings_sizer->AddStretchSpacer(1);

    m_generate_button = new wxButton(settings_panel, wxID_ANY, text("生成 2D 设计图"),
                                     wxDefaultPosition, wxSize(-1, FromDIP(48)), wxBORDER_NONE);
    m_generate_button->SetBackgroundColour(wxColour(254, 212, 69));
    style_text(m_generate_button, wxColour(20, 20, 20), 11, true);
    m_generate_button->Enable(false);
    settings_sizer->Add(m_generate_button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    m_content_host = new wxPanel(this, wxID_ANY);
    m_content_host->SetBackgroundColour(background_colour());
    workspace->Add(m_content_host, 1, wxEXPAND);
    auto* content_host_sizer = new wxBoxSizer(wxVERTICAL);
    m_content_host->SetSizer(content_host_sizer);

    m_image_page = new wxPanel(m_content_host, wxID_ANY);
    m_image_page->SetBackgroundColour(background_colour());
    auto* content_sizer = new wxBoxSizer(wxVERTICAL);
    m_image_page->SetSizer(content_sizer);
    content_host_sizer->Add(m_image_page, 1, wxEXPAND);

    m_guide_panel = new wxPanel(m_image_page, wxID_ANY);
    m_guide_panel->SetBackgroundColour(background_colour());
    auto* guide_sizer = new wxBoxSizer(wxVERTICAL);
    m_guide_panel->SetSizer(guide_sizer);
    auto* title = label(m_guide_panel, "上传图片创建你的专属模型吧！", 24);
    guide_sizer->AddStretchSpacer(1);
    guide_sizer->Add(title, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(76));

    auto* flow = new wxBoxSizer(wxHORIZONTAL);
    auto add_step = [this, flow](const char* image_name, const char* heading_text, const char* subtitle_text) {
        auto* step = new wxBoxSizer(wxVERTICAL);
        if (image_name != nullptr) {
            auto* picture = new wxStaticBitmap(m_guide_panel, wxID_ANY,
                                               resource_bitmap(image_name, wxSize(FromDIP(138), FromDIP(168))));
            step->Add(picture, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(18));
        } else {
            auto* prompt_card = new RoundedPanel(m_guide_panel, wxSize(FromDIP(250), FromDIP(160)),
                                                 wxColour(38, 38, 41), background_colour(), 8);
            prompt_card->SetMinSize(wxSize(FromDIP(250), FromDIP(160)));
            prompt_card->SetBackgroundColour(wxColour(38, 38, 41));
            auto* prompt_sizer = new wxBoxSizer(wxVERTICAL);
            prompt_card->SetSizer(prompt_sizer);
            auto* sample = label(prompt_card, "生成一只可爱的小怪兽手办。", 9);
            style_text(sample, secondary_text_colour(), 9);
            prompt_sizer->Add(sample, 0, wxALL, FromDIP(10));
            step->Add(prompt_card, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(18));
        }
        auto* heading = label(m_guide_panel, heading_text, 11);
        step->Add(heading, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(4));
        auto* subtitle = label(m_guide_panel, subtitle_text, 9);
        style_text(subtitle, secondary_text_colour(), 9);
        step->Add(subtitle, 0, wxALIGN_CENTER);
        flow->Add(step, 0, wxALIGN_CENTER);
    };
    auto add_arrow = [this, flow] {
        auto* arrow = label(m_guide_panel, "➜", 30);
        style_text(arrow, wxColour(116, 198, 255), 30, true);
        flow->Add(arrow, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(28));
    };
    add_step(nullptr, "上传图片或输入提示词生成图片", "描述想创作的图片");
    add_arrow();
    add_step("redesign_model_blue.png", "生成图片", "生成图片并完善");
    add_arrow();
    add_step("redesign_model_mono.png", "转为 3D", "获得可打印的专属 3D 模型");
    guide_sizer->Add(flow, 0, wxALIGN_CENTER);
    guide_sizer->AddStretchSpacer(1);
    content_sizer->Add(m_guide_panel, 1, wxEXPAND);

    m_preview_host = new wxPanel(m_image_page, wxID_ANY);
    m_preview_host->SetBackgroundColour(background_colour());
    auto* preview_sizer = new wxBoxSizer(wxVERTICAL);
    m_preview_host->SetSizer(preview_sizer);
    preview_sizer->AddStretchSpacer(1);
    m_preview = new wxStaticBitmap(m_preview_host, wxID_ANY, wxNullBitmap);
    preview_sizer->Add(m_preview, 0, wxALIGN_CENTER);
    preview_sizer->AddStretchSpacer(1);
    content_sizer->Add(m_preview_host, 1, wxEXPAND);
    m_preview_host->Hide();
    m_image_page->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        if (m_image_state == ImageState::Ready)
            m_preview_resize_timer.StartOnce(150);
        event.Skip();
    });
    update_image_state();

    m_pages[static_cast<std::size_t>(Page::Assets)] =
        create_placeholder_page(text("资产中心"), text("新界面资产中心正在建设中。此页面不会跳回旧版工作区。"));
    m_pages[static_cast<std::size_t>(Page::Image)] = m_image_page;
    m_pages[static_cast<std::size_t>(Page::Model)] =
        create_placeholder_page(text("3D 模型"), text("新界面 3D 模型工作区正在建设中。现有模型业务状态将通过统一命令接入。"));
    m_pages[static_cast<std::size_t>(Page::Print)] =
        create_placeholder_page(text("准备与打印"), text("新界面准备与打印工作区正在建设中。旧版 Prepare/Preview 请求已在此界面内承接。"));
    navigate_to(Page::Image);
}

wxPanel* RedesignShell::create_placeholder_page(const wxString& title, const wxString& body)
{
    auto* page = new wxPanel(m_content_host, wxID_ANY);
    page->SetBackgroundColour(background_colour());
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    page->SetSizer(sizer);
    sizer->AddStretchSpacer(1);
    auto* heading = new wxStaticText(page, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    style_text(heading, primary_text_colour(), 24, true);
    sizer->Add(heading, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(18));
    auto* description = new wxStaticText(page, wxID_ANY, body, wxDefaultPosition,
                                         wxSize(FromDIP(460), -1), wxALIGN_CENTER);
    description->Wrap(FromDIP(460));
    style_text(description, secondary_text_colour(), 11);
    sizer->Add(description, 0, wxALIGN_CENTER);
    sizer->AddStretchSpacer(1);
    m_content_host->GetSizer()->Add(page, 1, wxEXPAND);
    page->Hide();
    return page;
}

bool RedesignShell::navigate_to(Page page)
{
    const std::size_t index = static_cast<std::size_t>(page);
    if (index >= m_pages.size() || m_pages[index] == nullptr)
        return false;

    m_active_page = page;
    switch (page) {
    case Page::Assets:
        m_active_tab_id = wxString::FromUTF8(kRedesignAssetsTabId);
        break;
    case Page::Image:
        m_active_tab_id = TAB_ID_HOME;
        break;
    case Page::Model:
        m_active_tab_id = TAB_ID_GENERATE_3D;
        break;
    case Page::Print:
        m_active_tab_id = TAB_ID_PREPARE;
        break;
    }
    for (std::size_t i = 0; i < m_pages.size(); ++i) {
        const bool active = i == index;
        if (m_pages[i] != nullptr)
            m_pages[i]->Show(active);
        if (m_nav_markers[i] != nullptr)
            m_nav_markers[i]->Show(active);
        if (m_nav_labels[i] != nullptr)
            style_text(m_nav_labels[i], active ? primary_text_colour() : secondary_text_colour(), 9);
    }
    if (m_image_settings_panel != nullptr)
        m_image_settings_panel->Show(page == Page::Image);

    if (m_content_host != nullptr)
        m_content_host->Layout();
    if (m_content_host != nullptr && m_content_host->GetParent() != nullptr)
        m_content_host->GetParent()->Layout();
    Layout();
    return true;
}

bool RedesignShell::navigate_to_tab(const wxString& id)
{
    if (id.empty())
        return true;

    Page page;
    if (id == wxString::FromUTF8(kRedesignAssetsTabId))
        page = Page::Assets;
    else if (id == TAB_ID_HOME)
        page = Page::Image;
    else if (id == TAB_ID_GENERATE_3D)
        page = Page::Model;
    else if (id == TAB_ID_PREPARE || id == TAB_ID_PREVIEW || id == TAB_ID_MONITOR ||
             id == TAB_ID_MONITOR_WEB || id == TAB_ID_MULTI_DEVICE || id == TAB_ID_CALIBRATION)
        page = Page::Print;
    else if (id == TAB_ID_PROJECT)
        page = Page::Assets;
    else {
        // A plugin or an older caller may still send a legacy page id while
        // migration is in progress. Keep that request inside the new shell
        // instead of exposing a route back to the legacy notebook.
        BOOST_LOG_TRIVIAL(warning) << "[UiRedesign] unmapped legacy tab routed to Assets: " << id;
        page = Page::Assets;
    }

    if (!navigate_to(page))
        return false;
    // navigate_to() sets the host's canonical id. Restore the original
    // semantic request so command availability and status queries do not
    // mistake Preview/Monitor/Multi-device for Prepare merely because they
    // share the same migration host.
    m_active_tab_id = id;
    return true;
}

wxString RedesignShell::active_tab_id() const
{
    return m_active_tab_id;
}
void RedesignShell::bind_upload_click(wxWindow* window)
{
    window->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        if (m_image_state != ImageState::Loading)
            choose_image();
    });
}

void RedesignShell::choose_image()
{
    wxString directory = wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Pictures);
    if (!m_selected_image_path.empty())
        directory = wxString(m_selected_image_path.parent_path().wstring());
    else if (wxGetApp().app_config != nullptr) {
        const std::string saved = wxGetApp().app_config->get("model_generation_image_directory");
        if (!saved.empty() && boost::filesystem::is_directory(saved))
            directory = wxString::FromUTF8(saved);
    }
    wxFileDialog dialog(this, text("选择参考图"), directory, wxEmptyString,
                        text("PNG 和 JPEG 图片 (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg"),
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK)
        accept_image(dialog.GetPath());
}

void RedesignShell::accept_image(const wxString& path)
{
    if (m_image_state == ImageState::Loading)
        return;
    const ImageState previous_state = m_image_state;
    const std::uint64_t generation = ++m_image_request_generation;
    m_image_state = ImageState::Loading;
    update_image_state();
    CallAfter([this, path, generation, previous_state] {
        if (generation != m_image_request_generation)
            return;
        const boost::filesystem::path selected_path(path.ToStdWstring());
        bool valid = false;
        wxImage image;
        try {
            valid = ModelGenerationPresentation::is_supported_image(selected_path);
            if (valid)
                image.LoadFile(path);
        } catch (const boost::filesystem::filesystem_error&) {
            valid = false;
        }
        if (!valid || !image.IsOk()) {
            m_image_state = previous_state == ImageState::Ready ? ImageState::Ready : ImageState::Failed;
            update_image_state();
            wxMessageBox(text("请选择可完整打开、宽高至少 64 px 且不超过 20 MB 的 PNG 或 JPEG 图片。"),
                         text("图片不可用"), wxOK | wxICON_ERROR, this);
            return;
        }
        m_selected_image = std::move(image);
        m_selected_image_path = selected_path;
        if (wxGetApp().app_config != nullptr)
            wxGetApp().app_config->set("model_generation_image_directory", selected_path.parent_path().string());
        m_image_state = ImageState::Ready;
        m_last_preview_bounds = wxDefaultSize;
        update_image_state();
    });
}

void RedesignShell::clear_image()
{
    ++m_image_request_generation;
    m_preview_resize_timer.Stop();
    m_selected_image = wxImage();
    m_selected_image_path.clear();
    m_image_state = ImageState::Empty;
    m_last_preview_bounds = wxDefaultSize;
    update_image_state();
}

void RedesignShell::update_preview_bitmap()
{
    if (m_image_state != ImageState::Ready || !m_selected_image.IsOk() || !m_preview)
        return;
    const wxSize available = m_preview->GetParent()->GetClientSize();
    const wxSize bounds(std::min(FromDIP(500), std::max(1, available.x - FromDIP(80))),
                        std::min(FromDIP(670), std::max(1, available.y - FromDIP(80))));
    if (bounds == m_last_preview_bounds)
        return;
    m_last_preview_bounds = bounds;
    const wxBitmap bitmap = scaled_bitmap(m_selected_image, bounds);
    m_preview->SetBitmap(bitmap);
    m_preview->SetMinSize(bitmap.GetSize());
    m_preview->GetParent()->Layout();
}

void RedesignShell::update_image_state()
{
    const bool ready = m_image_state == ImageState::Ready;
    const bool loading = m_image_state == ImageState::Loading;
    m_upload_icon->Show(!ready);
    m_upload_thumbnail->Show(ready);
    m_remove_image->Show(ready);
    m_upload_status->Show(!ready);
    m_upload_filename->Show(false);
    m_upload_hint->Show(!ready);
    if (loading) {
        m_upload_icon->SetLabel(text("◌"));
        m_upload_status->SetLabel(text("读取中..."));
        m_upload_hint->SetLabel(text("本地图片处理中"));
    } else if (m_image_state == ImageState::Failed) {
        m_upload_icon->SetLabel(text("+"));
        m_upload_status->SetLabel(text("图片不可用，点击重试"));
        m_upload_hint->SetLabel(text("支持：PNG、JPG、JPEG，最大 20MB"));
    } else {
        m_upload_icon->SetLabel(text("+"));
        m_upload_status->SetLabel(text("点击、拖拽选择图片"));
        m_upload_hint->SetLabel(text("支持：PNG、JPG、JPEG，最大 20MB"));
    }
    if (ready) {
        const wxBitmap thumbnail = scaled_bitmap(m_selected_image, wxSize(FromDIP(134), FromDIP(134)));
        m_upload_thumbnail->SetBitmap(thumbnail);
        m_upload_thumbnail->SetToolTip(wxString(m_selected_image_path.filename().wstring()));
    }
    m_guide_panel->Show(!ready);
    m_preview_host->Show(ready);
    m_upload_surface->Layout();
    if (ready) {
        const wxPoint thumbnail_position = m_upload_thumbnail->GetPosition();
        const wxSize thumbnail_size = m_upload_thumbnail->GetSize();
        m_remove_image->Move(thumbnail_position.x + thumbnail_size.x - FromDIP(12),
                             thumbnail_position.y - FromDIP(6));
        m_remove_image->Raise();
    }
    m_upload_surface->GetParent()->Layout();
    m_image_page->Layout();
    if (ready) {
        m_preview_host->Layout();
        update_preview_bitmap();
    }
}

}
