#include "RedesignShell.hpp"
#include "ImageHistorySidebar.hpp"
#include "AssetsWorkspace.hpp"
#include "RedesignTheme.hpp"
#include "RedesignFeatureFlags.hpp"
#include "RedesignModelRoute.hpp"
#include "RedesignWidgets.hpp"
#include "PrinterWorkspace.hpp"
#include "../MainFrame.hpp"
#include "../GUI_App.hpp"
#include "../AI/AIDesktopFeatureHost.hpp"
#include "../AI/ModelGeneration/ModelGenerationPresentation.hpp"
#include "../AI/ModelGeneration/ModelPreview3D.hpp"
#include "../AI/SmartSlicing/SmartSlicingFeatureHost.hpp"
#include "../Plater.hpp"
#include "../Widgets/Label.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <boost/log/trivial.hpp>

#include <wx/dnd.h>
#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/filedlgcustomize.h>
#include <wx/colour.h>
#include <wx/font.h>
#include <wx/fontenum.h>
#include <wx/gauge.h>
#include <wx/image.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/popupwin.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/weakref.h>
#include <wx/window.h>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

#include "libslic3r/Utils.hpp"

namespace Slic3r::GUI {
namespace {

using ImageDiagnosticClock = std::chrono::steady_clock;

long long image_diagnostic_elapsed(ImageDiagnosticClock::time_point started)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(ImageDiagnosticClock::now() - started).count();
}

// Observe the native dialog through wxWidgets' supported hook without adding
// controls, changing dialog flags, or accepting/rejecting a selection.
class ImagePickerDiagnostics final : public wxFileDialogCustomizeHook
{
public:
    explicit ImagePickerDiagnostics(wxFileDialog& dialog) : m_dialog(dialog), m_id(++s_sequence) {}

    void AddCustomControls(wxFileDialogCustomize&) override { log("hook_ready"); }
    void UpdateCustomControls() override
    {
        const wxString selection = m_dialog.GetCurrentlySelectedFilename();
        if (selection == m_selection)
            return;
        m_selection = selection;
        // No image contents, filenames or directories are written to the log.
        BOOST_LOG_TRIVIAL(info) << "[ImageUpload] picker=" << m_id
            << " stage=selection_changed has_selection=" << !selection.empty()
            << " path_chars=" << selection.length()
            << " elapsed_ms=" << image_diagnostic_elapsed(m_started);
    }
    void TransferDataFromCustomControls() override { log("file_ok_callback"); }
    void log(const char* stage) const
    {
        BOOST_LOG_TRIVIAL(info) << "[ImageUpload] picker=" << m_id << " stage=" << stage
            << " elapsed_ms=" << image_diagnostic_elapsed(m_started);
    }

private:
    inline static std::uint64_t s_sequence = 0; // File dialogs run on the GUI thread.
    wxFileDialog& m_dialog;
    const std::uint64_t m_id;
    const ImageDiagnosticClock::time_point m_started = ImageDiagnosticClock::now();
    wxString m_selection;
};

wxColour background_colour()
{
    return RedesignTheme::background_colour();
}

// The guide artwork uses the Figma canvas token (#313136), which is one blue
// channel lighter than the rest of the redesign shell background.
wxColour flow_background_colour()
{
    return RedesignTheme::flow_background_colour();
}

// Moving child windows can preserve pixels from their old positions on MSW.
// Invalidate the whole visible center after layout, including card backgrounds
// and labels, instead of relying on the newly exposed resize strip alone.
void refresh_image_surface(wxWindow* window)
{
    if (!window || !window->IsShown())
        return;
    window->Refresh();
    for (auto* child : window->GetChildren())
        refresh_image_surface(child);
}

wxColour panel_colour()
{
    return RedesignTheme::panel_colour();
}

wxColour control_colour()
{
    return RedesignTheme::control_colour();
}

wxColour primary_text_colour()
{
    return RedesignTheme::primary_text_colour();
}

wxColour secondary_text_colour()
{
    return RedesignTheme::secondary_text_colour();
}

wxColour accent_colour()
{
    return RedesignTheme::accent_colour();
}

wxColour divider_colour()
{
    return RedesignTheme::divider_colour();
}

wxString text(const char* value)
{
    return wxString::FromUTF8(value);
}

constexpr const char* kRedesignAssetsTabId = "REDESIGN_ASSETS";

void style_text(wxWindow* window, const wxColour& colour, int point_size, bool bold = false)
{
    RedesignTheme::style_text(window, colour, point_size, bold);
}

void style_medium_text(wxWindow* window, const wxColour& colour, int point_size)
{
    RedesignTheme::style_medium_text(window, colour, point_size);
}

class ImageServiceStatusRow final : public wxPanel
{
public:
    explicit ImageServiceStatusRow(wxWindow* parent) : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundColour(parent->GetBackgroundColour());
        auto* sizer = new wxBoxSizer(wxHORIZONTAL);
        SetSizer(sizer);
        m_indicator = new wxPanel(this, wxID_ANY);
        m_indicator->SetBackgroundColour(GetBackgroundColour());
        m_indicator->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_indicator->SetMinSize(FromDIP(wxSize(14, 14)));
        m_indicator->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(m_indicator);
            dc.SetBackground(wxBrush(m_indicator->GetBackgroundColour()));
            dc.Clear();
            std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
            if (!gc)
                return;
            const wxSize size = m_indicator->GetClientSize();
            const wxColour colour = m_indicator->GetForegroundColour();
            const double diameter = m_indicator->FromDIP(8);
            const double halo = m_indicator->FromDIP(14);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(wxColour(colour.Red(), colour.Green(), colour.Blue(), 35)));
            gc->DrawEllipse((size.x - halo) / 2.0, (size.y - halo) / 2.0, halo, halo);
            gc->SetBrush(wxBrush(colour));
            gc->DrawEllipse((size.x - diameter) / 2.0, (size.y - diameter) / 2.0, diameter, diameter);
        });
        sizer->Add(m_indicator, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(7));
        m_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
        style_text(m_label, secondary_text_colour(), 11);
        sizer->Add(m_label, 0, wxALIGN_CENTER_VERTICAL);
        set_status(AIServiceStatus::Checking);
    }

    wxStaticText* label() const { return m_label; }

    void set_status(AIServiceStatus status)
    {
        wxColour colour(156, 156, 163);
        wxString description = text("未连接");
        wxString tooltip;
        switch (status) {
        case AIServiceStatus::Checking:
            description = text("检测中");
            colour = accent_colour();
            break;
        case AIServiceStatus::Reconnecting:
            description = text("重新连接中");
            colour = accent_colour();
            break;
        case AIServiceStatus::Unavailable:
            break;
        case AIServiceStatus::GenerationUnavailable:
            tooltip = text("AI 图片生成服务已连接，但生成功能暂不可用，请检查服务配置。");
            [[fallthrough]];
        case AIServiceStatus::Connected:
            description = text("已连接");
            colour = wxColour(122, 205, 153);
            break;
        }
        m_label->SetLabel(text("AI 图片生成服务: ") + description);
        m_label->SetForegroundColour(colour);
        m_indicator->SetForegroundColour(colour);
        for (wxWindow* control : {static_cast<wxWindow*>(this),
                                  static_cast<wxWindow*>(m_label),
                                  static_cast<wxWindow*>(m_indicator)}) {
            if (tooltip.empty())
                control->UnsetToolTip();
            else
                control->SetToolTip(tooltip);
        }
        m_indicator->Refresh();
        m_label->Refresh();
        Layout();
        GetParent()->Layout();
    }

private:
    wxPanel* m_indicator { nullptr };
    wxStaticText* m_label { nullptr };
};

wxBitmap scaled_bitmap(const wxImage& image, const wxSize& bounds, bool allow_upscale = false)
{
    if (!image.IsOk() || bounds.x <= 0 || bounds.y <= 0)
        return wxNullBitmap;
    const double scale = allow_upscale ?
        std::min(double(bounds.x) / image.GetWidth(), double(bounds.y) / image.GetHeight()) :
        std::min(1.0, std::min(double(bounds.x) / image.GetWidth(), double(bounds.y) / image.GetHeight()));
    return wxBitmap(image.Scale(std::max(1, int(std::round(image.GetWidth() * scale))),
                                std::max(1, int(std::round(image.GetHeight() * scale))), wxIMAGE_QUALITY_HIGH));
}

// Alpha-mask just the thumbnail corners; the upload surface itself remains a separate rounded panel.
wxBitmap rounded_thumbnail(const wxImage& source, const wxSize& bounds, int radius,
                           bool allow_upscale = false, bool round_only_when_filled = false)
{
    wxBitmap scaled;
    if (allow_upscale && source.IsOk() && bounds.x > 0 && bounds.y > 0) {
        const double scale = std::min(double(bounds.x) / source.GetWidth(),
                                      double(bounds.y) / source.GetHeight());
        const int width = std::min(bounds.x, std::max(1, int(std::floor(source.GetWidth() * scale))));
        const int height = std::min(bounds.y, std::max(1, int(std::floor(source.GetHeight() * scale))));
        scaled = wxBitmap(source.Scale(width, height,
                                       wxIMAGE_QUALITY_HIGH));
    } else {
        scaled = scaled_bitmap(source, bounds);
    }
    if (!scaled.IsOk()) return wxNullBitmap;
    // Letterboxed previews keep the image's rectangular edges. Allow one pixel
    // of scale rounding when deciding whether it fills the preview bounds.
    if (round_only_when_filled &&
        (scaled.GetWidth() < bounds.x - 1 || scaled.GetHeight() < bounds.y - 1))
        return scaled;
    wxImage image = scaled.ConvertToImage();
    if (!image.HasAlpha()) image.InitAlpha();
    const double r = std::min<double>(radius, std::min(image.GetWidth(), image.GetHeight()) / 2.0);
    for (int y = 0; y < image.GetHeight(); ++y) {
        for (int x = 0; x < image.GetWidth(); ++x) {
            const double px = x + 0.5, py = y + 0.5;
            const double dx = px - std::clamp(px, r, image.GetWidth() - r);
            const double dy = py - std::clamp(py, r, image.GetHeight() - r);
            const double coverage = std::clamp(r + 0.5 - std::hypot(dx, dy), 0.0, 1.0);
            image.SetAlpha(x, y, static_cast<unsigned char>(std::round(image.GetAlpha(x, y) * coverage)));
        }
    }
    return wxBitmap(image);
}

wxBitmap resource_bitmap(wxWindow* window, const char* name, const wxSize& logical_bounds)
{
    if (window == nullptr || logical_bounds.x <= 0 || logical_bounds.y <= 0)
        return wxNullBitmap;

    const wxSize pixel_bounds(window->FromDIP(logical_bounds.x), window->FromDIP(logical_bounds.y));
    wxBitmap bitmap = scaled_bitmap(
        wxImage(wxString::FromUTF8((Slic3r::resources_dir() + "/images/" + name).c_str())),
        pixel_bounds, true);
    if (bitmap.IsOk())
        bitmap.SetScaleFactor(window->GetDPIScaleFactor());
    return bitmap;
}

wxImage resource_image(const char* name)
{
    return wxImage(wxString::FromUTF8((Slic3r::resources_dir() + "/images/" + name).c_str()));
}

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

class PromptTextCtrl final : public wxTextCtrl
{
public:
    PromptTextCtrl(wxWindow* parent, const wxSize& size)
    {
        Create(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, size,
               wxTE_MULTILINE | wxTE_NO_VSCROLL | wxBORDER_NONE);
#ifdef __WXMSW__
        Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
            if (event.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL || event.GetWheelDelta() <= 0) {
                event.Skip();
                return;
            }

            m_wheel_rotation += event.GetWheelRotation();
            const int steps = m_wheel_rotation / event.GetWheelDelta();
            m_wheel_rotation %= event.GetWheelDelta();
            if (event.IsPageScroll()) {
                for (int i = 0; i < std::abs(steps); ++i)
                    ::SendMessageW(GetHandle(), EM_SCROLL, steps > 0 ? SB_PAGEUP : SB_PAGEDOWN, 0);
            } else if (steps != 0) {
                ::SendMessageW(GetHandle(), EM_LINESCROLL, 0, -steps * event.GetLinesPerAction());
            }
        });
#endif
    }

#ifdef __WXMSW__
    WXDWORD MSWGetStyle(long style, WXDWORD* exstyle) const override
    {
        // wxTE_NO_VSCROLL also removes automatic scrolling on MSW.
        return wxTextCtrl::MSWGetStyle(style, exstyle) | ES_AUTOVSCROLL;
    }

private:
    int m_wheel_rotation { 0 };
#endif
};

class StylePickerPopup;

class StylePicker final : public wxPanel {
public:
    using SelectionChanged = std::function<void(int)>;

    StylePicker(wxWindow* parent, wxArrayString choices, SelectionChanged on_selection,
                bool show_icon = false, int height = 48)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, parent->FromDIP(height)))
        , m_choices(std::move(choices))
        , m_on_selection(std::move(on_selection))
        , m_show_icon(show_icon)
        , m_height(height)
    {
        SetMinSize(wxSize(-1, FromDIP(m_height)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(panel_colour());
        style_text(this, primary_text_colour(), 10);
        SetCanFocus(true);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
            m_hovered = true;
            Refresh();
            event.Skip();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            m_hovered = false;
            m_pressed = false;
            Refresh();
            event.Skip();
        });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            if (!IsEnabled() || !m_interactive)
                return;
            m_pressed = true;
            SetFocus();
            Refresh();
            event.Skip();
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (!IsEnabled() || !m_interactive)
                return;
            m_pressed = false;
            SetFocus();
            Refresh();
            toggle_popup();
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { Refresh(); event.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { Refresh(); event.Skip(); });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (!IsEnabled() || !m_interactive) {
                event.Skip();
                return;
            }
            switch (event.GetKeyCode()) {
            case WXK_UP:
                set_selection(std::max(0, m_selection - 1));
                break;
            case WXK_DOWN:
                set_selection(std::min(static_cast<int>(m_choices.size()) - 1, m_selection + 1));
                break;
            case WXK_RETURN:
            case WXK_SPACE:
                toggle_popup();
                break;
            case WXK_ESCAPE:
                dismiss_popup();
                break;
            default:
                event.Skip();
                break;
            }
        });
    }

    int selection() const { return m_selection; }
    int choice_count() const { return static_cast<int>(m_choices.size()); }

    void set_choices(wxArrayString choices, int selection);

    void set_interactive(bool interactive)
    {
        m_interactive = interactive;
        Refresh();
    }

    void set_selection(int selection)
    {
        if (selection < 0 || selection >= static_cast<int>(m_choices.size()))
            return;
        if (m_selection == selection)
            return;
        m_selection = selection;
        Refresh();
        if (m_on_selection)
            m_on_selection(m_selection);
    }

