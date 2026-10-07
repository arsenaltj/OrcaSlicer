#include "StartupSetupDialog.hpp"
#include "StartupSetupService.hpp"
#include "StartupBufferView.hpp"
#include "../FilamentColourPalette.hpp"
#include "../GUI_App.hpp"
#include "../MainFrame.hpp"
#include "../I18N.hpp"
#include "../Widgets/Button.hpp"
#include "../../Utils/ColorSpaceConvert.hpp"

#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <thread>
#include <boost/log/trivial.hpp>
#include <wx/colordlg.h>
#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/evtloop.h>
#include <wx/eventfilter.h>
#include <wx/graphics.h>
#include <wx/popupwin.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/vlbox.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {
namespace {

wxColour setup_face() { return wxColour(32, 32, 34); }
wxColour row_face() { return wxColour(50, 50, 52); }

wxFont setup_font(wxWindow* window, int pixels, wxFontWeight weight = wxFONTWEIGHT_NORMAL)
{
    wxFontInfo info(pixels * 0.75);
    info.Family(wxFONTFAMILY_SWISS).Weight(weight);
    const wxString family = RedesignTheme::font_family();
    if (!family.empty()) info.FaceName(family);
    wxFont font(info);
    font.SetPixelSize(wxSize(0, window->FromDIP(pixels)));
    return font;
}

wxBitmap setup_image(wxWindow* window, const std::string& path, wxSize slot)
{
    wxImage image(wxString::FromUTF8(path));
    if (!image.IsOk()) return wxNullBitmap;
    const wxSize size = window->FromDIP(slot);
    const double scale = std::min(double(size.x) / image.GetWidth(), double(size.y) / image.GetHeight());
    wxBitmap bitmap(image.Scale(std::max(1, int(image.GetWidth() * scale)),
                                std::max(1, int(image.GetHeight() * scale)), wxIMAGE_QUALITY_HIGH));
    bitmap.SetScaleFactor(window->GetDPIScaleFactor());
    return bitmap;
}

struct SetupListItem {
    wxString label;
    wxString tooltip;
    std::string value;
    wxColour swatch;
};

class SetupList final : public wxPanel
{
public:
    SetupList(wxWindow* parent, std::function<void(size_t)> choose)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_choose(std::move(choose))
    {
        SetBackgroundColour(setup_face());
        m_rows = new Rows(this);
        m_bar = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_bar->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_bar->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(m_bar);
            dc.SetBackground(wxBrush(setup_face()));
            dc.Clear();
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(122, 122, 122)));
            dc.DrawRoundedRectangle(thumb(), FromDIP(2));
        });
        m_bar->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            const auto bounds = thumb();
            m_drag_offset = bounds.Contains(event.GetPosition()) ? event.GetY() - bounds.y : bounds.height / 2;
            m_bar->CaptureMouse();
            move_thumb(event.GetY());
        });
        m_bar->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (m_bar->HasCapture()) move_thumb(event.GetY());
        });
        m_bar->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (m_bar->HasCapture()) m_bar->ReleaseMouse();
        });
        m_bar->Bind(wxEVT_MOUSE_CAPTURE_LOST, [](wxMouseCaptureLostEvent&) {});
        m_bar->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
            m_rows->GetEventHandler()->ProcessEvent(event);
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { arrange(); event.Skip(); });
    }

    void set_items(std::vector<SetupListItem> items, int selection)
    {
        m_items = std::move(items);
        m_selection = selection;
        if (m_rows->GetItemCount() != m_items.size()) m_rows->SetItemCount(m_items.size());
        m_rows->SetSelection(selection);
        m_rows->UnsetToolTip();
        if (selection >= 0 && !m_rows->IsVisible(size_t(selection))) m_rows->ScrollToRow(size_t(selection));
        arrange();
        m_rows->RefreshAll();
    }

    int GetSelection() const { return m_rows->GetSelection(); }
    void SetFocus() override { m_rows->SetFocus(); }
    void rescale() { m_rows->SetFont(setup_font(m_rows, 14)); arrange(); m_rows->RefreshAll(); }

