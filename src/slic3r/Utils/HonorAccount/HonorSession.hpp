#pragma once
#include "HonorClient.hpp"
#include <mutex>
#include <optional>

namespace Slic3r::HonorAccount {
class Store {
public:
    virtual ~Store() = default;
    virtual std::optional<std::string> load(const std::string& key) = 0;
    virtual bool save(const std::string& key, const std::string& value) = 0;
    virtual bool erase(const std::string& key) = 0;
};
std::unique_ptr<Store> system_store(const std::string& profile);
// Application provisioning only; never stores a user's tokens or identity.
std::unique_ptr<Store> application_configuration_store();

enum class State { SignedOut, Checking, Authorizing, Exchanging, SignedIn, Offline, Failed };
struct Snapshot {
    State state {State::SignedOut};
    Identity identity;
    std::string message;
    bool persistent {false};
    bool has_identity() const { return !identity.subject.empty(); }
    bool busy() const { return state==State::Checking||state==State::Authorizing||state==State::Exchanging; }
};

// Main-thread state and storage. Detached workers own only an isolated Task;
// cancellation/config changes/destruction cannot resurrect a previous account.
class Session {
public:
    Session(std::unique_ptr<Store>, Transport transport = https_request,
            std::unique_ptr<Store> application_configuration = nullptr);
    ~Session();
    const Config& config() const { return m_config; }
    const Snapshot& snapshot() const { return m_snapshot; }
    bool configure(const Config&);
    void restore();
    std::string login();
    bool callback(const std::string&);
    void cancel();
    void fail_authorization(const std::string& safe_message);
    bool logout(bool clear_browser = false);
    // A durable request survives interrupted logout/reset. No authorization
    // page may reuse the browser profile until the host completes this reset.
    bool browser_reset_pending() const;
    bool complete_browser_reset();
    // Called by UI timer; returns true only when observable state changed.
    bool poll();
private:
    struct Task {
        Cancellation cancel {std::make_shared<std::atomic_bool>(false)};
        std::mutex mutex;
        bool done {false};
        std::optional<Credentials> credentials;
        Failure failure {Failure::Temporary};
        std::string message;
    };
    void start(std::function<Credentials(Cancellation)>);
    void cancel_task();
    void share_configuration();
    std::unique_ptr<Store> m_store;
    std::unique_ptr<Store> m_application_configuration;
    Transport m_transport;
    std::shared_ptr<Client::KeyCache> m_keys {std::make_shared<Client::KeyCache>()};
    Config m_config;
    Snapshot m_snapshot;
    std::optional<Credentials> m_credentials;
    std::shared_ptr<Task> m_task;
    std::string m_state, m_nonce;
    std::int64_t m_retry_at {0};
    bool m_retry_enabled {false};
};
} // namespace Slic3r::HonorAccount
