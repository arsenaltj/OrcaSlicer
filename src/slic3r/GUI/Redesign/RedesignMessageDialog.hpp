#pragma once

#include "../GUI_Utils.hpp"

#include <algorithm>
#include <vector>

#include <wx/msgdlg.h>

class wxBoxSizer;
class wxCheckBox;
class wxPanel;
class wxStaticText;
class wxScrolledWindow;
class wxWindow;

namespace Slic3r::GUI {

class RedesignDialogActionButton;
class RedesignDialogCloseButton;
class RedesignDetailsContentPanel;
class RedesignDetailsViewport;

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
    bool inline_confirmation_footer { false };
    int message_point_size { 0 };
    int action_point_size { 0 };
    bool fit_message_to_content { false };
    int maximum_message_height { 0 };
};

struct RedesignDialogMessageHeights {
    int content { 0 };
    int viewport { 0 };
    bool scrollable { false };
};

inline RedesignDialogMessageHeights redesign_dialog_message_heights(int minimum_height,
                                                                     int measured_height,
                                                                     int maximum_height)
{
    RedesignDialogMessageHeights result;
    result.content = std::max(0, std::max(minimum_height, measured_height));
    result.viewport = maximum_height > 0 ? std::min(result.content, maximum_height) : result.content;
    result.scrollable = result.viewport < result.content;
    return result;
}

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

class RedesignMessageDialog : public DPIDialog
{
public:
    enum class Appearance { Standard, Compact, GenerationConfirmation };
    // Scrolling is shared by confirmation and details dialogs, but their
    // content implementations are intentionally separate.  Confirmation
    // dialogs keep the native wxStaticText layout; only the long details
    // dialog opts into the owner-drawn viewport.
    enum class ContentMode { Native, OwnerDrawnDetails };
    RedesignMessageDialog(wxWindow* parent, const wxString& message,
                          const wxString& caption = wxEmptyString, long style = wxOK, bool compact = false,
                          bool scroll_message = false);
    void set_action_label(wxWindowID id, const wxString& label);
    void enable_action(wxWindowID id, bool enabled);
    RedesignMessageDialog(wxWindow* parent, const wxString& message,
                          const wxString& caption, long style, Appearance appearance,
                          int minimum_message_height = 105, bool scroll_message = false,
                          const std::vector<RedesignDialogAction>& actions = {},
                          const wxString& checkbox_label = wxEmptyString,
                          bool checkbox_checked = false,
                          ContentMode content_mode = ContentMode::Native,
                          bool inline_confirmation_footer = false,
                          int message_point_size = 0,
                          int action_point_size = 0,
                          bool fit_message_to_content = false,
                          int maximum_message_height = 0);
    bool checkbox_checked() const;
    void set_dismiss_result(int result) { m_cancel_result = result; }

private:
    void add_action_button(wxWindowID id, const wxString& label, bool primary);
    void bind_title_drag(wxWindow* window);
    void finish_with(int result);
    int message_text_width() const;
    void fit_message_layout();
    void update_shape();
    void wrap_message();
    void on_dpi_changed(const wxRect& suggested_rect) override;

    int m_cancel_result { wxID_CANCEL };
    int m_default_result { wxID_OK };
    int m_dialog_width { 560 };
    int m_message_width { 408 };
    int m_message_min_height { -1 };
    int m_message_point_size { 0 };
    int m_action_point_size { 0 };
    int m_message_content_height { -1 };
    int m_message_view_height { -1 };
    int m_maximum_message_height { 0 };
    bool m_stacked_actions { false };
    bool m_inline_confirmation_footer { false };
    bool m_fit_message_to_content { false };
    bool m_message_scrollable { false };
    Appearance m_appearance { Appearance::Standard };
    ContentMode m_content_mode { ContentMode::Native };
    wxString m_original_message;
    wxPoint m_drag_offset;
    wxWindow* m_drag_source { nullptr };
    wxPanel* m_title_bar { nullptr };
    wxStaticText* m_title { nullptr };
    wxStaticText* m_message { nullptr };
    wxWindow* m_message_window { nullptr };
    RedesignDetailsContentPanel* m_details_content { nullptr };
    RedesignDetailsViewport* m_details_viewport { nullptr };
    wxScrolledWindow* m_message_view { nullptr };
    wxCheckBox* m_checkbox { nullptr };
    wxPanel* m_icon { nullptr };
    RedesignDialogCloseButton* m_close_button { nullptr };
    wxBoxSizer* m_action_sizer { nullptr };
    std::vector<RedesignDialogActionButton*> m_action_buttons;
};

int show_redesign_confirmation(wxWindow* parent, const wxString& message,
                              const wxString& caption, const RedesignConfirmationOptions& options = {});

// Shows a read-only, scrollable details view using the same rounded dark
// dialog and dimmed backdrop as the redesign workflow. The helper is intended
// for long recommendation summaries and has a single localized close action.
int show_redesign_details(wxWindow* parent, const wxString& message,
                          const wxString& caption);

// Returns wxID_YES only for an affirmative action. Closing/Escape return wxID_NO.
int show_generation_confirmation(wxWindow* parent, const wxString& message,
                                 const wxString& caption, int minimum_message_height = 105);

}
