#pragma once

#include "PrinterTransferDialogStyle.hpp"
#include "../I18N.hpp"
#include "../Jobs/ProgressIndicator.hpp"

#include <algorithm>
#include <mutex>
#include <wx/dcbuffer.h>
#include <wx/wupdlock.h>

namespace Slic3r::GUI {

inline wxSize printer_transfer_footer_size(wxWindow* window)
{
    return window->FromDIP(wxSize(630, 100));
}

inline void set_printer_transfer_footer_layout(wxWindow* panel, Button* action,
                                               wxWindow* status = nullptr, wxWindow* track = nullptr)
{
    const int padding = panel->FromDIP(12);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    if (track)
        row->Add(track, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, panel->FromDIP(16));
    else
        row->AddStretchSpacer();
    row->Add(action, 0, wxALIGN_CENTER_VERTICAL);
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->AddStretchSpacer();
    if (status)
        layout->Add(status, 0, wxEXPAND | wxLEFT | wxRIGHT, padding);
    else
        layout->AddSpacer(panel->FromDIP(24));
    layout->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, padding);
    layout->AddStretchSpacer();
    panel->SetSizer(layout);
}

class PrinterTransferProgress : public ProgressIndicator
{
public:
    explicit PrinterTransferProgress(wxWindow* parent)
        : m_panel(new wxPanel(parent)),
          m_status(new wxStaticText(m_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                   wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END)),
          m_track(new wxPanel(m_panel)),
          m_action(new Button(m_panel, _L("取消上传")))
    {
        m_panel->SetBackgroundColour(RedesignTheme::panel_colour());
        m_track->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_track->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_track(); });
        RedesignTheme::style_text(m_status, RedesignTheme::primary_text_colour(), 10);
        set_printer_transfer_footer_layout(m_panel, m_action, m_status, m_track);
        m_action->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_pending)
                cancel();
            else if (m_finished)
                wxGetTopLevelParent(m_panel)->Close();
        });
        rescale();
    }

    wxPanel* panel() const { return m_panel; }
    bool showing_transfer() const { return m_pending || m_finished; }
    bool active() const { return m_pending; }

    void rescale()
    {
        m_panel->SetMinSize(printer_transfer_footer_size(m_panel));
        m_status->SetMinSize(m_panel->FromDIP(wxSize(0, 24)));
        m_track->SetMinSize(m_panel->FromDIP(wxSize(110, 26)));
        m_action->Rescale();
        style_printer_transfer_primary_button(m_action);
        m_panel->Layout();
    }

    void reset()
    {
        m_pending = m_finished = m_cancel_requested = false;
        m_cancel = {};
    }

    void begin(CancelFn cancel)
    {
        reset();
        m_pending = true;
        {
            std::lock_guard<std::mutex> lock(m_error_mutex);
            m_error.clear();
        }
        m_range = 100;
        m_value = 0;
        m_action->SetLabel(_L("取消上传"));
        m_action->Enable();
        m_status->SetForegroundColour(RedesignTheme::primary_text_colour());
        set_message(_L("正在准备上传…"));
        m_track->Refresh(false);
        m_cancel = std::move(cancel);
    }

    void cancel()
    {
        if (!m_pending || m_cancel_requested)
            return;
        m_cancel_requested = true;
        m_action->Disable();
        set_message(_L("正在取消上传…"));
        if (m_cancel)
            m_cancel();
    }

    void finish(const wxString& message, bool succeeded = false)
    {
        if (!m_pending)
            return;
        wxWindowUpdateLocker locker(m_panel);
        m_pending = false;
        m_finished = true;
        m_cancel = {};
        if (succeeded)
            m_value = m_range;
        m_status->SetForegroundColour(succeeded ? RedesignTheme::primary_text_colour() : RedesignTheme::accent_colour());
        set_message(message);
        m_action->SetLabel(_L("关闭"));
        m_action->Enable();
        m_track->Refresh(false);
    }

    wxString failure_detail() const
    {
        std::lock_guard<std::mutex> lock(m_error_mutex);
        return m_error;
    }

    void set_status_text(const char* message) override
    {
        if (m_pending && !m_cancel_requested && message && *message)
            set_message(wxString::FromUTF8(message));
    }

    void set_progress(int value) override
    {
        if (!m_pending || m_cancel_requested || value < 0)
            return;
        value = std::min(value, m_range);
        if (m_value != value) {
            m_value = value;
            m_track->Refresh(false);
        }
    }

    int get_range() const override { return m_range; }
    void set_range(int range) override { m_range = std::max(1, range); }
    void clear_percent() override {}
    void set_cancel_callback(CancelFn) override {}
    void show_error_info(wxString message, int, wxString description, wxString) override
    {
        std::lock_guard<std::mutex> lock(m_error_mutex);
        m_error = description.empty() ? message : message + ": " + description;
    }

private:
    void set_message(const wxString& message)
    {
        if (m_status->GetLabelText() != message) {
            m_status->SetLabelText(message);
            m_status->SetToolTip(message);
        }
    }

    void paint_track()
    {
        wxAutoBufferedPaintDC dc(m_track);
        dc.SetBackground(wxBrush(RedesignTheme::panel_colour()));
        dc.Clear();
        const auto size = m_track->GetClientSize();
        const int width = std::max(0, size.x - m_track->FromDIP(50));
        const int height = m_track->FromDIP(6);
        const int top = (size.y - height) / 2;
        const int fill = width * std::min(m_value, m_range) / m_range;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(wxColour(68, 68, 73)));
        dc.DrawRoundedRectangle(0, top, width, height, height / 2.0);
        if (fill > 0) {
            dc.SetBrush(wxBrush(RedesignTheme::accent_colour()));
            dc.DrawRoundedRectangle(0, top, fill, height, std::min(fill, height) / 2.0);
        }
        dc.SetFont(m_status->GetFont());
        dc.SetTextForeground(RedesignTheme::secondary_text_colour());
        dc.DrawLabel(wxString::Format("%d%%", 100 * std::min(m_value, m_range) / m_range),
                     wxRect(width, 0, size.x - width, size.y), wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
    }

    wxPanel* m_panel;
    wxStaticText* m_status;
    wxPanel* m_track;
    Button* m_action;
    CancelFn m_cancel;
    mutable std::mutex m_error_mutex;
    wxString m_error;
    int m_range = 100;
    int m_value = 0;
    bool m_pending = false;
    bool m_finished = false;
    bool m_cancel_requested = false;
};

} // namespace Slic3r::GUI
