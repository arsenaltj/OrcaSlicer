#pragma once

#include <functional>
#include <utility>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <wx/window.h>

namespace Slic3r::GUI {

// The workspace stops these subscriptions before wxWindow destroys its children.
class AdvancedCreationWindowEvents {
public:
    AdvancedCreationWindowEvents() = default;
    AdvancedCreationWindowEvents(const AdvancedCreationWindowEvents&) = delete;
    AdvancedCreationWindowEvents& operator=(const AdvancedCreationWindowEvents&) = delete;
    ~AdvancedCreationWindowEvents() { stop(); }

    void bind(wxWindow& page, int timer_id, std::function<void(bool)> shown,
              std::function<void()> dpi_changed, std::function<void()> selection_due)
    {
        stop();
        m_page = &page;
        m_timer_id = timer_id;
        m_shown = std::move(shown);
        m_dpi_changed = std::move(dpi_changed);
        m_selection_due = std::move(selection_due);
        page.Bind(wxEVT_SHOW, &AdvancedCreationWindowEvents::on_show, this);
        page.Bind(wxEVT_DPI_CHANGED, &AdvancedCreationWindowEvents::on_dpi_changed, this);
        page.Bind(wxEVT_TIMER, &AdvancedCreationWindowEvents::on_timer, this, timer_id);
    }

    void stop()
    {
        if (m_page) {
            m_page->Unbind(wxEVT_SHOW, &AdvancedCreationWindowEvents::on_show, this);
            m_page->Unbind(wxEVT_DPI_CHANGED, &AdvancedCreationWindowEvents::on_dpi_changed, this);
            m_page->Unbind(wxEVT_TIMER, &AdvancedCreationWindowEvents::on_timer, this, m_timer_id);
            m_page = nullptr;
        }
        m_shown = {};
        m_dpi_changed = {};
        m_selection_due = {};
    }

private:
    void on_show(wxShowEvent& event) { event.Skip(); if (m_shown) m_shown(event.IsShown()); }
    void on_dpi_changed(wxDPIChangedEvent& event) { event.Skip(); if (m_dpi_changed) m_dpi_changed(); }
    void on_timer(wxTimerEvent&) { if (m_selection_due) m_selection_due(); }

    wxWeakRef<wxWindow> m_page;
    int m_timer_id {wxID_ANY};
    std::function<void(bool)> m_shown;
    std::function<void()> m_dpi_changed;
    std::function<void()> m_selection_due;
};

} // namespace Slic3r::GUI
