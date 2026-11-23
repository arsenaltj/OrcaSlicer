#pragma once
#include <wx/popupwin.h>
#include <wx/timer.h>
#include <chrono>
#include <cstdint>
#include <vector>

class wxGraphicsContext;

namespace Slic3r::GUI {

// A queued event from an earlier presentation must respect the latest interval.
class GenerationToastTiming {
public:
    using Clock = std::chrono::steady_clock;
    void restart(Clock::time_point now) { m_started = now; m_active = true; }
    void cancel() { m_active = false; }
    int remaining_ms(Clock::time_point now) const {
        if (!m_active) return 0;
        const auto remaining = m_started + std::chrono::seconds(3) - now;
        return remaining > Clock::duration::zero() ?
            static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count()) : 0;
    }
    double elapsed_ms(Clock::time_point now) const {
        return std::chrono::duration<double, std::milli>(now - m_started).count();
    }
private:
    Clock::time_point m_started;
    bool m_active {false};
};

template<class Measure>
std::vector<wxString> generation_toast_lines(const wxString& message, int width, Measure measure) {
    std::vector<wxString> lines;
    wxString line;
    for (auto character : message) {
        if (character == '\r') continue;
        if (character == '\n') { lines.push_back(line); line.clear(); continue; }
        if (!line.empty() && measure(line + wxString(character)) > width) {
            lines.push_back(line); line.clear();
        }
        line += character;
    }
    lines.push_back(line);
    return lines;
}

class GenerationCompletionToast final : public wxPopupWindow {
public:
    explicit GenerationCompletionToast(wxWindow* owner);
    void present();
    void present_error(const wxString& message);
    void dismiss();
    void position();
private:
    bool AcceptsFocus() const override { return false; }
    bool AcceptsFocusFromKeyboard() const override { return false; }
    void update_layout();
    void show_for_interval();
    void trace(const char* action) const;
    void draw_bubble(wxGraphicsContext& gc) const;
    void update_native_surface();
    wxWindow* m_owner;
    wxTimer m_timer;
    GenerationToastTiming m_timing;
    unsigned m_presentation {0};
    wxBitmap m_icon;
    bool m_error {false};
    wxString m_message;
    std::vector<wxString> m_lines;
    wxSize m_body;
    bool m_layered {false};
    std::intptr_t m_plain_extended_style {0};
};
}
