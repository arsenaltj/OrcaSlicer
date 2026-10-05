#pragma once
#include "slic3r/GUI/AI/AIWindowAppearance.hpp"

#include <functional>
#include <cstdint>
#include <cstddef>
#include <wx/panel.h>
#include <wx/timer.h>

class wxButton;
class Button;
class wxCheckBox;
class ComboBox;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r::GUI {
class Plater;

class OrcaModelPreparationPanel final : public wxPanel, public AIThemeOwner
{
public:
    OrcaModelPreparationPanel(wxWindow* parent, Plater& plater,
                              std::function<bool()> busy, std::function<void()> changed,
                              std::function<void()> expanded = {});
    void apply_ai_theme(bool update_fonts) override;
    void suggest_base_for_object(uint64_t object_id);
    // Native Plater selection events update the displayed object before another
    // preparation input can be accepted. The timer remains a fallback for busy state.
    void refresh_selection() { refresh(); }
private:
    int editable_object(wxString& reason) const;
    void refresh();
    void apply();
    void show_feedback(const wxString& message);
    Plater& m_plater;
    std::function<bool()> m_busy;
    std::function<void()> m_changed;
    std::function<void()> m_expanded;
    wxStaticText* m_context;
    wxStaticText* m_feedback;
    wxTextCtrl* m_height;
    wxCheckBox* m_base;
    ComboBox* m_base_template;
    Button* m_apply;
    uint64_t m_displayed_object_id {0};
    uint64_t m_requested_base_object_id {0};
    double m_displayed_height_mm {0};
    std::size_t m_displayed_volume_count {0};
    wxString m_feedback_text;
    wxTimer m_timer;
};
} // namespace Slic3r::GUI