private:
    // wxVListBox keeps native keyboard, wheel and virtual-row behavior. Only the
    // scrollbar and selected-row rendering are local to the dark setup surface.
    class Rows final : public wxVListBox
    {
    public:
        explicit Rows(SetupList* owner)
            : wxVListBox(owner, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), m_owner(owner)
        {
            SetBackgroundColour(setup_face());
            SetForegroundColour(wxColour(220, 220, 221));
            SetFont(setup_font(this, 14));
            Bind(wxEVT_LISTBOX, [this](wxCommandEvent& event) { m_owner->m_choose(size_t(event.GetInt())); });
            Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
                const int index = VirtualHitTest(event.GetY());
                if (index != wxNOT_FOUND && size_t(index) < m_owner->m_items.size())
                    SetToolTip(m_owner->m_items[size_t(index)].tooltip);
                else UnsetToolTip();
                event.Skip();
            });
            Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) { UnsetToolTip(); event.Skip(); });
            Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
                if (m_owner->m_bar) m_owner->m_bar->Refresh();
                event.Skip();
            });
        }

        void SetScrollbar(int orientation, int, int, int, bool refresh = true) override
        {
            wxVListBox::SetScrollbar(orientation, 0, 0, 0, refresh);
        }

    protected:
        wxCoord OnMeasureItem(size_t) const override { return FromDIP(32); }
        void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t index) const override
        {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(int(index) == m_owner->m_selection ? row_face() : setup_face()));
            dc.DrawRectangle(rect);
        }
        void OnDrawItem(wxDC& dc, const wxRect& rect, size_t index) const override
        {
            m_owner->draw_item(dc, rect, index);
        }

    private:
        SetupList* m_owner;
    };

    void draw_item(wxDC& dc, const wxRect& rect, size_t index) const
    {
        if (index >= m_items.size()) return;
        const auto& item = m_items[index];
        dc.SetFont(m_rows->GetFont());
        dc.SetTextForeground(m_rows->GetForegroundColour());
        int left = rect.x + FromDIP(16);
        if (item.swatch.IsOk()) {
            dc.SetPen(wxPen(wxColour(115, 115, 117), 1));
            dc.SetBrush(wxBrush(item.swatch));
            dc.DrawCircle(left + FromDIP(8), rect.y + rect.height / 2, FromDIP(7));
            left += FromDIP(26);
        }
        const wxString label = wxControl::Ellipsize(item.label, dc, wxELLIPSIZE_END,
            std::max(1, rect.GetRight() - left - FromDIP(12)));
        dc.DrawText(label, left, rect.y + (rect.height - dc.GetTextExtent(label).y) / 2);
    }

    int visible_count() const { return std::max(1, GetClientSize().y / FromDIP(32)); }

    wxRect thumb() const
    {
        const int track = m_bar->GetClientSize().y;
        const int height = std::min(track, std::max(FromDIP(24),
            int(double(track) * visible_count() / std::max<size_t>(1, m_items.size()))));
        const int range = std::max(1, int(m_items.size()) - visible_count());
        const int top = int(double(track - height) * m_rows->GetVisibleBegin() / range);
        return wxRect(0, std::clamp(top, 0, std::max(0, track - height)), m_bar->GetClientSize().x, height);
    }

    void move_thumb(int y)
    {
        const int range = std::max(0, int(m_items.size()) - visible_count());
        const int travel = std::max(1, m_bar->GetClientSize().y - thumb().height);
        const int row = int(std::lround(double(y - m_drag_offset) * range / travel));
        m_rows->ScrollToRow(size_t(std::clamp(row, 0, range)));
        m_bar->Refresh();
    }

    void arrange()
    {
        if (!m_rows || !m_bar) return;
        const auto size = GetClientSize();
        const bool overflow = int(m_items.size()) > visible_count();
        m_rows->SetSize(0, 0, std::max(1, size.x - (overflow ? FromDIP(6) : 0)), size.y);
        m_bar->SetSize(std::max(0, size.x - FromDIP(5)), FromDIP(2), FromDIP(4),
                       std::max(1, size.y - FromDIP(4)));
        m_bar->Show(overflow);
        m_bar->Refresh();
    }

    Rows* m_rows = nullptr;
    wxPanel* m_bar = nullptr;
    std::function<void(size_t)> m_choose;
    std::vector<SetupListItem> m_items;
    int m_selection = wxNOT_FOUND;
    int m_drag_offset = 0;
};

class SetupRegionMenu final : public wxPopupTransientWindow
{
public:
    SetupRegionMenu(wxWindow* parent, StartupSetupRegion region,
                    std::function<void(StartupSetupRegion)> choose)
        : wxPopupTransientWindow(parent, wxBORDER_NONE)
    {
        SetBackgroundColour(row_face());
        auto* list = new SetupList(this, [this, choose](size_t index) {
            choose(static_cast<StartupSetupRegion>(index));
            close();
        });
        list->set_items({{_L("中国"), _L("中国"), "China", {}},
                         {_L("亚太地区"), _L("亚太地区"), "Asia-Pacific", {}},
                         {_L("欧洲"), _L("欧洲"), "Europe", {}},
                         {_L("北美洲"), _L("北美洲"), "North America", {}}}, int(region));
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(list, 1, wxEXPAND | wxALL, FromDIP(4));
        SetSizer(layout);
        SetClientSize(FromDIP(wxSize(336, 136)));
        list->SetFocus();
        list->Bind(wxEVT_KEY_DOWN, [this, list, choose](wxKeyEvent& event) {
            if (event.GetKeyCode() == WXK_RETURN && list->GetSelection() != wxNOT_FOUND) {
                choose(static_cast<StartupSetupRegion>(list->GetSelection()));
                close();
            } else event.Skip();
        });
    }

protected:
    void OnDismiss() override
    {
        if (!m_closing) { m_closing = true; Destroy(); }
    }

private:
    void close()
    {
        if (m_closing) return;
        m_closing = true;
        Dismiss();
        Destroy();
    }

    bool m_closing = false;
};

wxString region_label(StartupSetupRegion region)
{
    switch (region) {
    case StartupSetupRegion::AsiaPacific: return _L("亚太地区");
    case StartupSetupRegion::Europe: return _L("欧洲");
    case StartupSetupRegion::NorthAmerica: return _L("北美洲");
    default: return _L("中国");
    }
}