private:
    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(panel_colour()));
        dc.Clear();
        const wxSize size = GetClientSize();
        if (size.x <= 0 || size.y <= 0)
            return;

        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        const bool active = IsEnabled() && m_interactive;
        const wxColour face = !active ? wxColour(40, 40, 43) :
                              m_pressed ? wxColour(47, 47, 51) :
                              m_hovered ? wxColour(35, 35, 39) : control_colour();
        gc->SetPen(FindFocus() == this ? wxPen(accent_colour(), std::max(1, FromDIP(1))) : *wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(face));
        gc->DrawRoundedRectangle(FromDIP(1), FromDIP(1), size.x - FromDIP(2), size.y - FromDIP(2), FromDIP(10));

        int text_x = FromDIP(16);
        if (m_show_icon) {
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(wxColour(255, 255, 255, 38)));
            const int tile_size = std::min(FromDIP(44), size.y - FromDIP(10));
            const wxRect icon_tile(FromDIP(6), (size.y - tile_size) / 2, tile_size, tile_size);
            gc->DrawRoundedRectangle(icon_tile.x, icon_tile.y, icon_tile.width, icon_tile.height, FromDIP(9));
            const wxImage icon_image = resource_image("redesign_skill_emoji.png");
            if (icon_image.IsOk()) {
                const int icon_size = FromDIP(24);
                const wxBitmap icon(icon_image.Scale(icon_size, icon_size, wxIMAGE_QUALITY_HIGH));
                dc.DrawBitmap(icon, icon_tile.x + (icon_tile.width - icon.GetWidth()) / 2,
                              icon_tile.y + (icon_tile.height - icon.GetHeight()) / 2, true);
            }
            text_x = icon_tile.GetRight() + FromDIP(12);
        }

        dc.SetFont(GetFont());
        dc.SetTextForeground(active ? primary_text_colour() : wxColour(255, 255, 255, 82));
        if (m_selection >= 0 && m_selection < static_cast<int>(m_choices.size())) {
            const wxSize extent = dc.GetTextExtent(m_choices[m_selection]);
            dc.DrawText(m_choices[m_selection], text_x, (size.y - extent.y) / 2);
        }

        const int arrow_x = size.x - FromDIP(20);
        const int centre_y = size.y / 2;
        dc.SetPen(wxPen(active ? secondary_text_colour() : wxColour(255, 255, 255, 70),
                        std::max(1, FromDIP(1))));
        dc.DrawLine(arrow_x - FromDIP(4), centre_y - FromDIP(2), arrow_x, centre_y + FromDIP(2));
        dc.DrawLine(arrow_x, centre_y + FromDIP(2), arrow_x + FromDIP(4), centre_y - FromDIP(2));
    }

    void toggle_popup();
    void dismiss_popup();
    void select_from_popup(int selection);

    wxArrayString m_choices;
    SelectionChanged m_on_selection;
    int m_selection { 0 };
    StylePickerPopup* m_popup { nullptr };
    bool m_show_icon { false };
    bool m_interactive { true };
    bool m_hovered { false };
    bool m_pressed { false };
    int m_height { 48 };

    friend class StylePickerPopup;
};

class StylePickerPopup final : public wxPopupTransientWindow {
public:
    StylePickerPopup(StylePicker* owner, const wxArrayString& choices)
        : wxPopupTransientWindow(owner, wxBORDER_NONE)
        , m_owner(owner)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(38, 38, 38));
        auto* content = new wxBoxSizer(wxVERTICAL);
        content->AddSpacer(owner->FromDIP(10));
        for (std::size_t index = 0; index < choices.size(); ++index) {
            auto* row = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, owner->FromDIP(41)));
            row->SetMinSize(wxSize(-1, owner->FromDIP(41)));
            const bool selected = static_cast<int>(index) == m_owner->selection();
            const wxColour row_face = selected ? wxColour(56, 52, 39) : wxColour(38, 38, 38);
            row->SetBackgroundColour(row_face);
            auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);
            auto* row_label = new wxStaticText(row, wxID_ANY, choices[index], wxDefaultPosition, wxDefaultSize,
                                               wxALIGN_CENTER_VERTICAL);
            row_label->SetBackgroundColour(row_face);
            style_text(row_label, selected ? accent_colour() : primary_text_colour(), 10);
            row_sizer->Add(row_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, owner->FromDIP(16));
            row->SetSizer(row_sizer);
            const int selection = static_cast<int>(index);
            row->Bind(wxEVT_LEFT_UP, [this, selection](wxMouseEvent&) { choose(selection); });
            row_label->Bind(wxEVT_LEFT_UP, [this, selection](wxMouseEvent&) { choose(selection); });
            auto set_hover = [row, row_label, selected](bool hover) {
                const wxColour face = hover ? wxColour(53, 53, 57) :
                                      selected ? wxColour(56, 52, 39) : wxColour(38, 38, 38);
                row->SetBackgroundColour(face);
                row_label->SetBackgroundColour(face);
                row->Refresh();
                row_label->Refresh();
            };
            row->Bind(wxEVT_ENTER_WINDOW, [set_hover](wxMouseEvent& event) { set_hover(true); event.Skip(); });
            row->Bind(wxEVT_LEAVE_WINDOW, [set_hover](wxMouseEvent& event) { set_hover(false); event.Skip(); });
            row_label->Bind(wxEVT_ENTER_WINDOW, [set_hover](wxMouseEvent& event) { set_hover(true); event.Skip(); });
            row_label->Bind(wxEVT_LEAVE_WINDOW, [set_hover](wxMouseEvent& event) { set_hover(false); event.Skip(); });
            content->Add(row, 0, wxEXPAND);
        }
        content->AddSpacer(owner->FromDIP(10));
        SetSizer(content);
        SetSize(wxSize(owner->GetSize().x,
                       owner->FromDIP(20 + static_cast<int>(choices.size()) * 41)));
        Bind(wxEVT_PAINT, [this, owner](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(wxColour(38, 38, 38)));
            dc.Clear();
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
            if (!gc)
                return;
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(wxColour(38, 38, 38)));
            const wxSize size = GetClientSize();
            gc->DrawRoundedRectangle(0, 0, size.x, size.y, owner->FromDIP(12));
        });
    }

private:
    void choose(int selection)
    {
        m_owner->select_from_popup(selection);
        Dismiss();
    }

    StylePicker* m_owner;
};

void StylePicker::toggle_popup()
{
    if (m_popup != nullptr) {
        if (m_popup->IsShown()) {
            m_popup->Dismiss();
            return;
        }
        m_popup->Destroy();
        m_popup = nullptr;
    }

    m_popup = new StylePickerPopup(this, m_choices);
    const wxPoint position = ClientToScreen(wxPoint(0, GetClientSize().y + FromDIP(12)));
    m_popup->Position(position, wxSize(0, 0));
    m_popup->Popup(this);
}

void StylePicker::select_from_popup(int selection)
{
    set_selection(selection);
}

void StylePicker::set_choices(wxArrayString choices, int selection)
{
    if (m_popup != nullptr) {
        m_popup->Destroy();
        m_popup = nullptr;
    }
    m_choices = std::move(choices);
    if (m_choices.empty())
        m_selection = -1;
    else
        m_selection = std::clamp(selection, 0, static_cast<int>(m_choices.size()) - 1);
    Refresh();
}

void StylePicker::dismiss_popup()
{
    if (m_popup != nullptr && m_popup->IsShown())
        m_popup->Dismiss();
}

class FlowArrow final : public wxPanel {
public:
    explicit FlowArrow(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(93), parent->FromDIP(259)))
        , m_bitmap(resource_bitmap(parent, "redesign_flow_arrow_right.png", wxSize(93, 93)))
    {
        SetMinSize(wxSize(FromDIP(93), FromDIP(259)));
        SetMaxSize(wxSize(FromDIP(93), FromDIP(259)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(flow_background_colour());
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(flow_background_colour()));
            dc.Clear();
            if (m_bitmap.IsOk())
                dc.DrawBitmap(m_bitmap, (GetClientSize().x - m_bitmap.GetLogicalWidth()) / 2,
                              FromDIP(56), true);
        });
    }

private:
    wxBitmap m_bitmap;
};

class FlowPromptCard final : public wxPanel {
public:
    explicit FlowPromptCard(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(276), parent->FromDIP(160)))
    {
        SetMinSize(wxSize(FromDIP(276), FromDIP(160)));
        SetMaxSize(wxSize(FromDIP(276), FromDIP(160)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(32, 32, 35));

        auto* content = new wxBoxSizer(wxVERTICAL);
        SetSizer(content);
        auto* sample = label(this, "生成一只可爱的小怪兽手办。", 11);
        style_text(sample, wxColour(166, 166, 167), 11);
        content->Add(sample, 0, wxALL, FromDIP(12));
        content->AddStretchSpacer(1);
        auto* count = label(this, "13/800", 11);
        style_text(count, wxColour(166, 166, 167), 11);
        content->Add(count, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, FromDIP(12));

        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(wxColour(32, 32, 35)));
            dc.Clear();
            dc.SetPen(wxPen(wxColour(98, 98, 101), FromDIP(1), wxPENSTYLE_SHORT_DASH));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            const wxSize size = GetClientSize();
            dc.DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, FromDIP(12));
        });
    }
};

class FlowCardImage final : public wxPanel {
public:
    FlowCardImage(wxWindow* parent, wxImage image, double rotation_degrees = 0.0,
                  double scale_x = 1.0, double scale_y = 1.0)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(139), parent->FromDIP(177)))
        , m_image(std::move(image)), m_rotation_degrees(rotation_degrees)
        , m_scale_x(scale_x), m_scale_y(scale_y)
    {
        const wxSize size(FromDIP(139), FromDIP(177));
        SetMinSize(size);
        SetMaxSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(flow_background_colour());
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(flow_background_colour()));
            dc.Clear();
            const wxSize size = GetClientSize();
            if (!m_image.IsOk() || size.x <= 0 || size.y <= 0)
                return;
            const wxSize bitmap_size(
                std::max(1, static_cast<int>(std::round(size.x * m_scale_x))),
                std::max(1, static_cast<int>(std::round(size.y * m_scale_y))));
            const wxImage scaled = m_image.Scale(bitmap_size.x, bitmap_size.y, wxIMAGE_QUALITY_HIGH);
            const wxBitmap bitmap(scaled);
            if (std::abs(m_rotation_degrees) < 0.001) {
                dc.DrawBitmap(bitmap, 0, 0, true);
                return;
            }

            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
            if (!gc) {
                dc.DrawBitmap(bitmap, 0, 0, true);
                return;
            }
            gc->Translate(size.x / 2.0, size.y / 2.0);
            gc->Rotate(m_rotation_degrees * M_PI / 180.0);
            gc->DrawBitmap(bitmap, -bitmap_size.x / 2.0, -bitmap_size.y / 2.0,
                           bitmap_size.x, bitmap_size.y);
        });
    }

private:
    wxImage m_image;
    double m_rotation_degrees { 0.0 };
    double m_scale_x { 1.0 };
    double m_scale_y { 1.0 };
};

class FlowCompositeImage final : public wxPanel {
public:
    FlowCompositeImage(wxWindow* parent, wxImage rear, wxImage front)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(208), parent->FromDIP(167)))
        , m_rear(std::move(rear)), m_front(std::move(front))
    {
        const wxSize size(FromDIP(208), FromDIP(167));
        SetMinSize(size);
        SetMaxSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(flow_background_colour());
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(flow_background_colour()));
            dc.Clear();
            const wxSize size = GetClientSize();
            if (!m_rear.IsOk() || !m_front.IsOk() || size.x <= 0 || size.y <= 0)
                return;

            const wxImage rear = m_rear.Scale(FromDIP(126), FromDIP(145), wxIMAGE_QUALITY_HIGH);
            const wxImage front = m_front.Scale(FromDIP(146), FromDIP(167), wxIMAGE_QUALITY_HIGH);
            dc.DrawBitmap(wxBitmap(rear), 0, FromDIP(11), true);
            dc.DrawBitmap(wxBitmap(front), FromDIP(62), 0, true);
        });
    }

private:
    wxImage m_rear, m_front;
};

