#pragma once
#include "AccountAvatar.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/HonorAccount/HonorSession.hpp"
#include <wx/timer.h>
#include <wx/weakref.h>
#include <map>
#include <vector>
#include "../Presentation/UiRuntime.hpp"

namespace Slic3r::GUI {
// Application lifetime, independent of which account/library page is visible.
class PlatformAccountService final : public wxEvtHandler {
public:
    struct Avatar {
        wxImage image;
        std::string data;
    };

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
            m_timer.Stop(); m_avatar_scope.cancel(); m_listeners.clear(); m_session.stop_accepting();
        });
    }
    ~PlatformAccountService() override {
        Presentation::background_tasks().remove_shutdown_hook(this); m_timer.Stop();
    }
    HonorAccount::Session& session() { return m_session; }
    std::optional<HonorAccount::Services> services() const { return m_session.services(); }
    const Avatar& avatar() const { return m_avatar; }
    std::uint64_t avatar_revision() const { return m_avatar_revision; }
    bool logout(bool clear_browser) {
        ++m_logout_revision;
        const bool changed = m_session.logout(clear_browser);
        tick();
        return changed;
    }
    void start() {
        if (m_started) return;
        m_started = true; m_session.restore(); tick(); m_timer.Start(100);
    }
    void subscribe(void* owner, std::function<void()> changed) { m_listeners[owner] = std::move(changed); }
    void unsubscribe(void* owner) { m_listeners.erase(owner); }
    std::uint64_t revision() const { return m_revision; }
    std::uint64_t logout_revision() const { return m_logout_revision; }
private:
    static Avatar placeholder(const char* filename) {
        auto image = AccountAvatar::read(wxString::FromUTF8(Slic3r::resources_dir() + "/images/" + filename));
        if (image.IsOk()) image.Rescale(AccountAvatar::stored_edge, AccountAvatar::stored_edge, wxIMAGE_QUALITY_HIGH);
        auto data = AccountAvatar::png_data(image);
        return {std::move(image), std::move(data)};
    }
    bool set_avatar(Avatar value) {
        if (m_avatar.data == value.data) return false;
        m_avatar = std::move(value);
        ++m_avatar_revision;
        return true;
    }
    void refresh_avatar() {
        const auto& snapshot = m_session.snapshot();
        const auto epoch = m_session.generation();
        const auto owner = snapshot.has_identity() ? m_session.config().api_url + '\n' + snapshot.identity.subject : "";
        const auto url = owner.empty() ? "" : snapshot.avatar_url.value_or("");
        const bool owner_changed = owner != m_avatar_owner;
        const bool url_changed = url != m_avatar_url;
        if (!owner_changed && !url_changed && epoch == m_avatar_epoch) return;

        m_avatar_scope.cancel();
        m_avatar_owner = owner;
        m_avatar_url = url;
        m_avatar_epoch = epoch;
        if (owner_changed || url.empty()) set_avatar(owner.empty() ? m_guest_avatar : m_default_avatar);
        if (owner_changed || url_changed) m_avatar_loaded = false;
        if (url.empty() || m_avatar_loaded) return;

        const auto task = m_avatar_scope.begin(owner, Presentation::TaskLifetime::Session, epoch);
        wxWeakRef<PlatformAccountService> weak(this);
        Presentation::background_tasks().submit(task, [url](const Presentation::TaskContext& context) {
            auto image = AccountAvatar::square(AccountAvatar::fetch(url, context.cancellation));
            auto data = AccountAvatar::png_data(image);
            return Avatar{std::move(image), std::move(data)};
        }, [weak, task](Presentation::TaskResult<Avatar> result) {
            if (!weak || !weak->m_avatar_scope.matches(task) || task.account_epoch != weak->m_session.generation() ||
                !result.value || result.value->data.empty()) return;
            weak->m_avatar_loaded = true;
            if (weak->set_avatar(std::move(*result.value))) weak->notify();
        });
    }
    void tick() {
        const bool polled = m_session.poll();
        refresh_avatar();
        const auto& snapshot = m_session.snapshot();
        const auto access = m_session.access();
        const std::string signature = std::to_string(int(snapshot.state)) + "\n" +
            std::to_string(m_session.generation()) + "\n" + snapshot.identity.subject + "\n" +
            snapshot.identity.name + "\n" + snapshot.message + "\n" +
            std::to_string(int(snapshot.avatar_update)) + "\n" + (access ? access->access_token : "");
        if (!polled && signature == m_signature) return;
        m_signature = signature;
        notify();
    }
    void notify() {
        ++m_revision;
        std::vector<void*> owners;
        for (const auto& listener : m_listeners) owners.push_back(listener.first);
        for (void* owner : owners) {
            const auto found = m_listeners.find(owner);
            if (found != m_listeners.end()) { const auto callback = found->second; if (callback) callback(); }
        }
    }
    HonorAccount::Session m_session;
    wxTimer m_timer;
    Avatar m_guest_avatar = placeholder("account_guest_avatar.png");
    Avatar m_default_avatar = placeholder("honor_account_avatar.png");
    Avatar m_avatar = m_guest_avatar;
    Presentation::TaskScope m_avatar_scope;
    std::string m_avatar_owner, m_avatar_url;
    std::uint64_t m_avatar_epoch {0};
    std::uint64_t m_avatar_revision {0};
    bool m_avatar_loaded {false};
    std::map<void*, std::function<void()>> m_listeners;
    std::string m_signature;
    std::uint64_t m_revision {0};
    std::uint64_t m_logout_revision {0};
    bool m_started {false};
};
}
