#include "RedesignShell.hpp"
#include "../MainFrame.hpp"
#include "../GUI_App.hpp"
#include "../AI/AIDesktopFeatureHost.hpp"
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
#include <wx/popupwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/window.h>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

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

// Alpha-mask just the thumbnail corners; the upload surface itself remains a separate rounded panel.
wxBitmap rounded_thumbnail(const wxImage& source, const wxSize& bounds, int radius)
{
    wxBitmap scaled = scaled_bitmap(source, bounds);
    if (!scaled.IsOk()) return wxNullBitmap;
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

wxBitmap resource_bitmap(const char* name, const wxSize& bounds)
{
    return scaled_bitmap(wxImage(wxString::FromUTF8((Slic3r::resources_dir() + "/images/" + name).c_str())), bounds);
}

wxImage resource_image(const char* name)
{
    return wxImage(wxString::FromUTF8((Slic3r::resources_dir() + "/images/" + name).c_str()));
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

    StylePicker(wxWindow* parent, wxArrayString choices, SelectionChanged on_selection)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, parent->FromDIP(56)))
        , m_choices(std::move(choices))
        , m_on_selection(std::move(on_selection))
    {
        SetMinSize(wxSize(-1, FromDIP(56)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(panel_colour());
        style_text(this, primary_text_colour(), 10);
        SetCanFocus(true);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            SetFocus();
            toggle_popup();
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
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

    void set_selection(int selection)
    {
        if (selection < 0 || selection >= static_cast<int>(m_choices.size()))
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
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(control_colour()));
        gc->DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(12));
        gc->SetBrush(wxBrush(wxColour(255, 255, 255, 38)));
        const wxRect icon_tile(FromDIP(6), FromDIP(6), FromDIP(45), FromDIP(45));
        gc->DrawRoundedRectangle(icon_tile.x, icon_tile.y, icon_tile.width, icon_tile.height, FromDIP(10));

        const wxImage icon_image = resource_image("redesign_skill_emoji.png");
        if (icon_image.IsOk()) {
            const int icon_size = FromDIP(24);
            const wxBitmap icon(icon_image.Scale(icon_size, icon_size, wxIMAGE_QUALITY_HIGH));
            dc.DrawBitmap(icon, icon_tile.x + (icon_tile.width - icon.GetWidth()) / 2,
                          icon_tile.y + (icon_tile.height - icon.GetHeight()) / 2, true);
        }

        dc.SetFont(GetFont());
        dc.SetTextForeground(primary_text_colour());
        if (m_selection >= 0 && m_selection < static_cast<int>(m_choices.size())) {
            const wxSize extent = dc.GetTextExtent(m_choices[m_selection]);
            dc.DrawText(m_choices[m_selection], FromDIP(62), (size.y - extent.y) / 2);
        }

        const int arrow_x = size.x - FromDIP(20);
        const int centre_y = size.y / 2;
        dc.SetPen(wxPen(secondary_text_colour(), std::max(1, FromDIP(1))));
        dc.DrawLine(arrow_x - FromDIP(4), centre_y - FromDIP(3), arrow_x, centre_y - FromDIP(7));
        dc.DrawLine(arrow_x, centre_y - FromDIP(7), arrow_x + FromDIP(4), centre_y - FromDIP(3));
        dc.DrawLine(arrow_x - FromDIP(4), centre_y + FromDIP(3), arrow_x, centre_y + FromDIP(7));
        dc.DrawLine(arrow_x, centre_y + FromDIP(7), arrow_x + FromDIP(4), centre_y + FromDIP(3));
    }

    void toggle_popup();
    void dismiss_popup();
    void select_from_popup(int selection);

    wxArrayString m_choices;
    SelectionChanged m_on_selection;
    int m_selection { 0 };
    StylePickerPopup* m_popup { nullptr };

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
            row->SetBackgroundColour(wxColour(38, 38, 38));
            auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);
            auto* row_label = new wxStaticText(row, wxID_ANY, choices[index], wxDefaultPosition, wxDefaultSize,
                                               wxALIGN_CENTER_VERTICAL);
            style_text(row_label, primary_text_colour(), 10);
            row_sizer->Add(row_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, owner->FromDIP(16));
            row->SetSizer(row_sizer);
            const int selection = static_cast<int>(index);
            row->Bind(wxEVT_LEFT_UP, [this, selection](wxMouseEvent&) { choose(selection); });
            row_label->Bind(wxEVT_LEFT_UP, [this, selection](wxMouseEvent&) { choose(selection); });
            content->Add(row, 0, wxEXPAND);
        }
        content->AddSpacer(owner->FromDIP(10));
        SetSizer(content);
        SetSize(wxSize(owner->GetSize().x, owner->FromDIP(143)));
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
        const wxPoint position = ClientToScreen(wxPoint(0, GetClientSize().y + FromDIP(12)));
        m_popup->SetSize(wxSize(GetSize().x, FromDIP(143)));
        m_popup->SetPosition(position);
        m_popup->Popup(this);
        return;
    }

    m_popup = new StylePickerPopup(this, m_choices);
    const wxPoint position = ClientToScreen(wxPoint(0, GetClientSize().y + FromDIP(12)));
    m_popup->SetPosition(position);
    m_popup->Popup(this);
}