class SetupRegionPicker final : public wxControl
{
public:
    SetupRegionPicker(wxWindow* parent, std::function<void(StartupSetupRegion)> choose)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_choose(std::move(choose)), m_arrow(this, "redesign_startup_region_arrows", 9)
    {
        SetName(_L("当前地区"));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(setup_face());
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { SetFocus(); open(); });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_SPACE ||
                event.GetKeyCode() == WXK_DOWN) open();
            else event.Skip();
        });
    }

    void set_region(StartupSetupRegion region) { m_region = region; Refresh(); }
    void rescale() { m_arrow.msw_rescale(); Refresh(); }

private:
    void open()
    {
        auto* menu = new SetupRegionMenu(this, m_region, m_choose);
        menu->Position(ClientToScreen(wxPoint(0, GetSize().y)), wxSize(0, 0));
        menu->Popup();
    }

    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(setup_face()));
        dc.Clear();
        const wxSize size = GetClientSize();
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetPen(HasFocus() ? wxPen(RedesignTheme::accent_colour(), 1) : *wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(row_face()));
        gc->DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(12));
        gc.reset();
        dc.SetFont(setup_font(this, 16));
        dc.SetTextForeground(wxColour(155, 155, 156));
        const auto label = _L("当前地区");
        dc.DrawText(label, FromDIP(12), (size.y - dc.GetTextExtent(label).y) / 2);
        const auto value = region_label(m_region);
        dc.SetFont(setup_font(this, 16, wxFONTWEIGHT_MEDIUM));
        dc.SetTextForeground(*wxWHITE);
        dc.DrawText(value, size.x - FromDIP(36) - dc.GetTextExtent(value).x,
                    (size.y - dc.GetTextExtent(value).y) / 2);
        dc.DrawBitmap(m_arrow.bmp(), size.x - FromDIP(23), (size.y - m_arrow.bmp().GetHeight()) / 2, true);
    }

    StartupSetupRegion m_region = StartupSetupRegion::China;
    std::function<void(StartupSetupRegion)> m_choose;
    ScalableBitmap m_arrow;
};

class SetupPrinterCard final : public wxControl
{
public:
    SetupPrinterCard(wxWindow* parent, std::function<void()> choose)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(setup_face());
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_LEFT_DOWN, [this, choose](wxMouseEvent&) { if (IsEnabled()) { SetFocus(); choose(); } });
        Bind(wxEVT_KEY_DOWN, [choose](wxKeyEvent& event) {
            if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_SPACE) choose();
            else event.Skip();
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { Refresh(); event.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { Refresh(); event.Skip(); });
    }

    void set_printer(const StartupSetupPrinter& printer, bool selected)
    {
        if (m_image_path != printer.image_path || m_scale != GetDPIScaleFactor()) {
            m_image_path = printer.image_path;
            m_scale = GetDPIScaleFactor();
            m_image = setup_image(this, m_image_path, wxSize(135, 139));
        }
        m_label = wxString::FromUTF8(printer.label);
        m_selected = selected;
        SetName(wxString::FromUTF8(printer.model));
        SetToolTip(wxString::FromUTF8(printer.model) + " | 0.4 mm");
        Refresh();
    }

private:
    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(setup_face()));
        dc.Clear();
        const wxSize size = GetClientSize();
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetPen(m_selected || HasFocus() ? wxPen(wxColour(255, 204, 48), FromDIP(1)) : *wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(row_face()));
        gc->DrawRoundedRectangle(FromDIP(0.5), FromDIP(0.5), size.x - FromDIP(1), size.y - FromDIP(1), FromDIP(12));
        gc.reset();
        if (m_image.IsOk())
            dc.DrawBitmap(m_image, (size.x - m_image.GetWidth()) / 2,
                           FromDIP(19) + (FromDIP(139) - m_image.GetHeight()) / 2, true);
        dc.SetFont(setup_font(this, 20, wxFONTWEIGHT_SEMIBOLD));
        dc.SetTextForeground(*wxWHITE);
        const auto label = wxControl::Ellipsize(m_label, dc, wxELLIPSIZE_END, size.x - FromDIP(12));
        dc.DrawText(label, (size.x - dc.GetTextExtent(label).x) / 2, FromDIP(174));
    }

    wxString m_label;
    std::string m_image_path;
    wxBitmap m_image;
    double m_scale = 0;
    bool m_selected = false;
};

class SetupSidebar final : public wxPanel
{
public:
    explicit SetupSidebar(wxWindow* parent) : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(RedesignTheme::flow_background_colour());
        for (const auto& [name, size] : std::array<std::pair<const char*, int>, 7>{{
            {"redesign_startup_assets", 24}, {"redesign_startup_image", 24},
            {"redesign_startup_model", 24}, {"redesign_startup_print", 24},
            {"redesign_startup_notification", 29}, {"redesign_startup_settings", 31},
            {"redesign_startup_help", 31}}})
            m_icons.emplace_back(this, name, size);
        m_avatar = setup_image(this, resources_dir() + "/images/redesign_startup_avatar.png", wxSize(32, 32));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
    }

    void rescale()
    {
        for (auto& icon : m_icons) icon.msw_rescale();
        m_avatar = setup_image(this, resources_dir() + "/images/redesign_startup_avatar.png", wxSize(32, 32));
        Refresh();
    }