// The guide is authored at the 1920px Figma canvas size.  Paint the complete
// composition in one responsive surface so the shell can shrink it as a unit
// when the main window is narrower than that reference canvas.
class FlowGuideCanvas final : public wxPanel {
public:
    explicit FlowGuideCanvas(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, parent->FromDIP(259)),
                  wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE)
        , m_design(resource_image("redesign_flow_design.png"))
        , m_model_rear(resource_image("redesign_flow_model_mono.png"))
        , m_model_front(resource_image("redesign_flow_model_blue.png"))
        , m_arrow(resource_image("redesign_flow_arrow_right.png"))
    {
        SetMinSize(wxSize(-1, FromDIP(259)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(flow_background_colour());
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    }

private:
    static wxFont flow_font(int point_size, bool medium, double scale = 1.0)
    {
        const wxString family = wxFontEnumerator::IsValidFacename("HONOR Sans Design") ? "HONOR Sans Design" :
                                wxFontEnumerator::IsValidFacename("HarmonyOS Sans SC") ? "HarmonyOS Sans SC" :
                                wxFontEnumerator::IsValidFacename("Microsoft YaHei UI") ? "Microsoft YaHei UI" :
                                wxString();
        wxFontInfo info(std::max(1, static_cast<int>(std::round(point_size * scale))));
        info.Family(wxFONTFAMILY_SWISS);
        info.Weight(medium ? wxFONTWEIGHT_MEDIUM : wxFONTWEIGHT_NORMAL);
        if (!family.empty())
            info.FaceName(family);
        return wxFont(info);
    }

    static void draw_centered_text(wxDC& dc, const wxString& value, const wxFont& font,
                                   const wxColour& colour, int centre_x, int top_y)
    {
        dc.SetFont(font);
        dc.SetTextForeground(colour);
        wxCoord text_width = 0, text_height = 0;
        dc.GetTextExtent(value, &text_width, &text_height);
        const int x = centre_x - static_cast<int>(text_width) / 2;
        dc.DrawText(value, x, top_y);
    }

    static wxBitmap scaled_bitmap(const wxImage& image, int width, int height)
    {
        if (!image.IsOk() || width <= 0 || height <= 0)
            return wxNullBitmap;
        return wxBitmap(image.Scale(width, height, wxIMAGE_QUALITY_HIGH));
    }

    void draw_card(wxDC& dc, int x, int y, int width, int height, double scale)
    {
        dc.SetPen(wxPen(wxColour(98, 98, 101), std::max(1, FromDIP(scale)), wxPENSTYLE_SHORT_DASH));
        dc.SetBrush(wxBrush(wxColour(32, 32, 35)));
        dc.DrawRoundedRectangle(x, y, width, height, FromDIP(12 * scale));
        dc.SetFont(flow_font(11, false, scale));
        dc.SetTextForeground(wxColour(166, 166, 167));
        dc.DrawText(text("生成一只可爱的小怪兽手办。"), x + FromDIP(12 * scale),
                    y + FromDIP(12 * scale));
        wxCoord count_width = 0, count_height = 0;
        dc.GetTextExtent(text("13/800"), &count_width, &count_height);
        dc.DrawText(text("13/800"), x + width - FromDIP(12 * scale) - count_width,
                    y + height - FromDIP(12 * scale) - count_height);
    }

    void draw_rotated_design(wxAutoBufferedPaintDC& dc, int x, int y, int width, int height)
    {
        // The exported PNG contains a 264x344 opaque card inside transparent
        // shadow padding. Match that card to Figma's rotated 132x172 inner
        // frame so its 2.3-degree bounds fit the existing 139x177 slot.
        constexpr double source_width = 328.0;
        constexpr double source_height = 392.0;
        constexpr double card_x = 0.0;
        constexpr double card_y = 20.0;
        constexpr double card_width = 264.0;
        constexpr double card_height = 344.0;
        constexpr double figma_slot_width = 138.736;
        constexpr double figma_slot_height = 176.896;
        constexpr double figma_card_width = 131.95;
        constexpr double figma_card_height = 171.739;
        const double target_card_width = width * figma_card_width / figma_slot_width;
        const double target_card_height = height * figma_card_height / figma_slot_height;
        const int image_width = std::max(1, static_cast<int>(std::floor(
            target_card_width * source_width / card_width)));
        const int image_height = std::max(1, static_cast<int>(std::floor(
            target_card_height * source_height / card_height)));
        const wxBitmap bitmap = scaled_bitmap(m_design, image_width, image_height);
        if (!bitmap.IsOk())
            return;
        const double card_centre_x = (card_x + card_width / 2.0) * image_width / source_width;
        const double card_centre_y = (card_y + card_height / 2.0) * image_height / source_height;
        dc.SetClippingRegion(x, y, width, height);
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (gc) {
            gc->Translate(x + width / 2.0, y + height / 2.0);
            gc->Rotate(2.3 * M_PI / 180.0);
            gc->DrawBitmap(bitmap, -card_centre_x, -card_centre_y, image_width, image_height);
        }
        dc.DestroyClippingRegion();
    }

    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(flow_background_colour()));
        dc.Clear();
        const wxSize client = GetClientSize();
        if (client.x <= 0 || client.y <= 0)
            return;

        // The Figma pane containing this surface is 1427px wide.  Scale the
        // 1067px guide composition against that pane, then center it locally.
        const double scale = std::min(1.0, double(client.x) / double(FromDIP(1427)));
        const int flow_width = FromDIP(1067 * scale);
        const int flow_height = FromDIP(259 * scale);
        const int left = (client.x - flow_width) / 2;
        const int top = std::max(0, (client.y - flow_height) / 2);
        const auto dip = [this, scale](double value) { return FromDIP(value * scale); };

        const int prompt_x = left + dip(11), prompt_y = top + dip(7);
        const int prompt_w = dip(276), prompt_h = dip(160);
        draw_card(dc, prompt_x, prompt_y, prompt_w, prompt_h, scale);

        const int arrow_size = dip(93);
        const wxBitmap arrow = scaled_bitmap(m_arrow, arrow_size, arrow_size);
        if (arrow.IsOk()) {
            dc.DrawBitmap(arrow, left + dip(328), top + dip(56), true);
            dc.DrawBitmap(arrow, left + dip(697), top + dip(56), true);
        }

        const int design_x = left + dip(493), design_y = top;
        draw_rotated_design(dc, design_x, design_y, dip(139), dip(177));

        const int model_x = left + dip(840), model_y = top;
        const wxBitmap rear = scaled_bitmap(m_model_rear, dip(126), dip(145));
        const wxBitmap front = scaled_bitmap(m_model_front, dip(146), dip(167));
        if (rear.IsOk()) dc.DrawBitmap(rear, model_x, model_y + dip(11), true);
        if (front.IsOk()) dc.DrawBitmap(front, model_x + dip(62), model_y, true);

        const wxColour heading_colour(226, 226, 227);
        const wxColour subtitle_colour(162, 162, 164);
        const wxFont heading_font = flow_font(14, true, scale);
        const wxFont subtitle_font = flow_font(12, false, scale);
        draw_centered_text(dc, text("上传图片或输入提示词生成图片"), heading_font, heading_colour,
                           left + dip(150), top + dip(207));
        draw_centered_text(dc, text("生成图片"), heading_font, heading_colour,
                           left + dip(563), top + dip(207));
        draw_centered_text(dc, text("转为3D"), heading_font, heading_colour,
                           left + dip(939), top + dip(207));
        constexpr double subtitle_top = 234.0;
        draw_centered_text(dc, text("描述想创作的图片"), subtitle_font, subtitle_colour,
                           left + dip(150), top + dip(subtitle_top));
        draw_centered_text(dc, text("生成图片并完善"), subtitle_font, subtitle_colour,
                           left + dip(563), top + dip(subtitle_top));
        draw_centered_text(dc, text("获得可打印的专属 3D模型"), subtitle_font, subtitle_colour,
                           left + dip(939), top + dip(subtitle_top));
    }

    wxImage m_design;
    wxImage m_model_rear;
    wxImage m_model_front;
    wxImage m_arrow;
};



}

// Draw image and badge in one control, avoiding native sibling overlap and square button chrome.
class UploadThumbnail final : public wxPanel
{
public:
    UploadThumbnail(wxWindow* parent, std::function<void()> remove, std::function<void()> choose)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(150), parent->FromDIP(150)))
        , m_remove(std::move(remove)), m_choose(std::move(choose))
    {
        SetMinSize(wxSize(FromDIP(150), FromDIP(150)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(control_colour()));
            dc.Clear();
            if (!m_bitmap.IsOk()) return;
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
            if (!gc) return;
            const wxRect rect = image_rect();
            gc->DrawBitmap(m_bitmap, rect.x, rect.y, rect.width, rect.height);
            const wxPoint centre = close_centre(rect);
            const int radius = FromDIP(7);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(wxColour(139, 139, 142)));
            gc->DrawEllipse(centre.x - radius, centre.y - radius, 2 * radius, 2 * radius);
            gc->SetPen(wxPen(*wxWHITE, std::max(1, FromDIP(1))));
            const int arm = std::max(1, FromDIP(2));
            gc->StrokeLine(centre.x - arm, centre.y - arm, centre.x + arm, centre.y + arm);
            gc->StrokeLine(centre.x + arm, centre.y - arm, centre.x - arm, centre.y + arm);
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            const wxPoint centre = close_centre(image_rect());
            const int radius = FromDIP(11);
            const int dx = event.GetX() - centre.x, dy = event.GetY() - centre.y;
            if (dx * dx + dy * dy <= radius * radius) m_remove();
            else m_choose();
        });
    }

    void SetBitmap(const wxBitmap& bitmap) { m_bitmap = bitmap; Refresh(); }

private:
    wxRect image_rect() const
    {
        const wxSize size = GetClientSize(), bitmap_size = m_bitmap.GetSize();
        return wxRect((size.x - bitmap_size.x) / 2, (size.y - bitmap_size.y) / 2,
                      bitmap_size.x, bitmap_size.y);
    }
    wxPoint close_centre(const wxRect& rect) const
    {
        return wxPoint(rect.GetRight() - FromDIP(3), rect.GetTop() + FromDIP(4));
    }
    wxBitmap m_bitmap;
    std::function<void()> m_remove, m_choose;
};

class ImagePreview final : public wxPanel
{
public:
    enum class PlaceholderMode { Idle, Generating, Error, Stopped };

    ImagePreview(wxWindow* parent, const wxString& placeholder)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                  wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE)
        , m_placeholder(placeholder)
        , m_animation_timer(this)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(background_colour());
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            m_spinner_frame = (m_spinner_frame + 1) % 12;
            Refresh(false);
        }, m_animation_timer.GetId());
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(background_colour()));
            dc.Clear();
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
            if (!gc) return;
            const wxSize size = GetClientSize();
            if (size.x <= 0 || size.y <= 0)
                return;
            const bool generating = m_placeholder_mode == PlaceholderMode::Generating;
            const wxColour face = m_placeholder_mode == PlaceholderMode::Error ? wxColour(48, 34, 36) :
                                  m_placeholder_mode == PlaceholderMode::Stopped ? wxColour(43, 43, 47) :
                                  control_colour();
            const wxColour foreground = generating ? wxColour(255, 255, 255) :
                                        m_placeholder_mode == PlaceholderMode::Error ? wxColour(237, 174, 176) :
                                        secondary_text_colour();
            gc->SetBrush(wxBrush(face));
            gc->SetPen(wxPen(wxColour(255, 255, 255, 32),
                             std::max(1, FromDIP(1))));
            gc->DrawRoundedRectangle(FromDIP(1), FromDIP(1), size.x - FromDIP(2), size.y - FromDIP(2), FromDIP(11));
            if (m_bitmap.IsOk()) {
                gc->DrawBitmap(m_bitmap, (size.x - m_bitmap.GetWidth()) / 2,
                               (size.y - m_bitmap.GetHeight()) / 2,
                               m_bitmap.GetWidth(), m_bitmap.GetHeight());
                return;
            }
            dc.SetFont(GetFont());
            dc.SetTextForeground(foreground);
            wxString placeholder = m_placeholder;
            if (generating && m_design_wait.active())
                placeholder += "\n" + m_design_wait.message(monotonic_seconds());
            const wxSize extent = dc.GetMultiLineTextExtent(placeholder);
            int text_y = (size.y - extent.y) / 2;
            if (generating) {
                const double pi = std::acos(-1.0);
                const int radius = FromDIP(20);
                const double start = (m_spinner_frame * 30.0 - 90.0) * pi / 180.0;
                auto path = gc->CreatePath();
                path.AddArc(size.x / 2.0, size.y / 2.0 - FromDIP(28), radius,
                            start, start + pi * 1.45, false);
                gc->SetPen(wxPen(wxColour(255, 255, 255), std::max(2, FromDIP(3))));
                gc->StrokePath(path);
                text_y += FromDIP(28);
            }
            dc.DrawLabel(placeholder, wxRect(0, text_y, size.x, extent.y), wxALIGN_CENTER_HORIZONTAL);
        });
        style_text(this, secondary_text_colour(), 10);
    }

    void SetBitmap(const wxBitmap& bitmap)
    {
        m_animation_timer.Stop();
        m_design_wait.clear();
        m_bitmap = bitmap;
        m_placeholder_mode = PlaceholderMode::Idle;
        Refresh();
    }

    void SetPlaceholder(const wxString& placeholder, PlaceholderMode mode)
    {
        m_bitmap = wxNullBitmap;
        m_placeholder = placeholder;
        m_placeholder_mode = mode;
        if (mode == PlaceholderMode::Generating) {
            if (!m_animation_timer.IsRunning())
                m_animation_timer.Start(90);
        } else {
            m_animation_timer.Stop();
            m_spinner_frame = 0;
        }
        Refresh();
    }

    void SetDesignTiming(const std::string& job_id, double elapsed, double estimate)
    {
        m_design_wait.synchronize(job_id, elapsed, estimate, monotonic_seconds());
    }
    void ClearDesignTiming() { m_design_wait.clear(); }

private:
    static double monotonic_seconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    ModelGenerationPresentation::DesignGenerationWait m_design_wait;
    wxBitmap m_bitmap;
    wxString m_placeholder;
    wxTimer m_animation_timer;
    PlaceholderMode m_placeholder_mode { PlaceholderMode::Idle };
    int m_spinner_frame { 0 };
};

RedesignShell::RedesignShell(wxWindow* parent, ModelGenerationFeatureHost* model_generation_host, Plater* plater)
    : wxPanel(parent)
    , m_sizer(new wxBoxSizer(wxVERTICAL))
    , m_preview_resize_timer(this)
    , m_model_generation_host(model_generation_host)
{
    m_plater = plater;
    SetBackgroundColour(background_colour());
    SetSizer(m_sizer);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { update_preview_bitmap(); }, m_preview_resize_timer.GetId());
    build_image_workspace();
    connect_model_generation_host();
    build_model_workflow();
}

