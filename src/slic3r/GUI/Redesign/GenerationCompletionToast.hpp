#pragma once
#include <wx/popupwin.h>
#include <wx/timer.h>

namespace Slic3r::GUI {
class GenerationCompletionToast final : public wxPopupWindow {
public:
    explicit GenerationCompletionToast(wxWindow* owner);
    void present();
    void dismiss();
    void position();
private:
    wxWindow* m_owner;
    wxTimer m_timer;
    wxBitmap m_icon;
};
}