void StylePicker::select_from_popup(int selection)
{
    set_selection(selection);
}

void StylePicker::dismiss_popup()
{
    if (m_popup != nullptr && m_popup->IsShown())
        m_popup->Dismiss();
}

class FlowArrow final : public wxPanel {
public:
    FlowArrow(wxWindow* parent, int centre_y)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(72), parent->FromDIP(255)))
        , m_centre_y(centre_y)
    {
        SetMinSize(wxSize(FromDIP(72), FromDIP(255)));
        SetMaxSize(wxSize(FromDIP(72), FromDIP(255)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(background_colour());
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(background_colour()));
            dc.Clear();
            const wxSize size = GetClientSize();
            const double y = FromDIP(m_centre_y);
            const int left = FromDIP(5);
            const int right = size.x - FromDIP(5);
            const int head = FromDIP(22);
            const int shaft_end = right - head;

            // Layered strokes approximate the Figma glow and tapered trail without a native image asset.
            dc.SetPen(wxPen(wxColour(73, 143, 216), FromDIP(10)));
            dc.DrawLine(left, static_cast<int>(y), shaft_end, static_cast<int>(y));
            dc.SetPen(wxPen(wxColour(129, 207, 255), FromDIP(5)));
            dc.DrawLine(left, static_cast<int>(y), shaft_end, static_cast<int>(y));

            wxPoint tail[] = {
                { left, static_cast<int>(y - FromDIP(12)) },
                { left + FromDIP(25), static_cast<int>(y - FromDIP(5)) },
                { shaft_end, static_cast<int>(y - FromDIP(5)) },
                { shaft_end, static_cast<int>(y + FromDIP(5)) },
                { left + FromDIP(25), static_cast<int>(y + FromDIP(5)) },
                { left, static_cast<int>(y + FromDIP(12)) }
            };
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(91, 177, 242)));
            dc.DrawPolygon(static_cast<int>(std::size(tail)), tail);

            wxPoint arrow[] = {
                { shaft_end - FromDIP(2), static_cast<int>(y - FromDIP(6)) },
                { shaft_end - FromDIP(2), static_cast<int>(y - FromDIP(15)) },
                { right, static_cast<int>(y) },
                { shaft_end - FromDIP(2), static_cast<int>(y + FromDIP(15)) },
                { shaft_end - FromDIP(2), static_cast<int>(y + FromDIP(6)) },
                { shaft_end - FromDIP(12), static_cast<int>(y) }
            };
            dc.SetPen(wxPen(wxColour(99, 184, 245), FromDIP(2)));
            dc.SetBrush(wxBrush(wxColour(116, 198, 255)));
            dc.DrawPolygon(static_cast<int>(std::size(arrow)), arrow);
        });
    }

private:
    int m_centre_y;
};

class FlowPromptCard final : public wxPanel {
public:
    explicit FlowPromptCard(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(250), parent->FromDIP(160)))
    {
        SetMinSize(wxSize(FromDIP(250), FromDIP(160)));
        SetMaxSize(wxSize(FromDIP(276), FromDIP(160)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(38, 38, 41));

        auto* content = new wxBoxSizer(wxVERTICAL);
        SetSizer(content);
        auto* sample = label(this, "生成一只可爱的小怪兽手办。", 9);
        style_text(sample, secondary_text_colour(), 9);
        content->Add(sample, 0, wxALL, FromDIP(10));
        content->AddStretchSpacer(1);
        auto* count = label(this, "13/800", 9);
        style_text(count, secondary_text_colour(), 9);
        content->Add(count, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, FromDIP(10));

        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(wxColour(38, 38, 41)));
            dc.Clear();
            dc.SetPen(wxPen(wxColour(92, 92, 101), FromDIP(1), wxPENSTYLE_SHORT_DASH));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            const wxSize size = GetClientSize();
            dc.DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, FromDIP(8));
        });
    }
};