RedesignShell::~RedesignShell()
{
    if (m_print_page != nullptr)
        m_print_page->set_active(false);
    disconnect_model_generation_host();
    ++m_model_preview_request_generation;
    if (m_model_preview_worker.joinable())
        m_model_preview_worker.join();
}

void RedesignShell::disconnect_model_generation_host()
{
    m_import_timer.Stop();
    if (m_import_session) m_import_session->invalidate();
    if (m_print_page != nullptr)
        m_print_page->set_active(false);
    if (m_slicing_host) {
        m_slicing_host->set_workbench_listener({});
        m_slicing_host->set_workbench_active(false);
        m_slicing_host = nullptr;
    }
    if (m_plater && m_plater_original_parent) {
        if (auto* sizer = m_plater->GetContainingSizer()) sizer->Detach(m_plater);
        m_plater->Reparent(m_plater_original_parent);
        m_plater->collapse_sidebar(m_saved_sidebar_collapsed);
        m_plater->Hide();
        m_plater_original_parent = nullptr;
    }
    if (m_model_generation_host != nullptr) {
        m_model_generation_host->set_workbench_listener({});
        m_model_generation_host->set_workbench_results_handler({});
        m_model_generation_host->set_workbench_import_handler({});
        m_model_generation_host->unmount_workbench();
        m_model_generation_host->set_history_navigation_handler({});
        m_model_generation_host->unmount_assets();
        m_model_generation_host->set_state_listener({});
        m_model_generation_host = nullptr;
    }
}

void RedesignShell::set_service_status(AIServiceStatus status)
{
    if (!m_sidecar_status)
        return;
    static_cast<ImageServiceStatusRow*>(m_sidecar_status->GetParent())->set_status(status);
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
                                    resource_bitmap(navigation, "redesign_logo.png", wxSize(48, 48)));
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
                                             resource_bitmap(item, navigation_icons[index], wxSize(24, 24)));
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
    const std::array<wxSize, 4> footer_sizes = {
        wxSize(32, 32),
        wxSize(29, 29),
        wxSize(31, 31),
        wxSize(31, 31)};
    for (std::size_t index = 0; index < footer_icons.size(); ++index) {
        auto* item = new wxStaticBitmap(navigation, wxID_ANY,
                                        resource_bitmap(navigation, footer_icons[index], footer_sizes[index]));
        // RoundedPanel paints its face separately from its inherited surrounding colour.
        // Match the child background to the face so transparent icon assets do not show a square.
        item->SetBackgroundColour(wxColour(33, 33, 35));
        navigation_sizer->Add(item, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(index == 3 ? 21 : 40));
    }

    m_image_settings_panel = new RoundedPanel(this, wxDefaultSize, panel_colour(), background_colour(), 12);
    auto* settings_panel = m_image_settings_panel;
    settings_panel->SetBackgroundColour(panel_colour());
    settings_panel->SetMinSize(wxSize(FromDIP(373), -1));
    auto* settings_sizer = new wxBoxSizer(wxVERTICAL);
    settings_panel->SetSizer(settings_sizer);
    workspace->Add(settings_panel, 0, wxEXPAND | wxRIGHT, FromDIP(16));

    m_image_settings_scroll = new wxScrolledWindow(settings_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                                    wxVSCROLL | wxBORDER_NONE);
    auto* settings_scroll = m_image_settings_scroll;
    settings_scroll->SetBackgroundColour(panel_colour());
    settings_scroll->SetScrollRate(0, FromDIP(12));
    settings_scroll->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);
    auto* settings_content_host = m_image_settings_content = new wxPanel(settings_scroll, wxID_ANY);
    settings_content_host->SetBackgroundColour(panel_colour());
    auto* settings_content = new wxBoxSizer(wxVERTICAL);
    settings_content_host->SetSizer(settings_content);
    settings_sizer->Add(settings_scroll, 1, wxEXPAND);

    auto* heading = new wxStaticText(settings_content_host, wxID_ANY, text("上传图片"));
    style_text(heading, primary_text_colour(), 13, true);
    settings_content->Add(heading, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(21));

    m_upload_surface = new RoundedPanel(settings_content_host, wxSize(-1, FromDIP(160)), control_colour(), panel_colour(), 8);
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
    m_upload_thumbnail = new UploadThumbnail(m_upload_surface, [this] { clear_image(); },
                                             [this] { choose_image(); });
    upload_sizer->Add(m_upload_thumbnail, 0, wxALIGN_CENTER);
    m_upload_status = label(m_upload_surface, "点击、拖拽选择图片", 10);
    upload_sizer->Add(m_upload_status, 0, wxALIGN_CENTER | wxTOP, FromDIP(7));
    m_upload_filename = label(m_upload_surface, "", 9);
    style_text(m_upload_filename, secondary_text_colour(), 9);
    upload_sizer->Add(m_upload_filename, 0, wxALIGN_CENTER | wxTOP, FromDIP(2));
    m_upload_hint = label(m_upload_surface, "支持：PNG、JPG、JPEG，最大 20MB", 8);
    style_text(m_upload_hint, secondary_text_colour(), 8);
    upload_sizer->Add(m_upload_hint, 0, wxALIGN_CENTER | wxBOTTOM | wxTOP, FromDIP(2));
    upload_sizer->AddStretchSpacer(1);
    settings_content->Add(m_upload_surface, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    const std::array<wxWindow*, 7> upload_targets = {m_upload_surface, upload_tile, m_upload_icon, m_upload_status,
                                                      m_upload_hint, m_upload_filename, m_upload_thumbnail};
    for (wxWindow* target : upload_targets) {
        target->SetDropTarget(new ImageDropTarget([this](const wxString& path) { accept_image(path); }));
        if (target != m_upload_thumbnail) bind_upload_click(target);
    }

    auto* prompt_label = new wxStaticText(settings_content_host, wxID_ANY, text("描述"));
    style_text(prompt_label, primary_text_colour(), 13, true);
    settings_content->Add(prompt_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    auto* prompt_surface = new RoundedPanel(settings_content_host, wxDefaultSize, control_colour(), panel_colour(), 8);
    prompt_surface->SetBackgroundColour(control_colour());
    auto* prompt_sizer = new wxBoxSizer(wxVERTICAL);
    prompt_surface->SetSizer(prompt_sizer);
    m_prompt = new PromptTextCtrl(prompt_surface, wxSize(-1, FromDIP(118)));
    m_prompt->SetBackgroundColour(control_colour());
    style_text(m_prompt, wxColour(230, 230, 233), 10);
    // wxWidgets emulates hints for multiline controls and remembers the current text colour.
    m_prompt->SetHint(text("描述你想创作的内容，例如：一只可爱的小猫。"));
    prompt_sizer->Add(m_prompt, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    auto* prompt_count = label(prompt_surface, "0/2000 UTF-8 字节", 9);
    style_text(prompt_count, secondary_text_colour(), 9);
    prompt_sizer->Add(prompt_count, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, FromDIP(12));
    m_prompt->Bind(wxEVT_TEXT, [this, prompt_count, prompt_surface](wxCommandEvent& event) {
        const auto encoded = m_prompt->GetValue().ToUTF8();
        const size_t bytes = encoded ? encoded.length() : 0;
        const bool over_limit = bytes > ModelGenerationPresentation::MAX_MODEL_INPUT_BYTES;
        prompt_count->SetLabel(wxString::Format("%zu/%zu", bytes,
            ModelGenerationPresentation::MAX_MODEL_INPUT_BYTES) + text(" UTF-8 字节") +
            (over_limit ? text("（已超限）") : wxString()));
        prompt_count->SetForegroundColour(over_limit ? wxColour(255, 128, 112) : secondary_text_colour());
        prompt_count->SetToolTip(text("中文通常占3字节。超限时保留完整文字，请精简后生成。"));
        prompt_surface->Layout();
        if (!m_applying_model_generation_state)
            synchronize_generation_input();
        event.Skip();
    });
    settings_content->Add(prompt_surface, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));

    m_provider_label = new wxStaticText(settings_content_host, wxID_ANY, text("3D模型选择"));
    style_text(m_provider_label, primary_text_colour(), 13, true);
    settings_content->Add(m_provider_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    const wxArrayString provider_choices {text("Tripo"), text("腾讯混元3D")};
    auto* provider_picker = new StylePicker(settings_content_host, provider_choices, [this](int) {
                                                if (!m_applying_model_generation_state)
                                                    on_generation_option_changed();
                                            });
    provider_picker->SetName("model-provider-choice");
    m_provider_choice = provider_picker;
    settings_content->Add(provider_picker, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));

    auto* style_label_control = new wxStaticText(settings_content_host, wxID_ANY, text("风格"));
    style_text(style_label_control, primary_text_colour(), 13, true);
    settings_content->Add(style_label_control, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    const wxArrayString style_choices { text("雕塑"), text("多色写实"), text("多色风格化") };
    auto* style_picker = new StylePicker(settings_content_host, style_choices, [this](int selection) {
        if (selection >= 0 && selection <= 2) {
            m_selected_style_id = ModelGenerationPresentation::selected_style(selection, m_last_stylized_style);
            update_generation_style_controls();
            if (!m_applying_model_generation_state)
                synchronize_generation_input();
        }
    }, true, 56);
    style_picker->SetName("image-style-choice");
    m_style_choice = style_picker;
    settings_content->Add(style_picker, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));

    m_stylized_styles_panel = new wxPanel(settings_content_host, wxID_ANY);
    m_stylized_styles_panel->SetBackgroundColour(panel_colour());
    auto* style_grid = new wxGridSizer(2, FromDIP(8), FromDIP(8));
    m_stylized_styles_panel->SetSizer(style_grid);
    for (size_t index = 0; index < ModelGenerationPresentation::STYLIZED_STYLE_IDS.size(); ++index) {
        const char* id = ModelGenerationPresentation::STYLIZED_STYLE_IDS[index];
        auto* button = new RoundedActionButton(m_stylized_styles_panel,
            ModelGenerationPresentation::style_label(id), false, 40);
        button->SetName("image-style-" + wxString::FromUTF8(id));
        button->SetToolTip(button->GetLabel());
        style_text(button, primary_text_colour(), 10);
        button->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            if (!generation_input_editable())
                return;
            m_last_stylized_style = static_cast<int>(index);
            m_selected_style_id = ModelGenerationPresentation::selected_style(2, m_last_stylized_style);
            update_generation_style_controls();
            synchronize_generation_input();
        });
        m_stylized_style_buttons[index] = button;
        style_grid->Add(button, 1, wxEXPAND);
    }
    settings_content->Add(m_stylized_styles_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));

    m_custom_style_panel = new wxPanel(settings_content_host, wxID_ANY);
    m_custom_style_panel->SetBackgroundColour(panel_colour());
    auto* custom_sizer = new wxBoxSizer(wxVERTICAL);
    m_custom_style_panel->SetSizer(custom_sizer);
    auto* custom_label = new wxStaticText(m_custom_style_panel, wxID_ANY, text("自定义风格描述"));
    style_text(custom_label, primary_text_colour(), 11);
    custom_sizer->Add(custom_label, 0, wxBOTTOM, FromDIP(10));
    auto* custom_surface = new RoundedPanel(m_custom_style_panel, wxDefaultSize,
        control_colour(), panel_colour(), 8);
    custom_surface->SetBackgroundColour(control_colour());
    auto* custom_input_sizer = new wxBoxSizer(wxVERTICAL);
    custom_surface->SetSizer(custom_input_sizer);
    m_custom_style = new PromptTextCtrl(custom_surface, wxSize(-1, FromDIP(76)));
    m_custom_style->SetName("image-custom-style-description");
    m_custom_style->SetBackgroundColour(control_colour());
    style_text(m_custom_style, wxColour(230, 230, 233), 10);
    m_custom_style->SetMaxLength(240);
    m_custom_style->SetHint(text("描述外观即可；系统会保留主体、构图和可见元素"));
    custom_input_sizer->Add(m_custom_style, 1, wxEXPAND | wxALL, FromDIP(12));
    custom_sizer->Add(custom_surface, 0, wxEXPAND);
    auto* custom_hint = new wxStaticText(m_custom_style_panel, wxID_ANY, text("必填，最多 240 字"));
    style_text(custom_hint, secondary_text_colour(), 9);
    custom_sizer->Add(custom_hint, 0, wxTOP, FromDIP(6));
    m_custom_style->Bind(wxEVT_TEXT, [this](wxCommandEvent& event) {
        if (!m_applying_model_generation_state)
            synchronize_generation_input();
        event.Skip();
    });
    settings_content->Add(m_custom_style_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    settings_content->AddSpacer(FromDIP(16));
    update_generation_style_controls();

    settings_scroll->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        layout_image_settings();
        event.Skip();
    });
    layout_image_settings();

    auto* action_panel = new wxPanel(settings_panel, wxID_ANY);
    action_panel->SetBackgroundColour(panel_colour());
    auto* action_sizer = new wxBoxSizer(wxVERTICAL);
    action_panel->SetSizer(action_sizer);
    auto* service_status_row = new ImageServiceStatusRow(action_panel);
    m_sidecar_status = service_status_row->label();
    action_sizer->Add(service_status_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    action_sizer->AddSpacer(FromDIP(8));
    m_generate_button = new RoundedActionButton(action_panel, text("生成 2D 设计图"), true, 48);
    m_generate_button->SetName("generate-2d-design");
    m_generate_button->Enable(false);
    m_generate_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { request_primary_action(); });
    action_sizer->Add(m_generate_button, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(21));
    m_secondary_action_button = new RoundedActionButton(action_panel, wxEmptyString, false, 40);
    m_secondary_action_button->SetName("generation-secondary-action");
    m_secondary_action_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { request_secondary_action(); });
    m_secondary_action_button->Hide();
    action_sizer->Add(m_secondary_action_button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM,
                      FromDIP(21));
    settings_sizer->Add(action_panel, 0, wxEXPAND);

    m_content_host = new wxPanel(this, wxID_ANY);
    m_content_host->SetBackgroundColour(background_colour());
    workspace->Add(m_content_host, 1, wxEXPAND);
    auto* content_host_sizer = new wxBoxSizer(wxVERTICAL);
    m_content_host->SetSizer(content_host_sizer);

    m_image_page = new wxPanel(m_content_host, wxID_ANY);
    m_image_page->SetBackgroundColour(background_colour());
    // The guide and previews alternate in the flexible center; history stays
    // beside either view and stretches vertically, never below the content.
    auto* content_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_image_page->SetSizer(content_sizer);
    content_host_sizer->Add(m_image_page, 1, wxEXPAND);

    m_guide_panel = new wxPanel(m_image_page, wxID_ANY);
    m_guide_panel->SetBackgroundColour(flow_background_colour());
    m_guide_panel->SetMinSize(wxSize(0, 0));
    auto* guide_sizer = new wxBoxSizer(wxVERTICAL);
    m_guide_panel->SetSizer(guide_sizer);
    auto* title = label(m_guide_panel, "上传图片创建你的专属模型吧！", 28);
    guide_sizer->AddStretchSpacer(1);
    guide_sizer->Add(title, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(76));

    auto* flow_canvas = new FlowGuideCanvas(m_guide_panel);
    guide_sizer->Add(flow_canvas, 0, wxEXPAND);
    guide_sizer->AddStretchSpacer(1);
    content_sizer->Add(m_guide_panel, 1, wxEXPAND);

    m_preview_host = new wxPanel(m_image_page, wxID_ANY);
    m_preview_host->SetBackgroundColour(background_colour());
    m_preview_host->SetMinSize(wxSize(0, 0));
    auto* preview_sizer = new wxBoxSizer(wxVERTICAL);
    m_preview_host->SetSizer(preview_sizer);
    preview_sizer->AddStretchSpacer(1);
    auto* preview_row = new wxBoxSizer(wxHORIZONTAL);
    auto create_preview_card = [this, preview_row](const char* title_text, const char* placeholder,
                                                    wxPanel** card_out, ImagePreview** preview_out) {
        auto* card = new wxPanel(m_preview_host, wxID_ANY);
        card->SetBackgroundColour(background_colour());
        auto* card_sizer = new wxBoxSizer(wxVERTICAL);
        card->SetSizer(card_sizer);
        auto* title = new wxStaticText(card, wxID_ANY, text(title_text), wxDefaultPosition,
                                       wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        style_text(title, primary_text_colour(), 12, true);
        card_sizer->Add(title, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(12));
        auto* preview = new ImagePreview(card, text(placeholder));
        card_sizer->Add(preview, 0, wxALIGN_CENTER);
        preview_row->Add(card, 0, wxALIGN_CENTER_VERTICAL);
        *card_out = card;
        *preview_out = preview;
    };
    create_preview_card("平面图", "等待图片", &m_source_preview_card, &m_preview);
    preview_row->AddSpacer(FromDIP(16));
    create_preview_card("2D 设计图", "等待生成", &m_result_preview_card, &m_result_preview);
    preview_sizer->Add(preview_row, 0, wxALIGN_CENTER);
    preview_sizer->AddStretchSpacer(1);
    content_sizer->Add(m_preview_host, 1, wxEXPAND);
    m_preview_host->Hide();
    m_image_history = new ImageHistorySidebar(m_image_page,
        [this](const std::string& id) { return request_open_history(id); },
        [this] { m_preview_resize_timer.StartOnce(1); });
    content_sizer->Add(m_image_history, 0, wxEXPAND | wxLEFT, FromDIP(12));
    // Run after native child repositioning finishes. Sizers can emit forced
    // size events without a size change: ignore those to avoid a layout loop.
    auto resize_center = [this, last_size = wxDefaultSize](wxSizeEvent& event) mutable {
        if (event.GetSize() != last_size) {
            last_size = event.GetSize();
            m_preview_resize_timer.StartOnce(1);
        }
        event.Skip();
    };
    m_guide_panel->Bind(wxEVT_SIZE, resize_center);
    m_preview_host->Bind(wxEVT_SIZE, resize_center);
    update_image_state();

    m_assets_page = m_assets_workspace = new AssetsWorkspace(m_content_host, [this] { save_print_project(); });
    m_content_host->GetSizer()->Add(m_assets_page, 1, wxEXPAND);
    m_assets_page->Hide();
    m_pages[static_cast<std::size_t>(Page::Assets)] = m_assets_page;
    m_pages[static_cast<std::size_t>(Page::Image)] = m_image_page;
    m_pages[static_cast<std::size_t>(Page::Model)] = build_model_workspace();
    m_print_page = new PrinterWorkspace(m_content_host, m_plater);
    m_pages[static_cast<std::size_t>(Page::Print)] = m_print_page;
    m_content_host->GetSizer()->Add(m_print_page, 1, wxEXPAND);
    m_print_page->Hide();
    navigate_to(Page::Image);
}

