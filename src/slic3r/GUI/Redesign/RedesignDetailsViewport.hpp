#pragma once

#include <wx/panel.h>
#include <wx/scrolwin.h>

namespace Slic3r::GUI {

// A scrolled body with a theme-controlled thumb. The native window still
// owns the scroll position and keyboard and wheel behavior.
class RedesignDetailsViewport final : public wxPanel
{
public:
    RedesignDetailsViewport(wxWindow* parent, wxWindowID id,
                            const wxPoint& position, const wxSize& size, long style,
                            const wxColour& background, bool draw_details = false);

    wxScrolledWindow* scroller() const;
    void FitInside();
    void SetContentExtent(const wxSize& extent);

private:
    wxRect thumb_rect() const;
    void paint_bar();
    void begin_bar_pointer(wxMouseEvent& event);
    void drag_bar_pointer(wxMouseEvent& event);
    void end_bar_pointer();
    void layout_children();
    void update_bar();

    wxScrolledWindow* m_scroller {nullptr};
    wxPanel* m_bar {nullptr};
    int m_drag_offset {0};
    bool m_dragging {false};
};

} // namespace Slic3r::GUI
