#pragma once

#include "HonorClient.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <variant>
#include <thread>
#include <vector>

namespace Slic3r::HonorAccount {

class Store {
public:
    virtual ~Store() = default;
    virtual std::optional<std::string> load(const std::string& key) = 0;
    virtual bool save(const std::string& key, const std::string& value) = 0;
    virtual bool erase(const std::string& key) = 0;
};

std::unique_ptr<Store> system_store(const std::string& profile);
// Public platform endpoint provisioning only; never stores a user's session.
std::unique_ptr<Store> application_configuration_store();

enum class State {
    SignedOut,
    Checking,
    Preparing,
    Authorizing,
    Exchanging,
    SignedIn,
    Offline,
    Failed
};

struct Snapshot {
    State state {State::SignedOut};
    Identity identity;
    std::string message;
    bool persistent {false};

    bool has_identity() const { return !identity.subject.empty(); }
    bool busy() const
    {
        return state == State::Checking || state == State::Preparing ||
               state == State::Authorizing || state == State::Exchanging;
    }
};

// Native request lease; never serialized to a WebView or persisted by consumers.
struct SessionAccess {
    std::string api_url, access_token, subject;
    std::uint64_t generation {0};
};

// Main-thread state and storage. Managed workers own only an isolated Task;
// cancellation, logout, configuration changes, and destruction cannot publish
// a result into a newer session generation.
class Session {
public:
    using Executor = std::function<bool(std::function<void()>)>;
    Session(std::unique_ptr<Store>, Transport transport = https_request,
            std::unique_ptr<Store> application_configuration = nullptr, Executor executor = {});
    ~Session();

    const Config& config() const { return m_config; }
    const Snapshot& snapshot() const { return m_snapshot; }
    std::uint64_t generation() const noexcept { return m_generation; }
    std::optional<SessionAccess> access() const;
    bool refresh_access(std::uint64_t expected_generation);

    bool configure(const Config&);
    void restore();
    bool login();
    std::optional<std::string> take_authorization_url();
    bool callback(const std::string&, std::optional<std::uint64_t> expected_generation = std::nullopt);
    void cancel();
    void fail_authorization(const std::string& safe_message);
    bool logout(bool clear_browser = false);

    bool browser_reset_pending() const;
    bool complete_browser_reset();
    // Called by the UI timer; returns true only when observable state changed.
    bool poll();
    void stop_accepting();

private:
    enum class TaskKind { Authorize, Exchange, Validate, Refresh };
    using TaskResult = std::variant<Authorization, PlatformSession>;

    struct Task {
        TaskKind kind;
        std::uint64_t generation {0};
        Cancellation cancel {std::make_shared<std::atomic_bool>(false)};
        std::mutex mutex;
        bool done {false};
        std::optional<TaskResult> result;
        Failure failure {Failure::Temporary};
        std::string message;
    };

    struct RevokeTask {
        std::uint64_t generation {0};
        Cancellation cancel {std::make_shared<std::atomic_bool>(false)};
        std::mutex mutex;
        bool done {false};
        bool success {false};
    };

    void start(TaskKind, std::function<TaskResult(Cancellation)>);
    bool start_refresh();
    void start_revoke(const std::string& refresh_token);
    void cancel_task();
    void cancel_revoke();
    bool execute(std::function<void()>);
    bool save_session(const PlatformSession&);
    bool erase_session();
    bool migrate_legacy(Store&, const std::string&, Config&, bool profile_store);
    void share_configuration();
    std::string session_key() const;
    static const char* browser_reset_key();

    std::unique_ptr<Store> m_store;
    std::unique_ptr<Store> m_application_configuration;
    Transport m_transport;
    Executor m_executor;
    struct OwnedWork { std::thread thread; std::shared_ptr<std::atomic_bool> done; };
    std::vector<OwnedWork> m_workers;
    bool m_stopping {false};
    Config m_config;
    Snapshot m_snapshot;
    std::optional<PlatformSession> m_session;
    std::shared_ptr<Task> m_task;
    std::shared_ptr<RevokeTask> m_revoke;
    std::optional<std::string> m_authorization_url;
    std::string m_verifier;
    std::int64_t m_authorization_expires_at {0};
    std::int64_t m_retry_at {0};
    bool m_retry_enabled {false};
    bool m_remote_revoke_pending {false};
    bool m_browser_reset_unconfirmed {false};
    std::uint64_t m_generation {0};
};

} // namespace Slic3r::HonorAccount