wxPanel* RedesignShell::build_model_workspace()
{
    m_model_page = new wxPanel(m_content_host, wxID_ANY);
    m_model_page->SetBackgroundColour(background_colour());
    auto* outer = new wxBoxSizer(wxVERTICAL);
    m_model_page->SetSizer(outer);

    m_model_stage_title = new wxStaticText(m_model_page, wxID_ANY, text("3D 模型"));
    style_text(m_model_stage_title, primary_text_colour(), 24, true);
    outer->Add(m_model_stage_title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(42));
    m_model_stage_status = new wxStaticText(m_model_page, wxID_ANY, text("完成 2D 设计后可开始生成 3D 模型"));
    style_text(m_model_stage_status, secondary_text_colour(), 11);
    m_model_stage_status->Wrap(FromDIP(760));
    outer->Add(m_model_stage_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(42));

    auto* body = new wxBoxSizer(wxHORIZONTAL);
    m_model_visual_host = new wxPanel(m_model_page, wxID_ANY);
    m_model_visual_host->SetBackgroundColour(background_colour());
    auto* visual_sizer = new wxBoxSizer(wxVERTICAL);
    m_model_visual_host->SetSizer(visual_sizer);
    m_model_stage_visual = new ImagePreview(m_model_visual_host, text("等待 3D 生成"));
    m_model_stage_visual->SetMinSize(wxSize(FromDIP(420), FromDIP(480)));
    visual_sizer->Add(m_model_stage_visual, 1, wxEXPAND);
    m_model_preview_3d = new ModelPreview3D(m_model_visual_host);
    m_model_preview_3d->SetBackgroundColour(background_colour());
    m_model_preview_3d->set_selection_enabled(false);
    m_model_preview_3d->set_color_controls_visible(false);
    m_model_preview_3d->Hide();
    visual_sizer->Add(m_model_preview_3d, 1, wxEXPAND);
    body->Add(m_model_visual_host, 1, wxEXPAND | wxRIGHT, FromDIP(34));

    auto* status_column = new wxPanel(m_model_page, wxID_ANY, wxDefaultPosition,
                                      wxSize(FromDIP(340), -1));
    status_column->SetMinSize(wxSize(FromDIP(300), -1));
    status_column->SetMaxSize(wxSize(FromDIP(380), -1));
    status_column->SetBackgroundColour(background_colour());
    auto* status_sizer = new wxBoxSizer(wxVERTICAL);
    status_column->SetSizer(status_sizer);
    auto* progress_heading = new wxStaticText(status_column, wxID_ANY, text("任务状态"));
    style_text(progress_heading, primary_text_colour(), 12, true);
    status_sizer->Add(progress_heading, 0, wxBOTTOM, FromDIP(12));
    m_model_progress_label = new wxStaticText(status_column, wxID_ANY, text("尚未开始"));
    style_text(m_model_progress_label, primary_text_colour(), 11);
    status_sizer->Add(m_model_progress_label, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    auto* gauge = new wxGauge(status_column, wxID_ANY, 100, wxDefaultPosition,
                              wxSize(-1, FromDIP(7)), wxGA_HORIZONTAL | wxBORDER_NONE);
    gauge->SetValue(0);
    m_model_progress = gauge;
    status_sizer->Add(gauge, 0, wxEXPAND | wxBOTTOM, FromDIP(20));
    m_model_stage_summary = new Label(status_column, wxGetApp().normal_font(), wxEmptyString,
                                      LB_AUTO_WRAP, FromDIP(wxSize(300, -1)));
    style_text(m_model_stage_summary, secondary_text_colour(), 10);
    m_model_stage_summary->SetMinSize(wxSize(1, -1));
    status_sizer->Add(m_model_stage_summary, 0, wxEXPAND);
    m_model_preview_details = new Label(status_column, wxGetApp().normal_font(), wxEmptyString,
                                        LB_AUTO_WRAP, FromDIP(wxSize(300, -1)));
    style_text(m_model_preview_details, secondary_text_colour(), 9);
    m_model_preview_details->SetMinSize(wxSize(1, -1));
    m_model_preview_details->Hide();
    status_sizer->Add(m_model_preview_details, 0, wxEXPAND | wxTOP, FromDIP(14));
    m_model_view_controls = new wxPanel(status_column, wxID_ANY);
    m_model_view_controls->SetBackgroundColour(background_colour());
    auto* view_controls_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_model_view_controls->SetSizer(view_controls_sizer);
    auto* front_view = new RoundedActionButton(m_model_view_controls, text("正视图"), false, 34);
    front_view->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_model_preview_3d != nullptr)
            m_model_preview_3d->front_view();
    });
    view_controls_sizer->Add(front_view, 1, wxRIGHT, FromDIP(8));
    auto* reset_view = new RoundedActionButton(m_model_view_controls, text("重置视角"), false, 34);
    reset_view->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_model_preview_3d != nullptr)
            m_model_preview_3d->reset_view();
    });
    view_controls_sizer->Add(reset_view, 1);
    m_model_view_controls->Hide();
    status_sizer->Add(m_model_view_controls, 0, wxEXPAND | wxTOP, FromDIP(14));
    status_sizer->AddStretchSpacer(1);
    m_model_action_button = new RoundedActionButton(status_column, wxEmptyString, true, 46);
    m_model_action_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { request_model_page_action(); });
    m_model_action_button->Hide();
    status_sizer->Add(m_model_action_button, 0, wxEXPAND | wxTOP, FromDIP(14));
    m_model_stop_button = new RoundedActionButton(status_column, text("停止生成"), false, 40);
    m_model_stop_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_model_generation_host != nullptr && m_model_generation_host->request_stop())
            apply_model_generation_state(m_model_generation_host->snapshot());
    });
    m_model_stop_button->Hide();
    status_sizer->Add(m_model_stop_button, 0, wxEXPAND | wxTOP, FromDIP(10));
    body->Add(status_column, 0, wxEXPAND);
    outer->Add(body, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(42));

    m_content_host->GetSizer()->Add(m_model_page, 1, wxEXPAND);
    m_model_page->Hide();
    return m_model_page;
}

void RedesignShell::connect_model_generation_host()
{
    if (m_model_generation_host == nullptr) {
        apply_model_generation_state({});
        return;
    }

    apply_model_generation_state(m_model_generation_host->snapshot());
    wxWeakRef<RedesignShell> weak(this);
    m_model_generation_host->mount_assets(m_assets_workspace->local_assets_host());
    m_model_generation_host->set_history_navigation_handler([weak](bool design) {
        if (!weak || !weak->m_model_generation_host) return;
        weak->navigate_to(design ? Page::Image : Page::Model);
        if (!design) weak->open_model_workbench();
    });
    m_model_generation_host->set_state_listener([weak](const ModelGenerationUIState& state) {
        if (!weak || weak->m_model_generation_host == nullptr)
            return;
        weak->CallAfter([weak, state] {
            if (!weak || weak->m_model_generation_host == nullptr ||
                state.revision < weak->m_model_generation_state.revision)
                return;
            weak->apply_model_generation_state(state);
        });
    });
}

