#pragma once

#include "../GUI_Utils.hpp"

#include <vector>

#include <wx/msgdlg.h>

class wxBoxSizer;
class wxPanel;
class wxStaticText;
class wxScrolledWindow;
class wxWindow;

namespace Slic3r::GUI {

class RedesignDialogActionButton;
class RedesignDialogCloseButton;

class RedesignMessageDialog : public DPIDialog
{
public:
    RedesignMessageDialog(wxWindow* parent, const wxString& message,
                          const wxString& caption = wxEmptyString, long style = wxOK, bool compact = false,
                          bool scroll_message = false);
    void set_action_label(wxWindowID id, const wxString& label);
    void enable_action(wxWindowID id, bool enabled);

private:
    void add_action_button(wxWindowID id, const wxString& label, bool primary);
    void bind_title_drag(wxWindow* window);
    void finish_with(int result);
    void update_shape();
    void on_dpi_changed(const wxRect& suggested_rect) override;

    int m_cancel_result { wxID_CANCEL };
    int m_default_result { wxID_OK };
    int m_dialog_width { 560 };
    int m_message_width { 408 };
    wxPoint m_drag_offset;
    wxWindow* m_drag_source { nullptr };
    wxPanel* m_title_bar { nullptr };
    wxStaticText* m_title { nullptr };
    wxStaticText* m_message { nullptr };
    wxScrolledWindow* m_message_view { nullptr };
    wxPanel* m_icon { nullptr };
    RedesignDialogCloseButton* m_close_button { nullptr };
    wxBoxSizer* m_action_sizer { nullptr };
    std::vector<RedesignDialogActionButton*> m_action_buttons;
};

}
