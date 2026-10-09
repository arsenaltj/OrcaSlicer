#pragma once

#include "../GUI_Utils.hpp"

#include <vector>

#include <wx/msgdlg.h>

class wxBoxSizer;
class wxCheckBox;
class wxPanel;
class wxStaticText;
class wxWindow;

namespace Slic3r::GUI {

class RedesignDialogActionButton;
class RedesignDialogCloseButton;

struct RedesignDialogAction {
    wxWindowID id;
    wxString label;
    bool primary { false };
};

struct RedesignConfirmationOptions {
    long style { wxYES_NO };
    int minimum_message_height { 105 };
    std::vector<RedesignDialogAction> actions;
    int dismiss_result { wxID_NONE };
    wxString checkbox_label;
    bool checkbox_checked { false };
    bool* checkbox_state { nullptr };
    bool standard_layout { false };
};

inline int redesign_dialog_dismiss_result(long style)
{
    return (style & wxCANCEL) ? wxID_CANCEL : (style & wxNO) ? wxID_NO : wxID_CANCEL;
}

inline int redesign_dialog_default_result(long style)
{
    return (style & wxCANCEL_DEFAULT) && (style & wxCANCEL) ? wxID_CANCEL :
           (style & wxNO_DEFAULT) && (style & wxNO) ? wxID_NO :
           (style & wxYES) ? wxID_YES : wxID_OK;
}

class RedesignMessageDialog final : public DPIDialog
{
public:
    enum class Appearance { Standard, Compact, GenerationConfirmation };
    RedesignMessageDialog(wxWindow* parent, const wxString& message,
                          const wxString& caption = wxEmptyString, long style = wxOK, bool compact = false);
    RedesignMessageDialog(wxWindow* parent, const wxString& message,
                          const wxString& caption, long style, Appearance appearance,
                          int minimum_message_height = 105,
                          const std::vector<RedesignDialogAction>& actions = {},
                          const wxString& checkbox_label = wxEmptyString,
                          bool checkbox_checked = false);
    bool checkbox_checked() const;
    void set_dismiss_result(int result) { m_cancel_result = result; }

private:
    void add_action_button(wxWindowID id, const wxString& label, bool primary);
    void bind_title_drag(wxWindow* window);
    void finish_with(int result);
    void update_shape();
    void wrap_message();
    void on_dpi_changed(const wxRect& suggested_rect) override;

    int m_cancel_result { wxID_CANCEL };
    int m_default_result { wxID_OK };
    int m_dialog_width { 560 };
    int m_message_width { 408 };
    int m_message_min_height { -1 };
    bool m_stacked_actions { false };
    Appearance m_appearance { Appearance::Standard };
    wxString m_original_message;
    wxPoint m_drag_offset;
    wxWindow* m_drag_source { nullptr };
    wxPanel* m_title_bar { nullptr };
    wxStaticText* m_title { nullptr };
    wxStaticText* m_message { nullptr };
    wxCheckBox* m_checkbox { nullptr };
    wxPanel* m_icon { nullptr };
    RedesignDialogCloseButton* m_close_button { nullptr };
    wxBoxSizer* m_action_sizer { nullptr };
    std::vector<RedesignDialogActionButton*> m_action_buttons;
};

int show_redesign_confirmation(wxWindow* parent, const wxString& message,
                              const wxString& caption, const RedesignConfirmationOptions& options = {});

// Returns wxID_YES only for an affirmative action. Closing/Escape return wxID_NO.
int show_generation_confirmation(wxWindow* parent, const wxString& message,
                                 const wxString& caption, int minimum_message_height = 105);

}