void RedesignShell::apply_model_generation_state(const ModelGenerationUIState& state)
{
    const auto route_action = model_generation_route_action(state, m_model_generation_state,
        m_model_route_locked, m_model_route_session, m_model_route_job_id);
    if (route_action == RedesignModelRouteAction::Ignore)
        return;
    const bool history_changed = state.design_ready &&
        (!m_model_generation_state.design_ready || state.job_id != m_model_generation_state.job_id);
    // An invalid draft must survive a host snapshot while the user edits it.
    // Loading another job still replaces the form with that job's input.
    bool preserve_over_limit_prompt = false;
    if (m_prompt && !state.busy && state.job_id == m_model_generation_state.job_id) {
        const auto encoded = m_prompt->GetValue().ToUTF8();
        preserve_over_limit_prompt = encoded && encoded.length() > ModelGenerationPresentation::MAX_MODEL_INPUT_BYTES;
    }
    if (route_action == RedesignModelRouteAction::Model || route_action == RedesignModelRouteAction::Image) {
        m_model_route_locked = route_action == RedesignModelRouteAction::Model;
        m_model_route_session = state.model_generation_session;
        m_model_route_job_id = state.job_id;
        m_model_view = ModelView::Result;
    }
    m_model_generation_state = state;
    if (m_image_history) {
        m_image_history->set_busy(state.busy || m_submit_in_progress);
        if (state.design_ready) m_image_history->set_selected(state.job_id);
        if (history_changed) m_image_history->refresh_history();
    }
    if (state.busy)
        m_submit_in_progress = false;

    m_applying_model_generation_state = true;
    if (m_prompt != nullptr && !preserve_over_limit_prompt &&
        m_prompt->GetValue().ToStdString(wxConvUTF8) != state.input.prompt)
        m_prompt->SetValue(wxString::FromUTF8(state.input.prompt.c_str()));
    if (m_style_choice != nullptr) {
        const int style_selection = ModelGenerationPresentation::style_selection(state.input.style);
        static_cast<StylePicker*>(m_style_choice)->set_selection(style_selection);
        m_selected_style_id = state.input.style;
        if (style_selection == 2)
            m_last_stylized_style = ModelGenerationPresentation::stylized_style_selection(state.input.style);
    }
    if (m_custom_style != nullptr && m_custom_style->GetValue().ToStdString(wxConvUTF8) != state.input.custom_style)
        m_custom_style->ChangeValue(wxString::FromUTF8(state.input.custom_style));
    apply_generation_options(state.options);
    m_applying_model_generation_state = false;
    m_input_sync_ok = m_model_generation_host != nullptr && current_generation_input() == state.input;
    m_option_sync_ok = m_model_generation_host != nullptr && current_generation_options() == state.options;
    if (state.design_ready && !state.design_image_path.empty())
        ensure_design_image(state.design_image_path);
    else
        clear_design_image();

    const bool editable = generation_input_editable();
    if (m_upload_surface != nullptr)
        m_upload_surface->Enable(editable);
    if (m_prompt != nullptr)
        m_prompt->Enable(editable);
    if (m_style_choice != nullptr) {
        auto* picker = static_cast<StylePicker*>(m_style_choice);
        picker->Enable(editable);
        picker->set_interactive(editable);
    }
    update_generation_style_controls();
    const bool provider_visible = true;
    if (m_provider_label != nullptr)
        m_provider_label->Show(provider_visible);
    const bool options_editable = editable && state.stage != ModelGenerationUIStage::GeneratingDesign &&
                                  state.stage != ModelGenerationUIStage::Saving3DOptions &&
                                  state.stage != ModelGenerationUIStage::Stopping;
    if (m_provider_choice != nullptr) {
        auto* picker = static_cast<StylePicker*>(m_provider_choice);
        picker->Show(provider_visible);
        picker->Enable(options_editable);
        picker->set_interactive(options_editable);
    }

    wxString primary_label = text("生成 2D 设计图");
    // The host validates both prompt-only and image inputs; editability still
    // blocks submission while an image is loading.
    bool primary_enabled = m_input_sync_ok && state.can_generate_design && editable;
    m_secondary_action = SecondaryAction::None;
    wxString secondary_label;
    wxString placeholder = text("等待生成");
    ImagePreview::PlaceholderMode placeholder_mode = ImagePreview::PlaceholderMode::Idle;
    bool show_design_bitmap = false;
    const auto update_design_preview = [&]() {
        if (state.design_image_path.empty() || m_design_image_state == ImageState::Failed) {
            placeholder = text("2D 设计图无法显示");
            placeholder_mode = ImagePreview::PlaceholderMode::Error;
        } else if (m_design_image_state == ImageState::Loading) {
            placeholder = text("正在加载 2D 设计图");
            placeholder_mode = ImagePreview::PlaceholderMode::Generating;
        } else {
            placeholder = text("2D 设计图已生成");
            show_design_bitmap = m_design_image_state == ImageState::Ready;
        }
    };

    switch (state.stage) {
    case ModelGenerationUIStage::Saving3DOptions:
        primary_enabled = false;
        update_design_preview();
        break;
    case ModelGenerationUIStage::GeneratingDesign:
        primary_label = text("生成中...");
        primary_enabled = false;
        placeholder = text("正在生成 2D 设计图");
        placeholder_mode = ImagePreview::PlaceholderMode::Generating;
        if (state.can_stop) {
            m_secondary_action = SecondaryAction::Stop;
            secondary_label = text("停止生成");
        }
        break;
    case ModelGenerationUIStage::Stopping:
        primary_label = text("正在停止");
        primary_enabled = false;
        placeholder = text("正在停止");
        placeholder_mode = ImagePreview::PlaceholderMode::Stopped;
        break;
    case ModelGenerationUIStage::Failed:
        if (state.model_generation_context)
            update_design_preview();
        else {
            placeholder = text("2D 设计图生成失败");
            placeholder_mode = ImagePreview::PlaceholderMode::Error;
        }
        if (state.can_retry_service) {
            m_secondary_action = SecondaryAction::RetryService;
            secondary_label = text("重新检测服务");
        } else if (state.can_restore_latest) {
            m_secondary_action = SecondaryAction::RestoreLatest;
            secondary_label = text("恢复上次任务");
        }
        break;
    case ModelGenerationUIStage::Stopped:
        if (state.model_generation_context)
            update_design_preview();
        else {
            primary_label = text("重新生成 2D 设计图");
            primary_enabled = m_input_sync_ok && state.can_generate_design && editable;
            placeholder = text("生成已停止");
            placeholder_mode = ImagePreview::PlaceholderMode::Stopped;
        }
        if (state.can_restore_latest) {
            m_secondary_action = SecondaryAction::RestoreLatest;
            secondary_label = text("恢复上次任务");
        }
        break;
    case ModelGenerationUIStage::DesignReady:
        if (!state.inputs_match_job) {
            primary_label = text("重新生成 2D 设计图");
            update_design_preview();
        } else {
            primary_label = text("生成 3D 模型");
            primary_enabled = m_option_sync_ok && state.can_generate_model &&
                              m_design_image_state == ImageState::Ready && editable;
            update_design_preview();
            if (state.can_restart) {
                m_secondary_action = SecondaryAction::Restart;
                secondary_label = text("重新开始");
            }
        }
        break;
    case ModelGenerationUIStage::GeneratingModel:
        primary_label = text("3D 模型生成中...");
        primary_enabled = false;
        update_design_preview();
        if (state.can_stop) {
            m_secondary_action = SecondaryAction::Stop;
            secondary_label = text("停止生成");
        }
        break;
    case ModelGenerationUIStage::ModelReady:
        primary_label = text("重新生成 2D 设计图");
        primary_enabled = m_input_sync_ok && state.can_generate_design && editable;
        update_design_preview();
        break;
    case ModelGenerationUIStage::Input:
        break;
    }

    if (!show_design_bitmap && m_result_preview != nullptr)
        m_result_preview->SetPlaceholder(placeholder, placeholder_mode);
    if (m_result_preview != nullptr) {
        if (state.stage == ModelGenerationUIStage::GeneratingDesign)
            m_result_preview->SetDesignTiming(state.job_id, state.design_elapsed_seconds, state.design_estimated_seconds);
        else
            m_result_preview->ClearDesignTiming();
    }
    if (m_generate_button != nullptr) {
        m_generate_button->SetLabel(primary_label);
        m_generate_button->Enable(primary_enabled);
        m_generate_button->Refresh();
    }
    if (m_secondary_action_button != nullptr) {
        m_secondary_action_button->SetLabel(secondary_label);
        m_secondary_action_button->Enable(m_secondary_action != SecondaryAction::None);
        m_secondary_action_button->Show(m_secondary_action != SecondaryAction::None);
        m_secondary_action_button->Refresh();
    }

    // Connection presentation is owned by set_service_status(). Generation
    // availability only gates actions; it must not overwrite a connected service.
    update_model_page(state);
    update_image_state();
    if (route_action == RedesignModelRouteAction::Model)
        navigate_to(Page::Model);
    else if (route_action == RedesignModelRouteAction::Image)
        navigate_to(Page::Image);
    if (m_image_settings_panel != nullptr)
        m_image_settings_panel->Layout();
    layout_image_settings();
}

void RedesignShell::update_generation_style_controls()
{
    const bool stylized = ModelGenerationPresentation::style_selection(m_selected_style_id) == 2;
    const bool editable = generation_input_editable();
    if (m_stylized_styles_panel != nullptr) {
        m_stylized_styles_panel->Show(stylized);
        for (size_t index = 0; index < m_stylized_style_buttons.size(); ++index) {
            auto* button = static_cast<RoundedActionButton*>(m_stylized_style_buttons[index]);
            const bool selected = m_selected_style_id == ModelGenerationPresentation::STYLIZED_STYLE_IDS[index];
            button->set_secondary_face(selected ? wxColour(73, 61, 29) : control_colour());
            button->set_text_colour(selected ? accent_colour() : primary_text_colour());
            button->Enable(editable);
        }
    }
    if (m_custom_style_panel != nullptr) {
        m_custom_style_panel->Show(m_selected_style_id == "custom");
        m_custom_style->Enable(editable);
    }
    layout_image_settings();
}

void RedesignShell::layout_image_settings()
{
    if (m_image_settings_scroll == nullptr || m_image_settings_content == nullptr)
        return;
    const wxSize client = m_image_settings_scroll->GetClientSize();
    const int width = std::max(FromDIP(330), client.x);
    const int height = std::max(client.y, m_image_settings_content->GetSizer()->CalcMin().y);
    // Keep the content in scrolled coordinates when expanding a style section.
    int x = 0, y = 0;
    m_image_settings_scroll->CalcScrolledPosition(0, 0, &x, &y);
    m_image_settings_content->SetSize(x, y, width, height);
    m_image_settings_content->Layout();
    m_image_settings_scroll->SetVirtualSize(width, height);
}

ModelGenerationUIInput RedesignShell::current_generation_input() const
{
    ModelGenerationUIInput input;
    if (!m_selected_image_path.empty()) {
        const wxScopedCharBuffer encoded = wxString(m_selected_image_path.wstring()).ToUTF8();
        if (encoded)
            input.image_path.assign(encoded.data(), encoded.length());
    }
    if (m_prompt != nullptr) {
        const wxScopedCharBuffer encoded = m_prompt->GetValue().ToUTF8();
        if (encoded)
            input.prompt.assign(encoded.data(), encoded.length());
    }
    input.style = m_selected_style_id;
    if (m_custom_style != nullptr)
        input.custom_style = m_custom_style->GetValue().ToStdString(wxConvUTF8);
    return input;
}

ModelGenerationUIOptions RedesignShell::current_generation_options() const
{
    ModelGenerationUIOptions options = m_generation_options;
    if (m_provider_choice != nullptr) {
        const int provider = static_cast<StylePicker*>(m_provider_choice)->selection();
        options.provider = provider == 1 ? "hunyuan" : "tripo";
    }
    if (options.provider == "hunyuan") {
        options.face_limit = std::min(options.face_limit, 1000000);
        options.geometry_quality = "standard";
        options.texture_quality = "standard";
    }
    return options;
}

void RedesignShell::apply_generation_options(const ModelGenerationUIOptions& options)
{
    m_generation_options = options;
    if (m_provider_choice != nullptr)
        static_cast<StylePicker*>(m_provider_choice)->set_selection(options.provider == "hunyuan" ? 1 : 0);
}

void RedesignShell::on_generation_option_changed()
{
    if (m_model_generation_host == nullptr || m_applying_model_generation_state || m_provider_choice == nullptr)
        return;
    const auto options = current_generation_options();
    m_generation_options = options;
    const bool valid = options.face_limit != 2000000 || options.geometry_quality == "detailed";
    if (!valid) {
        m_option_sync_ok = false;
        if (m_generate_button != nullptr &&
            m_model_generation_state.stage == ModelGenerationUIStage::DesignReady)
            m_generate_button->Enable(false);
        return;
    }
    synchronize_generation_options();
}

bool RedesignShell::synchronize_generation_input()
{
    if (m_model_generation_host == nullptr || m_applying_model_generation_state)
        return false;
    m_input_sync_ok = m_model_generation_host->synchronize_input(current_generation_input());
    apply_model_generation_state(m_model_generation_host->snapshot());
    return m_input_sync_ok;
}

bool RedesignShell::synchronize_generation_options()
{
    if (m_model_generation_host == nullptr || m_applying_model_generation_state)
        return false;
    m_option_sync_ok = m_model_generation_host->synchronize_options(current_generation_options());
    if (m_option_sync_ok)
        apply_model_generation_state(m_model_generation_host->snapshot());
    return m_option_sync_ok;
}

bool RedesignShell::generation_input_editable() const
{
    return m_image_state != ImageState::Loading && !m_submit_in_progress && !m_model_generation_state.busy &&
           m_model_generation_state.stage != ModelGenerationUIStage::GeneratingDesign &&
           m_model_generation_state.stage != ModelGenerationUIStage::GeneratingModel;
}

bool RedesignShell::request_open_history(const std::string& job_id)
{
    if (!generation_input_editable() || job_id.empty() || !m_model_generation_host ||
        !m_model_generation_host->request_open_image_history(job_id)) return false;
    const auto state = m_model_generation_host->snapshot();
    ++m_image_request_generation;
    m_selected_image_path = boost::filesystem::path(wxString::FromUTF8(state.input.image_path).ToStdWstring());
    m_selected_image = wxImage();
    if (!m_selected_image_path.empty()) m_selected_image.LoadFile(m_selected_image_path.wstring());
    m_image_state = m_selected_image.IsOk() ? ImageState::Ready : ImageState::Empty;
    m_last_preview_bounds = wxDefaultSize;
    m_pending_workbench_job.clear();
    apply_model_generation_state(state);
    update_image_state();
    return true;
}

void RedesignShell::request_generate_design()
{
    if (m_submit_in_progress || !generation_input_editable())
        return;
    m_submit_in_progress = true;
    apply_model_generation_state(m_model_generation_state);
    if (!synchronize_generation_input()) {
        m_submit_in_progress = false;
        apply_model_generation_state(m_model_generation_host->snapshot());
        return;
    }
    if (m_model_generation_host == nullptr || !m_model_generation_host->request_generate_design()) {
        m_submit_in_progress = false;
        apply_model_generation_state(m_model_generation_host != nullptr ? m_model_generation_host->snapshot() : ModelGenerationUIState());
        return;
    }
    m_submit_in_progress = false;
    apply_model_generation_state(m_model_generation_host->snapshot());
}