class FlowCardImage final : public wxPanel {
public:
    FlowCardImage(wxWindow* parent, wxImage image, const wxSize& size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , m_image(std::move(image))
    {
        SetMinSize(size);
        SetMaxSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(background_colour());
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(background_colour()));
            dc.Clear();
            const wxSize size = GetClientSize();
            if (!m_image.IsOk() || size.x <= 0 || size.y <= 0)
                return;
            dc.SetBrush(wxBrush(wxColour(175, 218, 247)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(18));

            const int image_height = std::min(size.y - FromDIP(12), FromDIP(150));
            const double scale = double(image_height) / m_image.GetHeight();
            const int image_width = std::max(1, int(std::round(m_image.GetWidth() * scale)));
            const wxImage scaled = m_image.Scale(image_width, image_height, wxIMAGE_QUALITY_HIGH);
            dc.DrawBitmap(wxBitmap(scaled), (size.x - image_width) / 2, size.y - image_height - FromDIP(4), true);
        });
    }

private:
    wxImage m_image;
};

class FlowCompositeImage final : public wxPanel {
public:
    FlowCompositeImage(wxWindow* parent, wxImage rear, wxImage front, const wxSize& size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , m_rear(std::move(rear)), m_front(std::move(front))
    {
        SetMinSize(size);
        SetMaxSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(background_colour());
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(background_colour()));
            dc.Clear();
            const wxSize size = GetClientSize();
            if (!m_rear.IsOk() || !m_front.IsOk() || size.x <= 0 || size.y <= 0)
                return;

            const int image_height = std::min(size.y - FromDIP(8), FromDIP(162));
            const double scale = double(image_height) / m_rear.GetHeight();
            const int image_width = std::max(1, int(std::round(m_rear.GetWidth() * scale)));
            const wxImage rear = m_rear.Scale(image_width, image_height, wxIMAGE_QUALITY_HIGH);
            const wxImage front = m_front.Scale(image_width, image_height, wxIMAGE_QUALITY_HIGH);
            const int overlap = FromDIP(66);
            const int x_rear = 0;
            const int x_front = std::max(0, image_width - overlap);
            const int y = size.y - image_height;
            dc.DrawBitmap(wxBitmap(rear), x_rear, y, true);
            dc.DrawBitmap(wxBitmap(front), x_front, y, true);
        });
    }

private:
    wxImage m_rear, m_front;
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
    explicit ImagePreview(wxWindow* parent) : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(background_colour());
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(background_colour()));
            dc.Clear();
            if (!m_bitmap.IsOk()) return;
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
            if (!gc) return;
            const wxSize size = GetClientSize();
            gc->DrawBitmap(m_bitmap, (size.x - m_bitmap.GetWidth()) / 2,
                           (size.y - m_bitmap.GetHeight()) / 2,
                           m_bitmap.GetWidth(), m_bitmap.GetHeight());
        });
    }

    void SetBitmap(const wxBitmap& bitmap) { m_bitmap = bitmap; Refresh(); }

private:
    wxBitmap m_bitmap;
};

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

