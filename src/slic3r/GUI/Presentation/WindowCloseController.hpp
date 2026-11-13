#pragma once

#include <functional>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <wx/toplevel.h>

namespace Slic3r::GUI::Presentation {

// Owns deferred close admission and draining. Project/gizmo confirmations and
// the final native shutdown remain with the frame and are performed once.
class WindowCloseController final : public wxEvtHandler {
public:
    enum class Phase { Idle, Transfer, Presentation, ShuttingDown };
    using Drain = std::function<bool(std::function<void()>)>;
    struct Confirmations {
        std::function<bool()> gizmo_clear, project, transfer_queue;
    };

    WindowCloseController(wxTopLevelWindow& owner, std::function<bool()> request_transfer,
                          std::function<bool()> poll_transfer, std::function<void()> accepted,
                          Drain drain = {});
    ~WindowCloseController() override;
    bool begin(wxCloseEvent& event);
    bool request_close(wxCloseEvent& event);
    void set_confirmations(Confirmations confirmations) { m_confirmations = std::move(confirmations); }
    bool confirmed() const { return m_confirmed; }
    bool drain(wxCloseEvent& event);
    Phase phase() const { return m_phase; }

private:
    void poll(wxTimerEvent&);
    void resume(Phase expected);
    wxWeakRef<wxTopLevelWindow> m_owner;
    wxTimer m_timer;
    std::function<bool()> m_request_transfer, m_poll_transfer;
    std::function<void()> m_accepted;
    Drain m_drain;
    Confirmations m_confirmations;
    Phase m_phase{Phase::Idle};
    bool m_confirmed{false};
};

} // namespace Slic3r::GUI::Presentation