void RedesignShell::request_generate_model()
{
    if (m_submit_in_progress || m_model_generation_state.stage != ModelGenerationUIStage::DesignReady ||
        !m_model_generation_state.inputs_match_job || m_design_image_state != ImageState::Ready)
        return;
    m_submit_in_progress = true;
    apply_model_generation_state(m_model_generation_state);
    if (!synchronize_generation_options()) {
        m_submit_in_progress = false;
        apply_model_generation_state(m_model_generation_state);
        return;
    }
    // The host presents confirmation on the current image page. Only its
    // confirmed submission state may enter and lock the model route.
    // A false return can also mean asynchronous history preparation is still
    // pending; the state listener handles that submission when it is ready.
    if (m_model_generation_host != nullptr)
        m_model_generation_host->request_generate_model();
    m_submit_in_progress = false;
    apply_model_generation_state(m_model_generation_host != nullptr ? m_model_generation_host->snapshot() : ModelGenerationUIState());
}

void RedesignShell::request_primary_action()
{
    if (m_model_generation_state.stage == ModelGenerationUIStage::DesignReady &&
        m_model_generation_state.inputs_match_job)
        request_generate_model();
    else
        request_generate_design();
}

void RedesignShell::request_secondary_action()
{
    bool handled = false;
    switch (m_secondary_action) {
    case SecondaryAction::Stop:
        handled = m_model_generation_host != nullptr && m_model_generation_host->request_stop();
        break;
    case SecondaryAction::RetryService:
        handled = m_model_generation_host != nullptr && m_model_generation_host->request_retry_service();
        break;
    case SecondaryAction::RestoreLatest:
        handled = m_model_generation_host != nullptr && m_model_generation_host->request_restore_latest();
        break;
    case SecondaryAction::Restart:
        handled = m_model_generation_host != nullptr && m_model_generation_host->request_restart();
        break;
    case SecondaryAction::None:
        return;
    }
    if (handled)
        apply_model_generation_state(m_model_generation_host->snapshot());
}

void RedesignShell::request_model_page_action()
{
    bool handled = false;
    switch (m_model_page_action) {
    case ModelPageAction::RetryService:
        handled = m_model_generation_host != nullptr && m_model_generation_host->request_retry_service();
        break;
    case ModelPageAction::RetryModel:
        handled = m_model_generation_host != nullptr && m_model_generation_host->request_retry_model();
        break;
    case ModelPageAction::RestoreLatest:
        handled = m_model_generation_host != nullptr && m_model_generation_host->request_restore_latest();
        break;
    case ModelPageAction::BackToDesign:
        m_model_route_locked = false;
        m_model_route_session = 0;
        m_model_route_job_id.clear();
        navigate_to(Page::Image);
        return;
    case ModelPageAction::ReloadPreview:
        m_model_preview_failed = false;
        m_model_preview_error.clear();
        m_loaded_model_path.clear();
        ensure_model_preview(m_model_generation_state);
        update_model_page(m_model_generation_state);
        return;
    case ModelPageAction::Import:
        handled = owns_model_workflow() ? open_model_workbench() :
            m_model_generation_host != nullptr && m_model_generation_host->request_import();
        break;
    case ModelPageAction::None:
        return;
    }
    if (handled)
        apply_model_generation_state(m_model_generation_host->snapshot());
}

void RedesignShell::update_model_page(const ModelGenerationUIState& state)
{
    if (m_model_page == nullptr || m_model_stage_visual == nullptr)
        return;
    m_model_page_action = ModelPageAction::None;
    wxString title = text("3D 模型");
    wxString status = text("完成 2D 设计后可开始生成 3D 模型");
    wxString progress_label = text("尚未开始");
    wxString summary;
    wxString action_label;
    wxString visual_label = text("等待 3D 生成");
    ImagePreview::PlaceholderMode visual_mode = ImagePreview::PlaceholderMode::Idle;
    bool show_progress = false;
    int progress = 0;
    bool show_stop = false;

    if (state.stage == ModelGenerationUIStage::GeneratingModel) {
        title = text("正在生成 3D 模型");
        status = !state.status_text.empty() ? wxString::FromUTF8(state.status_text.c_str())
                                            : text("任务已提交，正在等待现有生成流程完成");
        visual_label = text("正在生成 3D 模型");
        visual_mode = ImagePreview::PlaceholderMode::Generating;
        if (state.progress > 0 && state.progress < 100) {
            show_progress = true;
            progress = state.progress;
            progress_label = wxString::Format(text("当前进度  %d%%"), state.progress);
        } else {
            progress_label = text("处理中");
        }
        summary = !state.workflow_guidance.empty() ? wxString::FromUTF8(state.workflow_guidance.c_str())
                                                   : text("页面切换不会取消任务。停止只终止本地等待，远端任务可能继续运行并计费。");
        show_stop = state.can_stop;
    } else if (state.stage == ModelGenerationUIStage::Failed && state.model_generation_context) {
        title = text("3D 模型生成未完成");
        status = !state.status_text.empty() ? wxString::FromUTF8(state.status_text.c_str())
                                            : text("现有生成流程返回错误");
        progress_label = text("生成失败");
        visual_label = text("3D 模型生成失败");
        visual_mode = ImagePreview::PlaceholderMode::Error;
        summary = !state.summary_text.empty() ? wxString::FromUTF8(state.summary_text.c_str())
                                              : text("当前任务和输入已保留，请按现有恢复入口继续。");
        if (!state.provider_error_code.empty()) {
            summary += "\n" + text("错误码：") + wxString::FromUTF8(state.provider_error_code.c_str());
        }
        if (!state.provider_task_id.empty()) {
            summary += "\n" + text("Provider 任务 ID：") + wxString::FromUTF8(state.provider_task_id.c_str());
        }
        if (state.provider_error_ambiguous) {
            summary += "\n" + text("提交结果不明确，请先确认远端任务状态，避免重复计费。");
        }
        if (state.can_retry_model) {
            m_model_page_action = ModelPageAction::RetryModel;
            action_label = state.provider_task_id.empty() ? text("重试当前任务") : text("恢复当前任务");
        } else if (state.can_retry_service) {
            m_model_page_action = ModelPageAction::RetryService;
            action_label = text("重新检测服务");
        } else if (state.can_restore_latest) {
            m_model_page_action = ModelPageAction::RestoreLatest;
            action_label = text("恢复上次任务");
        } else {
            m_model_page_action = ModelPageAction::BackToDesign;
            action_label = text("返回 2D 设计");
        }
    } else if (state.stage == ModelGenerationUIStage::Stopped && state.model_generation_context) {
        title = text("3D 模型生成已停止");
        status = !state.status_text.empty() ? wxString::FromUTF8(state.status_text.c_str())
                                            : text("已停止本地等待");
        progress_label = text("已停止");
        visual_label = text("生成已停止");
        visual_mode = ImagePreview::PlaceholderMode::Stopped;
        summary = text("远端任务可能仍继续运行并计费；恢复操作会继续查询同一任务。");
        if (state.can_retry_model) {
            m_model_page_action = ModelPageAction::RetryModel;
            action_label = state.provider_task_id.empty() ? text("重试当前任务") : text("恢复当前任务");
        } else if (state.can_restore_latest) {
            m_model_page_action = ModelPageAction::RestoreLatest;
            action_label = text("恢复上次任务");
        } else {
            m_model_page_action = ModelPageAction::BackToDesign;
            action_label = text("返回 2D 设计");
        }
    } else if (state.stage == ModelGenerationUIStage::ModelReady) {
        title = text("3D 模型已生成");
        status = !state.status_text.empty() ? wxString::FromUTF8(state.status_text.c_str())
                                            : text("生成结果已就绪");
        progress_label = text("完成  100%");
        show_progress = true;
        progress = 100;
        visual_label = text("正在加载 3D 模型预览");
        visual_mode = ImagePreview::PlaceholderMode::Generating;
        summary = !state.summary_text.empty() ? wxString::FromUTF8(state.summary_text.c_str()) : wxString();
        ensure_model_preview(state);
        if (m_model_preview_failed) {
            visual_label = text("3D 模型预览加载失败");
            visual_mode = ImagePreview::PlaceholderMode::Error;
            progress_label = text("预览不可用");
            summary = m_model_preview_error.empty() ? text("模型文件缺失或无法解析。")
                                                    : wxString::FromUTF8(m_model_preview_error.c_str());
            m_model_page_action = ModelPageAction::ReloadPreview;
            action_label = text("重新加载预览");
        } else if (!m_loaded_model_path.empty()) {
            visual_label.clear();
            visual_mode = ImagePreview::PlaceholderMode::Idle;
            if (state.can_import) {
                m_model_page_action = ModelPageAction::Import;
                action_label = owns_model_workflow() ? text("打开工作台") : text("导入");
            }
        }
    } else if (!state.model_generation_context) {
        clear_model_preview();
    }

    m_model_stage_title->SetLabel(title);
    m_model_stage_status->SetLabel(status);
    m_model_stage_status->Wrap(FromDIP(760));
    m_model_progress_label->SetLabel(progress_label);
    static_cast<wxGauge*>(m_model_progress)->SetValue(progress);
    m_model_progress->Show(show_progress);
    m_model_stage_summary->SetLabel(summary);
    const bool show_model_preview = state.stage == ModelGenerationUIStage::ModelReady &&
                                    !m_loaded_model_path.empty() && !m_model_preview_failed;
    m_model_stage_visual->Show(!show_model_preview);
    m_model_preview_3d->Show(show_model_preview);
    if (!show_model_preview)
        m_model_stage_visual->SetPlaceholder(visual_label, visual_mode);
    if (m_model_preview_details != nullptr)
        m_model_preview_details->Show(show_model_preview);
    if (m_model_view_controls != nullptr)
        m_model_view_controls->Show(show_model_preview);
    m_model_action_button->SetLabel(action_label);
    m_model_action_button->Show(m_model_page_action != ModelPageAction::None);
    m_model_action_button->Enable(m_model_page_action != ModelPageAction::None && !state.busy);
    m_model_stop_button->Show(show_stop);
    m_model_stop_button->Enable(show_stop);
    m_model_page->Layout();
    if (m_model_visual_host != nullptr)
        m_model_visual_host->Layout();
}

void RedesignShell::ensure_model_preview(const ModelGenerationUIState& state)
{
    if (owns_model_workflow() && m_model_view != ModelView::Result) return;
    if (m_model_preview_3d == nullptr || !state.model_ready || state.busy || !state.can_import ||
        state.model_path.empty())
        return;
    const wxString decoded = wxString::FromUTF8(state.model_path.c_str());
    if (decoded.empty()) {
        m_model_preview_failed = true;
        m_model_preview_error = "模型路径无法读取。";
        return;
    }
    const boost::filesystem::path path(decoded.ToStdWstring());
    if (path == m_loaded_model_path || (m_model_preview_loading && path == m_loading_model_path))
        return;
    if (!ModelGenerationPresentation::is_nonempty_model(path)) {
        m_model_preview_failed = true;
        m_model_preview_error = "模型文件缺失或为空。";
        return;
    }
    if (m_model_preview_worker.joinable())
        m_model_preview_worker.join();

    const std::uint64_t generation = ++m_model_preview_request_generation;
    m_model_preview_loading = true;
    m_model_preview_failed = false;
    m_model_preview_error.clear();
    m_loading_model_path = path;
    const boost::filesystem::path metadata_path = state.job_id.empty()
        ? boost::filesystem::path()
        : ModelGenerationPresentation::library_metadata_path(state.job_id);
    auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
    wxWeakRef<RedesignShell> weak(this);
    try {
        m_model_preview_worker = std::thread([weak, generation, path, metadata_path, prepared] {
            std::string error;
            try {
                ModelPreview3D::prepare_model(path, *prepared, error, {}, metadata_path);
            } catch (const std::exception& exception) {
                error = exception.what();
            }
            wxGetApp().CallAfter([weak, generation, path, prepared, error = std::move(error)]() mutable {
                if (!weak)
                    return;
                auto* self = weak.get();
                if (self->m_model_preview_worker.joinable())
                    self->m_model_preview_worker.join();
                if (generation != self->m_model_preview_request_generation)
                    return;
                self->m_model_preview_loading = false;
                self->m_loading_model_path.clear();
                size_t triangles = 0;
                size_t colors = 0;
                Vec3d dimensions;
                std::string load_error = error;
                if (!load_error.empty() || !self->m_model_preview_3d->load_prepared_model(
                        std::move(*prepared), {}, triangles, dimensions, colors, load_error)) {
                    self->m_model_preview_failed = true;
                    self->m_model_preview_error = load_error.empty() ? "模型预览无法加载。" : load_error;
                    self->m_loaded_model_path.clear();
                } else {
                    self->m_model_preview_failed = false;
                    self->m_model_preview_error.clear();
                    self->m_loaded_model_path = path;
                    self->m_model_preview_3d->set_selection_enabled(false);
                    self->m_model_preview_3d->set_color_controls_visible(false);
                    self->m_model_preview_details->SetLabel(wxString::Format(
                        text("%llu 个三角面 · %.1f × %.1f × %.1f mm · %llu 种模型颜色"),
                        static_cast<unsigned long long>(triangles), dimensions.x(), dimensions.y(), dimensions.z(),
                        static_cast<unsigned long long>(colors)));
                }
                self->update_model_page(self->m_model_generation_state);
            });
        });
    } catch (const std::exception& exception) {
        m_model_preview_loading = false;
        m_loading_model_path.clear();
        m_model_preview_failed = true;
        m_model_preview_error = exception.what();
    }
}

void RedesignShell::clear_model_preview()
{
    if (!m_model_preview_loading && !m_model_preview_failed && m_loading_model_path.empty() &&
        m_loaded_model_path.empty())
        return;
    ++m_model_preview_request_generation;
    m_model_preview_loading = false;
    m_model_preview_failed = false;
    m_model_preview_error.clear();
    m_loading_model_path.clear();
    m_loaded_model_path.clear();
    if (m_model_preview_3d != nullptr)
        m_model_preview_3d->clear();
    if (m_model_preview_details != nullptr) {
        m_model_preview_details->SetLabel(wxEmptyString);
        m_model_preview_details->Hide();
    }
}