private:
    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(RedesignTheme::flow_background_colour()));
        dc.Clear();
        const wxSize size = GetClientSize();
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetPen(wxPen(wxColour(65, 65, 69), FromDIP(1)));
        gc->SetBrush(wxBrush(wxColour(29, 29, 32)));
        gc->DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, FromDIP(12));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(wxColour(116, 116, 119)));
        gc->DrawRoundedRectangle(FromDIP(18), FromDIP(40), FromDIP(48), FromDIP(48), FromDIP(12));
        gc->SetBrush(wxBrush(RedesignTheme::accent_colour()));
        gc->DrawRectangle(0, FromDIP(234), FromDIP(3), FromDIP(70));
        gc.reset();
        const bool compact = size.y < FromDIP(900);
        const std::array<wxString, 4> names = {_L("模型库"), _L("图像"), _L("3D模型"), _L("打印")};
        for (size_t index = 0; index < names.size(); ++index) {
            const int top = FromDIP((compact ? 128 : 152) + int(index) * (compact ? 64 : 94));
            dc.DrawBitmap(m_icons[index].bmp(), (size.x - m_icons[index].bmp().GetWidth()) / 2, top, true);
            dc.SetFont(setup_font(this, 15));
            dc.SetTextForeground(index == 1 ? *wxWHITE : wxColour(131, 131, 133));
            const auto extent = dc.GetTextExtent(names[index]);
            dc.DrawText(names[index], (size.x - extent.x) / 2, top + FromDIP(28));
        }
        if (size.y < FromDIP(620)) return;
        const int bottom_top = size.y - FromDIP(compact ? 224 : 261);
        dc.DrawBitmap(m_avatar, (size.x - m_avatar.GetWidth()) / 2, bottom_top, true);
        for (size_t index = 4; index < m_icons.size(); ++index)
            dc.DrawBitmap(m_icons[index].bmp(), (size.x - m_icons[index].bmp().GetWidth()) / 2,
                           bottom_top + FromDIP((compact ? 55 : 72) + int(index - 4) * (compact ? 64 : 70)), true);
    }

    std::vector<ScalableBitmap> m_icons;
    wxBitmap m_avatar;
};

class StartupSetupPanel final : public wxPanel, public wxEventFilter
{
public:
    StartupSetupPanel(wxTopLevelWindow* parent, std::function<void(bool)> complete)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL)
        , m_service(*wxGetApp().app_config)
        , m_frame(parent)
        , m_complete(std::move(complete))
    {
        SetName(_L("首次配置"));
        SetBackgroundColour(RedesignTheme::flow_background_colour());
        m_backdrop = new StartupBufferView(this, parent, false);
        m_backdrop->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { arrange(); event.Skip(); });
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(m_backdrop, 1, wxEXPAND);
        SetSizer(layout);
        m_content = new wxScrolledWindow(m_backdrop, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                          wxBORDER_NONE | wxHSCROLL | wxVSCROLL | wxTAB_TRAVERSAL);
        m_content->SetBackgroundColour(RedesignTheme::flow_background_colour());
        m_content->SetScrollRate(FromDIP(16), FromDIP(16));
        m_sidebar = new SetupSidebar(m_content);
        m_collapse = new wxStaticBitmap(m_content, wxID_ANY,
            ScalableBitmap(m_content, "redesign_startup_collapse", 133).bmp());
        m_panel = new wxPanel(m_content, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
        m_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_panel->SetBackgroundColour(setup_face());
        m_panel->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_panel(); });
        build_pages();
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { arrange(); event.Skip(); });
#ifdef __WXMSW__
        Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
            m_sidebar->rescale();
            m_region->rescale();
            for (auto* list : {m_heads, m_filaments, m_colours}) list->rescale();
            m_title->SetFont(setup_font(m_title, 32));
            m_welcome->SetFont(setup_font(m_welcome, 20));
            m_error->SetFont(setup_font(m_error, 16));
            for (auto* header : m_header->GetChildren())
                header->SetFont(setup_font(header, 16, wxFONTWEIGHT_MEDIUM));
            m_next->SetFont(setup_font(m_next, 19, wxFONTWEIGHT_MEDIUM));
            m_previous->SetFont(setup_font(m_previous, 19, wxFONTWEIGHT_MEDIUM));
            for (auto* button : {m_next, m_previous}) button->SetCornerRadius(FromDIP(12));
            m_warning->Rescale();
            m_globe->SetBitmap(ScalableBitmap(m_panel, "redesign_startup_globe", 102).bmp());
            m_collapse->SetBitmap(ScalableBitmap(m_content, "redesign_startup_collapse", 133).bmp());
            refresh();
            arrange();
            event.Skip();
        });
