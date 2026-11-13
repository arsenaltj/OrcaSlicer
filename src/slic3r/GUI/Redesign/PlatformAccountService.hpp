#pragma once
#include "slic3r/Utils/HonorAccount/HonorSession.hpp"
#include <wx/timer.h>
#include <map>
#include <vector>
#include "../Presentation/UiRuntime.hpp"

namespace Slic3r::GUI {
// Application lifetime, independent of which account/library page is visible.
class PlatformAccountService final : public wxEvtHandler {
public:
    PlatformAccountService(std::unique_ptr<HonorAccount::Store> store,
        std::unique_ptr<HonorAccount::Store> configuration,
        HonorAccount::Transport transport = HonorAccount::https_request)
        : m_session(std::move(store), std::move(transport), std::move(configuration),
            [](std::function<void()> operation) {
                Presentation::TaskContext context;
                context.lifetime = Presentation::TaskLifetime::Session;
                context.identity = "account-request";
                return Presentation::background_tasks().submit(context,
                    [operation = std::move(operation)](const Presentation::TaskContext&) { operation(); return true; },
                    [](Presentation::TaskResult<bool>) {});
            }), m_timer(this)
    {
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); }, m_timer.GetId());
        Presentation::background_tasks().on_shutdown(this, [this] {
            m_timer.Stop(); m_listeners.clear(); m_session.stop_accepting();
        });
    }
    ~PlatformAccountService() override {
        Presentation::background_tasks().remove_shutdown_hook(this); m_timer.Stop();
    }
    HonorAccount::Session& session() { return m_session; }
    void start() {
        if (m_started) return;
        m_started = true; m_session.restore(); tick(); m_timer.Start(100);
    }
    void subscribe(void* owner, std::function<void()> changed) { m_listeners[owner] = std::move(changed); }
    void unsubscribe(void* owner) { m_listeners.erase(owner); }
    std::uint64_t revision() const { return m_revision; }
private:
    void tick() {
        const bool polled = m_session.poll();
        const auto& snapshot = m_session.snapshot();
        const auto access = m_session.access();
        const std::string signature = std::to_string(int(snapshot.state)) + "\n" +
            std::to_string(m_session.generation()) + "\n" + snapshot.identity.subject + "\n" +
            snapshot.identity.name + "\n" + snapshot.message + "\n" + (access ? access->access_token : "");
        if (!polled && signature == m_signature) return;
        m_signature = signature; ++m_revision;
        std::vector<void*> owners;
        for (const auto& listener : m_listeners) owners.push_back(listener.first);
        for (void* owner : owners) {
            const auto found = m_listeners.find(owner);
            if (found != m_listeners.end()) { const auto callback = found->second; if (callback) callback(); }
        }
    }
    HonorAccount::Session m_session;
    wxTimer m_timer;
    std::map<void*, std::function<void()>> m_listeners;
    std::string m_signature;
    std::uint64_t m_revision {0};
    bool m_started {false};
};
}