void RedesignShell::ensure_design_image(const std::string& path)
{
    const wxString decoded = wxString::FromUTF8(path.c_str());
    if (decoded.empty()) {
        clear_design_image();
        m_design_image_state = ImageState::Failed;
        return;
    }
    const boost::filesystem::path requested(decoded.ToStdWstring());
    if (requested == m_design_image_path && m_design_image_state != ImageState::Empty)
        return;

    const std::uint64_t generation = ++m_design_image_request_generation;
    m_design_image_path = requested;
    m_design_image = wxImage();
    m_design_image_state = ImageState::Loading;
    CallAfter([this, requested, generation] {
        if (generation != m_design_image_request_generation)
            return;
        wxImage image;
        try {
            image.LoadFile(wxString(requested.wstring()));
        } catch (const boost::filesystem::filesystem_error&) {
        }
        if (!image.IsOk()) {
            m_design_image_state = ImageState::Failed;
            apply_model_generation_state(m_model_generation_state);
            return;
        }
        m_design_image = std::move(image);
        m_design_image_state = ImageState::Ready;
        update_image_state();
        m_last_preview_bounds = wxDefaultSize;
        apply_model_generation_state(m_model_generation_state);
    });
}

void RedesignShell::clear_design_image()
{
    if (m_design_image_state == ImageState::Empty && m_design_image_path.empty())
        return;
    ++m_design_image_request_generation;
    m_design_image = wxImage();
    m_design_image_path.clear();
    m_design_image_state = ImageState::Empty;
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
    if (m_import_in_progress && !m_import_switching_view) return false;
    if (page == Page::Image && m_model_route_locked) return false;
    const std::size_t index = static_cast<std::size_t>(page);
    if (index >= m_pages.size() || m_pages[index] == nullptr)
        return false;

    m_active_page = page;
    switch (page) {
    case Page::Assets:
        m_active_tab_id = wxString::FromUTF8(kRedesignAssetsTabId);
        refresh_project_save_actions();
        if (m_model_generation_host) m_model_generation_host->request_refresh_history();
        break;
    case Page::Image:
        m_active_tab_id = TAB_ID_HOME;
        break;
    case Page::Model:
        m_active_tab_id = owns_model_workflow() && m_model_view == ModelView::Preview ? TAB_ID_PREVIEW :
            owns_model_workflow() && m_model_view == ModelView::Slicing ? TAB_ID_PREPARE : TAB_ID_GENERATE_3D;
        break;
    case Page::Print:
        m_active_tab_id = TAB_ID_MONITOR;
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
    refresh_workflow_layout();
    if (page == Page::Image)
        m_preview_resize_timer.StartOnce(1);
    if (m_print_page != nullptr)
        m_print_page->set_active(page == Page::Print);
    return true;
}

bool RedesignShell::navigate_to_tab(const wxString& id)
{
    if (m_import_in_progress && !m_import_switching_view) return false;
    if (id.empty())
        return true;
    if (owns_model_workflow() && (id == TAB_ID_PREPARE || id == TAB_ID_PREVIEW)) {
        navigate_to(Page::Model);
        show_model_view(id == TAB_ID_PREVIEW ? ModelView::Preview : ModelView::Slicing);
        m_active_tab_id = id;
        return true;
    }

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
    if (page == Page::Print && m_print_page != nullptr) {
        if (id == TAB_ID_PREPARE || id == TAB_ID_PREVIEW)
            m_print_page->show_prepare();
        else if (id == TAB_ID_MONITOR || id == TAB_ID_MONITOR_WEB)
            m_print_page->show_monitor();
    }
    return true;
}

void RedesignShell::show_printer_media()
{
    if (navigate_to(Page::Print) && m_print_page != nullptr)
        m_print_page->show_media();
}

void RedesignShell::refresh_printer_state()
{
    if (m_print_page != nullptr)
        m_print_page->refresh_state();
}

wxString RedesignShell::active_tab_id() const
{
    if (m_active_page == Page::Print && m_print_page != nullptr) {
        // Prepare/Preview belong to the model workflow when it is enabled.
        if (owns_model_workflow() || m_print_page->monitoring())
            return TAB_ID_MONITOR;
        if (m_active_tab_id == TAB_ID_MONITOR)
            return TAB_ID_PREPARE;
    }
    return m_active_tab_id;
}
void RedesignShell::bind_upload_click(wxWindow* window)
{
    window->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        if (m_image_state != ImageState::Loading && generation_input_editable())
            choose_image();
    });
}

void RedesignShell::choose_image()
{
    if (!generation_input_editable()) {
        BOOST_LOG_TRIVIAL(info) << "[ImageUpload] stage=picker_ignored reason=input_not_editable";
        return;
    }
    wxString directory = wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Pictures);
    if (!m_selected_image_path.empty())
        directory = wxString(m_selected_image_path.parent_path().wstring());
    else if (wxGetApp().app_config != nullptr) {
        const std::string saved = wxGetApp().app_config->get("model_generation_image_directory");
        if (!saved.empty() && boost::filesystem::is_directory(saved))
            directory = wxString::FromUTF8(saved);
    }
    // Declare the hook first so it outlives the dialog holding its pointer.
    std::unique_ptr<ImagePickerDiagnostics> diagnostics;
    wxFileDialog dialog(this, text("选择参考图"), directory, wxEmptyString,
                        text("PNG 和 JPEG 图片 (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg"),
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    diagnostics = std::make_unique<ImagePickerDiagnostics>(dialog);
    diagnostics->log(dialog.SetCustomizeHook(*diagnostics) ? "hook_registered" : "hook_unavailable");
    diagnostics->log("show_modal_begin");
    const int result = dialog.ShowModal();
    diagnostics->log(result == wxID_OK ? "returned_ok" : result == wxID_CANCEL ? "returned_cancel" : "returned_other");
    if (result == wxID_OK)
        accept_image(dialog.GetPath());
}

void RedesignShell::accept_image(const wxString& path)
{
    if (m_image_state == ImageState::Loading || !generation_input_editable()) {
        BOOST_LOG_TRIVIAL(info) << "[ImageUpload] stage=load_ignored reason=input_not_editable"
            << " already_loading=" << (m_image_state == ImageState::Loading);
        return;
    }
    const ImageState previous_state = m_image_state;
    const std::uint64_t generation = ++m_image_request_generation;
    const auto started = ImageDiagnosticClock::now();
    BOOST_LOG_TRIVIAL(info) << "[ImageUpload] load=" << generation << " stage=load_begin";
    m_image_state = ImageState::Loading;
    update_image_state();
    apply_model_generation_state(m_model_generation_state);
    CallAfter([this, path, generation, previous_state, started] {
        if (generation != m_image_request_generation) {
            BOOST_LOG_TRIVIAL(info) << "[ImageUpload] load=" << generation << " stage=stale_callback"
                << " elapsed_ms=" << image_diagnostic_elapsed(started);
            return;
        }
        const boost::filesystem::path selected_path(path.ToStdWstring());
        bool valid = false;
        wxImage image;
        try {
            BOOST_LOG_TRIVIAL(info) << "[ImageUpload] load=" << generation << " stage=validate_begin"
                << " elapsed_ms=" << image_diagnostic_elapsed(started);
            valid = ModelGenerationPresentation::is_supported_image(selected_path);
            BOOST_LOG_TRIVIAL(info) << "[ImageUpload] load=" << generation << " stage=validate_end valid=" << valid
                << " elapsed_ms=" << image_diagnostic_elapsed(started);
            if (valid) {
                BOOST_LOG_TRIVIAL(info) << "[ImageUpload] load=" << generation << " stage=decode_begin"
                    << " elapsed_ms=" << image_diagnostic_elapsed(started);
                image.LoadFile(path);
                BOOST_LOG_TRIVIAL(info) << "[ImageUpload] load=" << generation << " stage=decode_end valid=" << image.IsOk()
                    << " elapsed_ms=" << image_diagnostic_elapsed(started);
            }
        } catch (const boost::filesystem::filesystem_error& error) {
            BOOST_LOG_TRIVIAL(warning) << "[ImageUpload] load=" << generation << " stage=filesystem_error"
                << " error_code=" << error.code().value() << " category=" << error.code().category().name()
                << " elapsed_ms=" << image_diagnostic_elapsed(started);
            valid = false;
        }
        if (!valid || !image.IsOk()) {
            BOOST_LOG_TRIVIAL(warning) << "[ImageUpload] load=" << generation << " stage=load_rejected"
                << " elapsed_ms=" << image_diagnostic_elapsed(started);
            m_image_state = previous_state == ImageState::Ready ? ImageState::Ready : ImageState::Failed;
            update_image_state();
            apply_model_generation_state(m_model_generation_state);
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
        BOOST_LOG_TRIVIAL(info) << "[ImageUpload] load=" << generation << " stage=preview_update_begin"
            << " elapsed_ms=" << image_diagnostic_elapsed(started);
        update_image_state();
        BOOST_LOG_TRIVIAL(info) << "[ImageUpload] load=" << generation << " stage=input_sync_begin"
            << " elapsed_ms=" << image_diagnostic_elapsed(started);
        synchronize_generation_input();
        BOOST_LOG_TRIVIAL(info) << "[ImageUpload] load=" << generation << " stage=load_end input_sync_ok=" << m_input_sync_ok
            << " elapsed_ms=" << image_diagnostic_elapsed(started);
    });
}

void RedesignShell::clear_image()
{
    if (!generation_input_editable())
        return;
    ++m_image_request_generation;
    m_preview_resize_timer.Stop();
    m_selected_image = wxImage();
    m_selected_image_path.clear();
    m_image_state = ImageState::Empty;
    m_last_preview_bounds = wxDefaultSize;
    synchronize_generation_input();
    // Input synchronization also clears the old design result. Lay out the
    // empty workspace only after both image states have been updated.
    update_image_state();
}

void RedesignShell::update_preview_bitmap()
{
    m_preview_resize_timer.Stop();
    if (!m_image_page)
        return;
    m_image_page->Layout();
    auto refresh_center = [this] {
        m_image_page->Refresh();
        refresh_image_surface(m_guide_panel);
        refresh_image_surface(m_preview_host);
    };
    // Empty placeholders need the same responsive card sizes as loaded images.
    // Only the guide view skips preview layout, not the absence of a bitmap.
    if (!m_preview_host || !m_preview_host->IsShown() || !m_preview ||
        !m_result_preview || !m_source_preview_card || !m_result_preview_card) {
        if (m_guide_panel)
            m_guide_panel->Layout();
        refresh_center();
        return;
    }
    const bool source_ready = m_image_state == ImageState::Ready && m_selected_image.IsOk();
    const wxSize available = m_preview_host->GetClientSize();
    if (available.x < FromDIP(240) || available.y < FromDIP(240)) {
        refresh_center();
        return;
    }

    const int gap = FromDIP(16);
    const int maximum_width = std::max(FromDIP(96), source_ready
        ? (available.x - FromDIP(48) - gap) / 2 : available.x - FromDIP(48));
    const int maximum_height = std::max(FromDIP(128), available.y - FromDIP(92));
    const double aspect = 503.0 / 671.0;
    int width = std::min(FromDIP(503), maximum_width);
    int height = static_cast<int>(std::round(width / aspect));
    if (height > std::min(FromDIP(671), maximum_height)) {
        height = std::min(FromDIP(671), maximum_height);
        width = static_cast<int>(std::round(height * aspect));
    }
    const wxSize bounds(std::max(FromDIP(96), width), std::max(FromDIP(128), height));
    const bool bounds_changed = bounds != m_last_preview_bounds;
    m_last_preview_bounds = bounds;
    const wxSize image_bounds(std::max(1, bounds.x - FromDIP(4)), std::max(1, bounds.y - FromDIP(4)));
    if (bounds_changed) {
        m_preview->SetMinSize(bounds);
        m_preview->SetMaxSize(bounds);
        m_result_preview->SetMinSize(bounds);
        m_result_preview->SetMaxSize(bounds);
        const wxSize card_size(bounds.x, bounds.y + FromDIP(36));
        m_source_preview_card->SetMinSize(card_size);
        m_source_preview_card->SetMaxSize(card_size);
        m_result_preview_card->SetMinSize(card_size);
        m_result_preview_card->SetMaxSize(card_size);
    }
    m_preview_host->Layout();
    m_source_preview_card->Layout();
    m_result_preview_card->Layout();
    const wxBitmap bitmap = source_ready ? rounded_thumbnail(m_selected_image, image_bounds, FromDIP(10), true, true) : wxBitmap();
    m_preview->SetBitmap(bitmap);
    // Placeholder/spinner repainting must not clear the active task clock.
    if (m_design_image_state == ImageState::Ready && m_design_image.IsOk()) {
        m_result_preview->SetBitmap(
            rounded_thumbnail(m_design_image, image_bounds, FromDIP(10), true, true));
    }
    refresh_center();
}

void RedesignShell::update_image_state()
{
    const bool ready = m_image_state == ImageState::Ready;
    const bool loading = m_image_state == ImageState::Loading;
    m_upload_icon->GetParent()->Show(!ready);
    m_upload_thumbnail->Show(ready);
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
        const wxBitmap thumbnail = rounded_thumbnail(m_selected_image, wxSize(FromDIP(134), FromDIP(134)), FromDIP(8));
        m_upload_thumbnail->SetBitmap(thumbnail);
        m_upload_thumbnail->SetToolTip(wxString(m_selected_image_path.filename().wstring()));
    }
    // Preserve text-only designs and active placeholders while retaining
    // the responsive workspace after its first use.
    const bool show_preview = ready || m_design_image_state == ImageState::Ready ||
        m_model_generation_state.stage != ModelGenerationUIStage::Input || m_preview_host->IsShown();
    m_source_preview_card->Show(ready);
    m_source_preview_card->GetContainingSizer()->Show(size_t(1), ready);
    m_guide_panel->Show(!show_preview);
    m_preview_host->Show(show_preview);
    m_upload_surface->Layout();
    m_upload_surface->GetParent()->Layout();
    m_image_page->Layout();
    update_preview_bitmap();
}

}