void RedesignShell::set_service_status(AIServiceStatus status)
{
    if (!m_sidecar_status)
        return;
    switch (status) {
    case AIServiceStatus::Checking:
        m_sidecar_status->SetLabel(text("AI 服务：检测中"));
        m_sidecar_status->SetForegroundColour(secondary_text_colour());
        break;
    case AIServiceStatus::Reconnecting:
        m_sidecar_status->SetLabel(text("AI 服务：重新连接中"));
        m_sidecar_status->SetForegroundColour(secondary_text_colour());
        break;
    case AIServiceStatus::Unavailable:
        m_sidecar_status->SetLabel(text("AI 服务：不可用"));
        m_sidecar_status->SetForegroundColour(wxColour(221, 165, 109));
        break;
    case AIServiceStatus::GenerationUnavailable:
        m_sidecar_status->SetLabel(text("AI 服务：已连接，生成功能不可用"));
        m_sidecar_status->SetForegroundColour(wxColour(221, 165, 109));
        break;
    case AIServiceStatus::Connected:
        m_sidecar_status->SetLabel(text("AI 服务：已连接"));
        m_sidecar_status->SetForegroundColour(wxColour(122, 205, 153));
        break;
    }
    m_sidecar_status->GetParent()->Layout();
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
    settings_sizer->Add(m_upload_surface, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    const std::array<wxWindow*, 7> upload_targets = {m_upload_surface, upload_tile, m_upload_icon, m_upload_status,
                                                      m_upload_hint, m_upload_filename, m_upload_thumbnail};
    for (wxWindow* target : upload_targets) {
        target->SetDropTarget(new ImageDropTarget([this](const wxString& path) { accept_image(path); }));
        if (target != m_upload_thumbnail) bind_upload_click(target);
    }

    auto* prompt_label = new wxStaticText(settings_panel, wxID_ANY, text("描述"));
    style_text(prompt_label, primary_text_colour(), 13, true);
    settings_sizer->Add(prompt_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    auto* prompt_surface = new RoundedPanel(settings_panel, wxDefaultSize, control_colour(), panel_colour(), 8);
    prompt_surface->SetBackgroundColour(control_colour());
    auto* prompt_sizer = new wxBoxSizer(wxVERTICAL);
    prompt_surface->SetSizer(prompt_sizer);
    m_prompt = new PromptTextCtrl(prompt_surface, wxSize(-1, FromDIP(118)));
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

    auto* style_label_control = new wxStaticText(settings_panel, wxID_ANY, text("风格"));
    style_text(style_label_control, primary_text_colour(), 13, true);
    settings_sizer->Add(style_label_control, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    const wxArrayString style_choices { text("雕塑"), text("多色写实"), text("多色风格化") };
    auto* style_picker = new StylePicker(settings_panel, style_choices, [this](int selection) {
        static constexpr const char* style_ids[] = { "sculpture", "realistic", "cartoon" };
        if (selection >= 0 && selection < static_cast<int>(std::size(style_ids)))
            m_selected_style_id = style_ids[selection];
    });
    m_style_choice = style_picker;
    settings_sizer->Add(style_picker, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    settings_sizer->AddStretchSpacer(1);

    m_sidecar_status = new wxStaticText(settings_panel, wxID_ANY, text("AI 服务：检测中"));
    style_text(m_sidecar_status, secondary_text_colour(), 9);
    m_sidecar_status->SetMinSize(wxSize(-1, FromDIP(20)));
    settings_sizer->Add(m_sidecar_status, 0, wxLEFT | wxRIGHT, FromDIP(21));
    settings_sizer->AddSpacer(FromDIP(8));
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
    auto add_step = [this, flow](const char* image_name, const char* heading_text, const char* subtitle_text,
                                 int step_width, int picture_width, const char* overlay_image_name) {
        constexpr int visual_height = 195;
        constexpr int heading_height = 36;
        constexpr int subtitle_height = 24;
        auto* step_panel = new wxPanel(m_guide_panel, wxID_ANY, wxDefaultPosition,
                                       wxSize(FromDIP(step_width), FromDIP(visual_height + heading_height + subtitle_height)));
        const int minimum_width = image_name != nullptr ? 148 : 250;
        step_panel->SetMinSize(wxSize(FromDIP(minimum_width),
                                      FromDIP(visual_height + heading_height + subtitle_height)));
        step_panel->SetMaxSize(wxSize(FromDIP(step_width),
                                      FromDIP(visual_height + heading_height + subtitle_height)));
        step_panel->SetBackgroundColour(background_colour());
        auto* step = new wxBoxSizer(wxVERTICAL);
        step_panel->SetSizer(step);

        auto* visual = new wxPanel(step_panel, wxID_ANY, wxDefaultPosition,
                                   wxSize(FromDIP(step_width), FromDIP(visual_height)));
        visual->SetMinSize(wxSize(FromDIP(step_width), FromDIP(visual_height)));
        visual->SetMaxSize(wxSize(FromDIP(step_width), FromDIP(visual_height)));
        visual->SetBackgroundColour(background_colour());
        auto* visual_sizer = new wxBoxSizer(wxVERTICAL);
        visual->SetSizer(visual_sizer);
        visual_sizer->AddStretchSpacer(1);
        if (image_name != nullptr) {
            wxWindow* picture = nullptr;
            if (overlay_image_name != nullptr) {
                picture = new FlowCompositeImage(visual, resource_image(image_name), resource_image(overlay_image_name),
                                                 wxSize(FromDIP(216), FromDIP(177)));
            } else {
                picture = new FlowCardImage(visual, resource_image(image_name),
                                            wxSize(FromDIP(138), FromDIP(177)));
            }
            visual_sizer->Add(picture, 0, wxALIGN_CENTER);
        } else {
            visual_sizer->Add(new FlowPromptCard(visual), 0, wxALIGN_CENTER);
        }
        visual_sizer->AddStretchSpacer(1);
        step->Add(visual, 0, wxEXPAND);

        auto* heading = new wxStaticText(step_panel, wxID_ANY, text(heading_text), wxDefaultPosition,
                                         wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        style_text(heading, primary_text_colour(), 11);
        heading->Wrap(FromDIP(picture_width));
        heading->SetMinSize(wxSize(FromDIP(picture_width), FromDIP(heading_height)));
        heading->SetMaxSize(wxSize(FromDIP(picture_width), FromDIP(heading_height)));
        step->Add(heading, 0, wxALIGN_CENTER);
        auto* subtitle = new wxStaticText(step_panel, wxID_ANY, text(subtitle_text), wxDefaultPosition,
                                          wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        style_text(subtitle, secondary_text_colour(), 9);
        subtitle->Wrap(FromDIP(picture_width));
        subtitle->SetMinSize(wxSize(FromDIP(picture_width), FromDIP(subtitle_height)));
        subtitle->SetMaxSize(wxSize(FromDIP(picture_width), FromDIP(subtitle_height)));
        step->Add(subtitle, 0, wxALIGN_CENTER);
        flow->Add(step_panel, 1, wxEXPAND);
    };
    auto add_arrow = [this, flow] {
        flow->Add(new FlowArrow(m_guide_panel, 98), 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(8));
    };
    add_step(nullptr, "上传图片或输入提示词生成图片", "描述想创作的图片", 276, 250, nullptr);
    add_arrow();
    add_step("redesign_model_blue.png", "生成图片", "生成图片并完善", 256, 138, nullptr);
    add_arrow();
    add_step("redesign_model_mono.png", "转为 3D", "获得可打印的专属 3D 模型", 256, 216, "redesign_model_blue.png");
    guide_sizer->Add(flow, 0, wxALIGN_CENTER);
    guide_sizer->AddStretchSpacer(1);
    content_sizer->Add(m_guide_panel, 1, wxEXPAND);

    m_preview_host = new wxPanel(m_image_page, wxID_ANY);
    m_preview_host->SetBackgroundColour(background_colour());
    auto* preview_sizer = new wxBoxSizer(wxVERTICAL);
    m_preview_host->SetSizer(preview_sizer);
    preview_sizer->AddStretchSpacer(1);
    m_preview = new ImagePreview(m_preview_host);
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
    const wxSize bounds(std::min(FromDIP(503), std::max(1, available.x - FromDIP(80))),
                        std::min(FromDIP(671), std::max(1, available.y - FromDIP(80))));
    if (bounds == m_last_preview_bounds)
        return;
    m_last_preview_bounds = bounds;
    const double scale = std::min(double(bounds.x) / m_selected_image.GetWidth(),
                                  double(bounds.y) / m_selected_image.GetHeight());
    const wxSize image_size(std::min(bounds.x, std::max(1, int(std::round(m_selected_image.GetWidth() * scale)))),
                            std::min(bounds.y, std::max(1, int(std::round(m_selected_image.GetHeight() * scale)))));
    const wxBitmap bitmap(m_selected_image.Scale(image_size.x, image_size.y, wxIMAGE_QUALITY_HIGH));
    m_preview->SetMinSize(bounds);
    m_preview->SetMaxSize(bounds);
    m_preview->SetBitmap(bitmap);
    m_preview->GetParent()->Layout();
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
    m_guide_panel->Show(!ready);
    m_preview_host->Show(ready);
    m_upload_surface->Layout();
    m_upload_surface->GetParent()->Layout();
    m_image_page->Layout();
    if (ready) {
        m_preview_host->Layout();
        update_preview_bitmap();
    }
}

}