#endif
        parent->Bind(wxEVT_CLOSE_WINDOW, &StartupSetupPanel::on_host_close, this);
        parent->Bind(wxEVT_SIZE, &StartupSetupPanel::on_host_size, this);
        parent->Bind(wxEVT_CHAR_HOOK, &StartupSetupPanel::on_host_key, this);
        SetSize(parent->GetClientRect());
        Layout();
        arrange();
        refresh();
        Raise();
        for (wxWindow* sibling : parent->GetChildren()) {
            if (sibling != this && !sibling->IsTopLevel() && sibling->IsShown()) {
                m_hidden_siblings.emplace_back(sibling);
                sibling->Hide();
            }
        }
        wxEvtHandler::AddFilter(this);
        m_next->SetFocus();
        load_catalog();
    }

    ~StartupSetupPanel() override
    {
        *m_cancel = true;
        if (m_worker.joinable()) m_worker.join();
        wxEvtHandler::RemoveFilter(this);
        if (m_frame && !m_frame->IsBeingDeleted()) {
            m_frame->Unbind(wxEVT_CLOSE_WINDOW, &StartupSetupPanel::on_host_close, this);
            m_frame->Unbind(wxEVT_SIZE, &StartupSetupPanel::on_host_size, this);
            m_frame->Unbind(wxEVT_CHAR_HOOK, &StartupSetupPanel::on_host_key, this);
        }
        if (m_frame && !m_frame->IsBeingDeleted()) {
            for (const auto& sibling : m_hidden_siblings)
                if (sibling && !sibling->IsBeingDeleted()) sibling->Show();
            m_frame->Layout();
            m_frame->Refresh();
        }
        if (!m_finished) {
            m_service.cancel();
            m_complete(false);
        }
    }

    int FilterEvent(wxEvent& event) override
    {
        if (!m_finished && IsShownOnScreen() && event.GetEventType() == wxEVT_MENU && event.GetId() != wxID_EXIT)
            return wxEventFilter::Event_Processed;
        return wxEventFilter::Event_Skip;
    }

