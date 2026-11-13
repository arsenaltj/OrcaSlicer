#include "WindowCloseController.hpp"
#include "UiRuntime.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r::GUI::Presentation {

WindowCloseController::WindowCloseController(wxTopLevelWindow& owner,
    std::function<bool()> request_transfer, std::function<bool()> poll_transfer,
    std::function<void()> accepted, Drain drain)
    : m_owner(&owner), m_timer(this), m_request_transfer(std::move(request_transfer)),
      m_poll_transfer(std::move(poll_transfer)), m_accepted(std::move(accepted)),
      m_drain(drain ? std::move(drain) : Drain([](std::function<void()> done) {
          return background_tasks().request_shutdown(std::move(done));
      }))
{
    Bind(wxEVT_TIMER, &WindowCloseController::poll, this, m_timer.GetId());
}

WindowCloseController::~WindowCloseController() { m_timer.Stop(); }

bool WindowCloseController::begin(wxCloseEvent& event)
{
    assert_gui_thread();
    BOOST_LOG_TRIVIAL(info) << "[AppShutdown] close_received phase=" << int(m_phase)
                            << " can_veto=" << event.CanVeto();
    if (m_phase == Phase::Idle) return true;
    BOOST_LOG_TRIVIAL(info) << "[AppShutdown] duplicate_close_ignored";
    if (event.CanVeto()) event.Veto();
    else event.Skip(false);
    return false;
}

bool WindowCloseController::drain(wxCloseEvent& event)
{
    assert_gui_thread();
    if (!m_confirmed) {
        m_confirmed = true;
        if (m_accepted) m_accepted();
    }
    if (m_request_transfer && !m_request_transfer()) {
        if (event.CanVeto()) {
            m_phase = Phase::Transfer;
            m_timer.Start(50);
            event.Veto();
            BOOST_LOG_TRIVIAL(info) << "[AppShutdown] close_deferred reason=printer_transfer";
            return false;
        }
        BOOST_LOG_TRIVIAL(warning) << "[AppShutdown] close_cannot_defer reason=non_vetoable_event";
    }
    m_phase = Phase::Presentation;
    wxWeakRef<WindowCloseController> lifetime(this);
    if (!m_drain([lifetime] { if (lifetime) lifetime->resume(Phase::Presentation); })) {
        if (event.CanVeto()) event.Veto();
        else event.Skip(false);
        BOOST_LOG_TRIVIAL(info) << "[AppShutdown] close_deferred reason=presentation_tasks";
        return false;
    }
    m_phase = Phase::ShuttingDown;
    return true;
}

bool WindowCloseController::request_close(wxCloseEvent& event)
{
    if (!begin(event)) return false;
    if (!m_confirmed && event.CanVeto()) {
        for (const auto* check : {&m_confirmations.gizmo_clear, &m_confirmations.project, &m_confirmations.transfer_queue}) {
            if (*check && !(*check)()) {
                event.Veto();
                BOOST_LOG_TRIVIAL(info) << "[AppShutdown] close_cancelled reason=confirmation";
                return false;
            }
        }
    }
    return drain(event);
}

void WindowCloseController::poll(wxTimerEvent&)
{
    if (m_phase != Phase::Transfer) return;
    if (m_poll_transfer && !m_poll_transfer()) return;
    m_timer.Stop();
    resume(Phase::Transfer);
}

void WindowCloseController::resume(Phase expected)
{
    if (m_phase != expected || !m_owner) return;
    m_phase = Phase::Idle;
    m_owner->Close(false);
}

} // namespace Slic3r::GUI::Presentation