private:
    void on_host_size(wxSizeEvent& event)
    {
        SetSize(m_frame->GetClientRect());
        Layout();
        Raise();
        event.Skip();
    }

    void on_host_close(wxCloseEvent& event)
    {
        if (event.CanVeto()) {
            event.Veto();
            cancel();
        } else {
            finish(false);
            event.Skip();
        }
    }

    void on_host_key(wxKeyEvent& event)
    {
        if (event.GetKeyCode() == WXK_ESCAPE || (event.CmdDown() && event.GetKeyCode() == 'Q'))
            cancel();
        else if (!event.CmdDown())
            event.Skip();
    }

    void finish(bool applied)
    {
        if (m_finished) return;
        m_finished = true;
        *m_cancel = true;
        m_complete(applied);
    }

    wxStaticText* label(const wxString& text, int pixels, const wxColour& colour = *wxWHITE)
    {
        auto* control = new wxStaticText(m_panel, wxID_ANY, text, wxDefaultPosition, wxDefaultSize,
                                         wxALIGN_CENTER_HORIZONTAL | wxST_NO_AUTORESIZE);
        control->SetForegroundColour(colour);
        control->SetBackgroundColour(setup_face());
        control->SetFont(setup_font(control, pixels));
        return control;
    }

    Button* button(const wxString& text, bool primary, int pixels = 19)
    {
        auto* control = new Button(m_panel, text, wxEmptyString, wxBORDER_NONE);
        control->SetBackgroundColour(setup_face());
        control->SetCornerRadius(FromDIP(12));
        control->SetBorderWidth(0);
        control->SetPaddingSize(wxSize(0, 0));
        control->SetFont(setup_font(control, pixels, wxFONTWEIGHT_MEDIUM));
        control->SetTextColor(StateColor(
            std::make_pair(primary ? *wxBLACK : *wxWHITE, int(StateColor::Normal))));
        const wxColour normal = primary ? RedesignTheme::accent_colour() : wxColour(77, 77, 78);
        control->SetBackgroundColor(StateColor(
            std::make_pair(primary ? wxColour(105, 94, 48) : wxColour(61, 61, 63), int(StateColor::Disabled)),
            std::make_pair(primary ? wxColour(237, 174, 20) : wxColour(68, 68, 70), int(StateColor::Pressed)),
            std::make_pair(primary ? wxColour(254, 212, 69) : wxColour(87, 87, 89), int(StateColor::Hovered)),
            std::make_pair(normal, int(StateColor::Normal))));
        return control;
    }

    void build_pages()
    {
        m_title = label(_L("欢迎使用 ----"), 32, RedesignTheme::accent_colour());
        m_welcome = label(_L("---- 需要几步安装步骤，让我们开始吧！"), 20, wxColour(117, 117, 118));
        m_globe = new wxStaticBitmap(m_panel, wxID_ANY, ScalableBitmap(m_panel, "redesign_startup_globe", 102).bmp());
        m_region = new SetupRegionPicker(m_panel, [this](StartupSetupRegion region) {
            m_service.choose_region(region);
            refresh();
        });
        for (size_t index = 0; index < m_cards.size(); ++index)
            m_cards[index] = new SetupPrinterCard(m_panel, [this, index] {
                const auto state = m_service.snapshot();
                if (index < state.printers.size()) m_service.choose_printer(state.printers[index].model);
                refresh();
            });
        m_heads = new SetupList(m_panel, [this](size_t index) { m_service.choose_head(index); refresh(); });
        m_filaments = new SetupList(m_panel, [this](size_t index) {
            const auto state = m_service.snapshot();
            const auto* printer = selected_printer(state);
            if (printer && index < printer->filaments.size()) m_service.choose_filament(printer->filaments[index].preset);
            refresh();
        });
        m_colours = new SetupList(m_panel, [this](size_t index) {
            if (index < m_colour_values.size()) m_service.choose_colour(m_colour_values[index]);
            else custom_colour();
            refresh();
        });
        m_header = new wxPanel(m_panel, wxID_ANY);
        m_header->SetBackgroundColour(row_face());
        auto* headers = new wxBoxSizer(wxHORIZONTAL);
        for (const auto& text : {_L("喷头"), _L("耗材类型"), _L("颜色")}) {
            auto* header = new wxStaticText(m_header, wxID_ANY, text);
            header->SetFont(setup_font(header, 16, wxFONTWEIGHT_MEDIUM));
            header->SetForegroundColour(*wxWHITE);
            headers->Add(header, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(16));
        }
        m_header->SetSizer(headers);
        m_error = label(wxEmptyString, 16);
        m_warning = new Button(m_panel, wxEmptyString, "redesign_startup_warning", wxBORDER_NONE, 56);
        m_warning->SetBackgroundColour(setup_face());
        m_warning->SetBackgroundColor(StateColor(std::make_pair(setup_face(), int(StateColor::Normal))));
        m_warning->SetBorderWidth(0);
        m_warning->SetPaddingSize(wxSize(0, 0));
        m_warning->SetToolTip(_L("重新加载本地配置"));
        m_warning->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { load_catalog(); });
        // Match the rendered text size of Figma's scaled button instance.
        m_previous = button(_L("上一步"), false);
        m_previous->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_service.back(); refresh(); });
        m_next = button(_L("开始"), true);
        m_next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { advance(); });
    }

    static const StartupSetupPrinter* selected_printer(const StartupSetupSnapshot& state)
    {
        auto it = std::find_if(state.printers.begin(), state.printers.end(),
                               [&](const auto& printer) { return printer.model == state.draft.printer_model; });
        return it == state.printers.end() ? nullptr : &*it;
    }

    void load_catalog()
    {
        if (m_service.snapshot().loading) return;
        if (m_worker.joinable()) m_worker.join();
        const auto revision = m_service.begin_load();
        refresh();
        const std::string resources = resources_dir();
        const std::string data = data_dir();
        const auto cancel_token = m_cancel;
        const wxWeakRef<StartupSetupPanel> weak(this);
        m_worker = std::thread([resources, data, cancel_token, weak, revision] {
            std::vector<StartupSetupPrinter> printers;
            std::string error;
            try {
                printers = StartupSetupService::load_catalog(resources, data, *cancel_token);
            } catch (const std::exception& exception) {
                error = exception.what();
                PresetBundle empty;
                printers = StartupSetupService::catalog_from_bundle(empty, resources);
            }
            if (*cancel_token) return;
            wxTheApp->CallAfter([cancel_token, weak, revision, printers = std::move(printers), error = std::move(error)]() mutable {
                if (*cancel_token || !weak || weak->IsBeingDeleted()) return;
                if (weak->m_service.finish_load(revision, std::move(printers), std::move(error))) {
                    const auto state = weak->m_service.snapshot();
                    BOOST_LOG_TRIVIAL(info) << "[StartupSetup] Catalog loaded printers=" << state.printers.size()
                                            << " revision=" << revision << " error=" << state.error;
                    weak->refresh();
                }
            });
        });
    }

    void custom_colour()
    {
        const auto state = m_service.snapshot();
        if (state.active_head >= state.draft.heads.size()) return;
        wxColourData colours;
        colours.SetChooseFull(true);
        colours.SetColour(wxColour(state.draft.heads[state.active_head].colour));
        for (size_t index = 0; index < std::min<size_t>(16, state.draft.custom_colours.size()); ++index) {
            const auto& value = state.draft.custom_colours[index];
            const wxColour colour = value.find(',') == std::string::npos ? wxColour(value) : string_to_wxColor(value);
            if (colour.IsOk()) colours.SetCustomColour(int(index), colour);
        }
        wxColourDialog dialog(this, &colours);
        dialog.SetTitle(_L("Please choose the filament colour"));
        if (dialog.ShowModal() != wxID_OK) return;
        const auto chosen = dialog.GetColourData();
        const auto hex = chosen.GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
        m_service.choose_colour(hex);
        std::vector<std::string> custom;
        for (int index = 0; index < 16; ++index)
            custom.push_back(color_to_string(chosen.GetCustomColour(index)));
        m_service.set_custom_colours(std::move(custom));
    }

    void advance()
    {
        if (m_service.snapshot().page != StartupSetupPage::Filaments) {
            m_service.next();
            refresh();
            return;
        }
        if (!m_service.snapshot().can_finish) return;
        m_next->Enable(false);
        m_previous->Enable(false);
        m_next->SetLabel(_L("正在保存…"));
        m_next->Update();
        if (m_service.complete(*wxGetApp().app_config, *wxGetApp().preset_bundle)) {
            BOOST_LOG_TRIVIAL(info) << "[StartupSetup] Configuration saved printer="
                                    << m_service.snapshot().draft.printer_model
                                    << " heads=" << m_service.snapshot().draft.heads.size();
            finish(true);
        } else {
            BOOST_LOG_TRIVIAL(error) << "[StartupSetup] Configuration failed: " << m_service.snapshot().error;
            refresh();
        }
    }

    void cancel()
    {
        if (m_service.snapshot().submitting) return;
        *m_cancel = true;
        m_service.cancel();
        finish(false);
    }

    void refresh()
    {
        const auto state = m_service.snapshot();
        const bool welcome = state.page == StartupSetupPage::Welcome;
        const bool region = state.page == StartupSetupPage::Region;
        const bool printers = state.page == StartupSetupPage::Printers;
        const bool filaments = state.page == StartupSetupPage::Filaments;
        const auto* printer = selected_printer(state);
        const bool failed = filaments && (!printer || !printer->error.empty() || !state.error.empty());
        m_title->SetLabel(welcome ? _L("欢迎使用 ----") : region ? _L("选择地区") :
                           printers ? _L("选择打印机") : _L("选择耗材"));
        m_welcome->Show(welcome);
        m_globe->Show(region);
        m_region->Show(region);
        m_region->set_region(state.draft.region);
        for (size_t index = 0; index < m_cards.size(); ++index) {
            m_cards[index]->Show(printers && !state.loading);
            m_cards[index]->Enable(!state.loading && !state.submitting);
            if (index < state.printers.size()) m_cards[index]->set_printer(state.printers[index],
                state.printers[index].model == state.draft.printer_model);
        }
        m_header->Show(filaments);
        m_heads->Show(filaments && !failed && !state.loading);
        m_filaments->Show(filaments && !failed && !state.loading);
        m_colours->Show(filaments && !failed && !state.loading);
        m_warning->Show(failed && !state.loading);
        m_error->Show(failed || (state.loading && (filaments || printers)));
        if (state.loading)
            m_error->SetLabel(_L("正在加载本地配置…"));
        else if (failed) {
            const auto reason = !state.error.empty() ? state.error : printer ? printer->error : "Missing printer selection.";
            m_error->SetLabel(_L("配置不可用") + "\n" + wxString::FromUTF8(reason));
            m_error->Wrap(FromDIP(592));
        }
        if (filaments && printer && !failed) {
            std::vector<SetupListItem> heads, materials, colours;
            for (size_t index = 0; index < state.draft.heads.size(); ++index) {
                const auto& head = state.draft.heads[index];
                heads.push_back({wxString::Format(_L("喷头 %d"), int(index + 1)),
                    wxString::FromUTF8(head.filament) + " | " + wxString::FromUTF8(head.colour), std::to_string(index), {}});
            }
            const auto& head = state.draft.heads[state.active_head];
            int selected_material = wxNOT_FOUND;
            for (size_t index = 0; index < printer->filaments.size(); ++index) {
                const auto& filament = printer->filaments[index];
                if (filament.preset == head.filament) selected_material = int(index);
                materials.push_back({wxString::FromUTF8(filament.label), wxString::FromUTF8(filament.preset), filament.preset, {}});
            }
            m_colour_values.clear();
            // Both the existing colour picker and setup use Orca's stock colour palette.
            for (const char* value : default_filament_colour_palette) m_colour_values.emplace_back(value);
            if (std::find(m_colour_values.begin(), m_colour_values.end(), head.colour) == m_colour_values.end())
                m_colour_values.insert(m_colour_values.begin(), head.colour);
            int selected_colour = wxNOT_FOUND;
            for (size_t index = 0; index < m_colour_values.size(); ++index) {
                const auto& value = m_colour_values[index];
                if (value == head.colour) selected_colour = int(index);
                colours.push_back({wxString::FromUTF8(value), wxString::FromUTF8(value), value, wxColour(value)});
            }
            colours.push_back({_L("自定义颜色…"), _L("自定义颜色…"), "", {}});
            m_heads->set_items(std::move(heads), int(state.active_head));
            m_filaments->set_items(std::move(materials), selected_material);
            m_colours->set_items(std::move(colours), selected_colour);
        }
        m_previous->Show(!welcome && !region);
        m_previous->Enable(!state.submitting);
        m_next->SetLabel(welcome ? _L("开始") : filaments ? _L("完成") : _L("下一步"));
        m_next->Enable(filaments ? state.can_finish : state.can_next);
        arrange_panel();
        m_panel->Refresh();
        BOOST_LOG_TRIVIAL(debug) << "[StartupSetup] Page=" << int(state.page)
                                << " loading=" << state.loading << " can_finish=" << state.can_finish;
    }

    void place(wxWindow* control, int x, int y, int width, int height)
    {
        control->SetSize(wxRect(m_panel->FromDIP(wxPoint(x, y)), m_panel->FromDIP(wxSize(width, height))));
    }

    void arrange_panel()
    {
        const auto state = m_service.snapshot();
        const bool welcome = state.page == StartupSetupPage::Welcome;
        const bool region = state.page == StartupSetupPage::Region;
        place(m_title, 16, welcome ? 232 : 32, 660, 48);
        place(m_welcome, 24, 290, 644, 60);
        place(m_globe, 295, 125, 102, 102);
        place(m_region, 178, 260, 336, 56);
        for (size_t index = 0; index < m_cards.size(); ++index)
            place(m_cards[index], 106 + int(index) * 165, 126, 149, 216);
        place(m_header, 24, 98, 644, 32);
        place(m_heads, 25, 140, 212, 239);
        place(m_filaments, 239, 140, 213, 239);
        place(m_colours, 454, 140, 213, 239);
        place(m_warning, 318, 198, 56, 56);
        place(m_error, 50, state.loading ? 222 : 272, 592, 100);
        place(m_previous, 134, 404, 200, 48);
        place(m_next, welcome ? 246 : region ? 251 : 358, welcome ? 388 : 404, 200, 48);
        m_next->SetMinSize(FromDIP(wxSize(200, 48)));
        m_next->SetMaxSize(FromDIP(wxSize(200, 48)));
        m_previous->SetMinSize(FromDIP(wxSize(200, 48)));
        m_previous->SetMaxSize(FromDIP(wxSize(200, 48)));
    }

    void arrange()
    {
        if (!m_content || !m_panel) return;
        const auto bounds = m_backdrop->GetClientSize();
        m_content->SetSize(0, FromDIP(40), bounds.x, std::max(1, bounds.y - FromDIP(40)));
        const wxSize virtual_size(std::max(bounds.x, FromDIP(920)),
                                   std::max(bounds.y - FromDIP(40), FromDIP(540)));
        m_content->SetVirtualSize(virtual_size);
        m_sidebar->SetSize(FromDIP(16), FromDIP(8), FromDIP(89), virtual_size.y - FromDIP(24));
        m_collapse->SetSize(virtual_size.x - FromDIP(25), (virtual_size.y - FromDIP(133)) / 2,
                             FromDIP(25), FromDIP(133));
        const auto panel_size = FromDIP(wxSize(692, 500));
        m_panel->SetSize((virtual_size.x - panel_size.x) / 2,
                         (virtual_size.y + FromDIP(40) - panel_size.y) / 2 - FromDIP(40), panel_size.x, panel_size.y);
        arrange_panel();
        m_panel->Raise();
    }

    void paint_panel()
    {
        wxAutoBufferedPaintDC dc(m_panel);
        dc.SetBackground(wxBrush(RedesignTheme::flow_background_colour()));
        dc.Clear();
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc) return;
        const auto size = m_panel->GetClientSize();
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(setup_face()));
        gc->DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(16));
        if (m_service.snapshot().page == StartupSetupPage::Welcome) {
            gc->SetBrush(wxBrush(wxColour(217, 217, 217)));
            gc->DrawRoundedRectangle(FromDIP(286), FromDIP(72), FromDIP(120), FromDIP(120), FromDIP(24));
        } else if (m_service.snapshot().page == StartupSetupPage::Filaments) {
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->SetPen(wxPen(wxColour(62, 62, 62), FromDIP(1)));
            gc->DrawRectangle(FromDIP(24), FromDIP(130), FromDIP(644), FromDIP(250));
            if (!m_warning->IsShown()) {
                gc->StrokeLine(FromDIP(238), FromDIP(130), FromDIP(238), FromDIP(380));
                gc->StrokeLine(FromDIP(453), FromDIP(130), FromDIP(453), FromDIP(380));
            }
        }
    }

    StartupSetupService m_service;
    wxWeakRef<wxTopLevelWindow> m_frame;
    std::function<void(bool)> m_complete;
    bool m_finished = false;
    std::vector<wxWeakRef<wxWindow>> m_hidden_siblings;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
    std::thread m_worker;
    StartupBufferView* m_backdrop = nullptr;
    wxScrolledWindow* m_content = nullptr;
    SetupSidebar* m_sidebar = nullptr;
    wxStaticBitmap* m_collapse = nullptr;
    wxPanel* m_panel = nullptr;
    wxStaticText* m_title = nullptr;
    wxStaticText* m_welcome = nullptr;
    wxStaticBitmap* m_globe = nullptr;
    SetupRegionPicker* m_region = nullptr;
    std::array<SetupPrinterCard*, 3> m_cards{};
    wxPanel* m_header = nullptr;
    SetupList* m_heads = nullptr;
    SetupList* m_filaments = nullptr;
    SetupList* m_colours = nullptr;
    wxStaticText* m_error = nullptr;
    Button* m_warning = nullptr;
    Button* m_previous = nullptr;
    Button* m_next = nullptr;
    std::vector<std::string> m_colour_values;
};

}

bool run_startup_setup(wxWindow* parent)
{
    auto* frame = wxDynamicCast(parent, wxTopLevelWindow);
    if (!frame || frame->IsBeingDeleted()) return false;
    wxEventLoop loop;
    bool applied = false;
    wxWeakRef<StartupSetupPanel> panel(new StartupSetupPanel(frame, [&](bool saved) {
        applied = saved;
        if (loop.IsRunning()) loop.Exit();
    }));
    // Keep the existing startup continuation ordered behind setup completion.
    wxEventLoopActivator activate(&loop);
    loop.Run();
    if (panel) {
        panel->Hide();
        delete panel.get();
    }
    if (applied) wxGetApp().update_mode();
    return applied;
}

}
