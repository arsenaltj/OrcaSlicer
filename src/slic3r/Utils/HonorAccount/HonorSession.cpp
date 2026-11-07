#include "HonorSession.hpp"

#include <algorithm>
#include <boost/log/trivial.hpp>
#include <cstdlib>
#include <regex>
#include <thread>

namespace Slic3r::HonorAccount {
namespace {

bool local_endpoint(const std::string& value)
{
    static const std::regex pattern(
        R"(^https://(?:localhost|127\.0\.0\.1|\[::1\])(?::[0-9]{1,5})?$)",
        std::regex::icase);
    return std::regex_match(value, pattern);
}

} // namespace

const char* Session::browser_reset_key()
{
    return "browser-reset-platform-v2";
}

Session::Session(std::unique_ptr<Store> store, Transport transport,
                 std::unique_ptr<Store> application_configuration)
    : m_store(std::move(store)),
      m_application_configuration(std::move(application_configuration)),
      m_transport(std::move(transport))
{
    try {
        const auto profile_raw = m_store->load("configuration");
        const auto application_raw =
            m_application_configuration
                ? m_application_configuration->load("configuration")
                : std::optional<std::string>{};

        std::optional<Config> profile_config;
        std::optional<Config> application_config;
        const auto profile_legacy =
            profile_raw ? legacy_profile_key(*profile_raw) : std::optional<std::string>{};
        const auto application_legacy =
            application_raw ? legacy_profile_key(*application_raw) : std::optional<std::string>{};

        if (profile_raw && !profile_legacy) {
            try {
                profile_config = parse_config(*profile_raw);
            } catch (...) {
                m_snapshot.message = "本机平台账号配置无法读取，请联系软件提供方";
                BOOST_LOG_TRIVIAL(warning)
                    << "[HonorAccount] stage=config_read result=failed source=profile";
                return;
            }
        }
        if (!profile_raw && application_raw && !application_legacy) {
            try {
                application_config = parse_config(*application_raw);
            } catch (...) {
                m_snapshot.message = "本机平台账号配置无法读取，请联系软件提供方";
                BOOST_LOG_TRIVIAL(warning)
                    << "[HonorAccount] stage=config_read result=failed source=application";
                return;
            }
        } else if (application_raw && !application_legacy) {
            try {
                application_config = parse_config(*application_raw);
            } catch (...) {
                // A valid profile has precedence. A corrupt shared fallback must
                // not replace it, but it also must not be copied anywhere.
                application_config.reset();
            }
        }

        const char* provisioned = std::getenv("ORCA_PLATFORM_API_URL");
        if (provisioned && *provisioned) {
            m_config.api_url = provisioned;
            if (profile_config) {
                const bool same_service =
                    profile_config->api_url == m_config.api_url &&
                    profile_config->callback_uri == m_config.callback_uri;
                m_config.browser_profile_key =
                    same_service ? profile_config->browser_profile_key
                                 : profile_config->browser_key();
            }
            else if (profile_legacy) m_config.browser_profile_key = *profile_legacy;
            else if (application_config) m_config.browser_profile_key = application_config->browser_key();
            else if (application_legacy) m_config.browser_profile_key = *application_legacy;
            if (const auto error = m_config.validate(); !error.empty()) {
                m_snapshot.message = error;
                BOOST_LOG_TRIVIAL(warning)
                    << "[HonorAccount] stage=config_read result=invalid source=environment";
                return;
            }
        } else if (profile_config && !profile_config->api_url.empty()) {
            m_config = *profile_config;
        } else if (application_config && !application_config->api_url.empty()) {
            m_config = *application_config;
            m_config.browser_profile_key.clear();
            if (profile_config) m_config.browser_profile_key = profile_config->browser_key();
            else if (profile_legacy) m_config.browser_profile_key = *profile_legacy;
            else if (application_legacy) m_config.browser_profile_key = *application_legacy;
        } else if (profile_config) {
            m_config = *profile_config;
        } else if (application_config) {
            m_config = *application_config;
            m_config.browser_profile_key.clear();
        } else if (profile_legacy || application_legacy) {
            // Persist a secret-free migration marker even before the public API
            // endpoint is provisioned. The old browser profile can still be reused.
            m_config.browser_profile_key =
                profile_legacy ? *profile_legacy : *application_legacy;
        }

        if (profile_raw && profile_legacy) {
            if (!migrate_legacy(*m_store, *profile_raw, m_config, true)) {
                m_snapshot.message =
                    "旧版荣耀凭据未能安全清除，请重新打开软件后重试";
                BOOST_LOG_TRIVIAL(warning)
                    << "[HonorAccount] stage=legacy_cleanup result=failed source=profile";
                return;
            }
            BOOST_LOG_TRIVIAL(info)
                << "[HonorAccount] stage=legacy_cleanup result=completed source=profile";
        } else if (profile_config && serialize_config(*profile_config) != serialize_config(m_config)) {
            const auto old_session = "platform-session-" + profile_config->key();
            if (!m_store->erase(old_session) ||
                !m_store->save("configuration", serialize_config(m_config))) {
                m_snapshot.message = "平台账号环境切换未完成，请重新打开软件后重试";
                return;
            }
        } else if ((!profile_raw && !m_config.api_url.empty()) ||
                   (profile_raw && profile_config &&
                    serialize_config(*profile_config) != serialize_config(m_config))) {
            if (!m_store->save("configuration", serialize_config(m_config))) {
                m_snapshot.message = "平台账号配置无法安全保存";
                return;
            }
        }

        if (application_raw && application_legacy) {
            if (!migrate_legacy(*m_application_configuration, *application_raw,
                                m_config, false)) {
                m_snapshot.message = "旧版荣耀应用配置未能安全清除";
                return;
            }
            BOOST_LOG_TRIVIAL(info)
                << "[HonorAccount] stage=legacy_cleanup result=completed source=application";
        }
        share_configuration();

        BOOST_LOG_TRIVIAL(info)
            << "[HonorAccount] stage=config_read result="
            << (m_config.validate().empty() ? "ready" : "missing")
            << " source="
            << (provisioned && *provisioned ? "environment"
                                            : profile_config ? "profile"
                                                             : application_config ? "application"
                                                                                  : "none");
        if (const auto error = m_config.validate(); !error.empty())
            m_snapshot.message = error;
    } catch (...) {
        m_snapshot.message = "本机账号服务无法读取，请联系软件提供方";
        BOOST_LOG_TRIVIAL(warning)
            << "[HonorAccount] stage=config_read result=failed";
    }
}

bool Session::migrate_legacy(Store& store, const std::string& raw, Config& config,
                             bool profile_store)
{
    const auto legacy = legacy_profile_key(raw);
    if (!legacy) return false;
    if (config.browser_profile_key.empty()) config.browser_profile_key = *legacy;
    try {
        if (profile_store) {
            const auto old_reset = "browser-reset-" + *legacy;
            const bool reset_pending = store.load(old_reset).has_value();
            const auto old_session = "session-" + *legacy;
            if (!store.erase(old_session) || store.load(old_session).has_value()) return false;
            if (reset_pending &&
                (!store.save(browser_reset_key(), "pending") ||
                 store.load(browser_reset_key()) !=
                     std::optional<std::string>("pending")))
                return false;
            if (reset_pending && !store.erase(old_reset)) return false;
        }
        const auto serialized = serialize_config(config);
        return store.save("configuration", serialized) &&
               store.load("configuration") == std::optional<std::string>(serialized);
    } catch (...) {
        return false;
    }
}

void Session::share_configuration()
{
    if (!m_application_configuration || !m_config.validate().empty() ||
        local_endpoint(m_config.api_url))
        return;
    try {
        auto shared = m_config;
        shared.browser_profile_key.clear();
        const auto value = serialize_config(shared);
        const bool ok = m_application_configuration->save("configuration", value) &&
                        m_application_configuration->load("configuration") ==
                            std::optional<std::string>(value);
        BOOST_LOG_TRIVIAL(info)
            << "[HonorAccount] stage=config_share result=" << (ok ? "ready" : "failed");
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning)
            << "[HonorAccount] stage=config_share result=failed";
    }
}

Session::~Session()
{
    cancel_task();
    cancel_revoke();
}

std::string Session::session_key() const
{
    return "platform-session-" + m_config.key();
}

void Session::cancel_task()
{
    ++m_generation;
    if (m_task) m_task->cancel->store(true);
    m_task.reset();
    m_authorization_url.reset();
    m_authorization_expires_at = 0;
    m_verifier.clear();
}

void Session::cancel_revoke()
{
    if (m_revoke) m_revoke->cancel->store(true);
    m_revoke.reset();
}

bool Session::configure(const Config& config)
{
    if (!config.validate().empty()) return false;
    if (serialize_config(config) == serialize_config(m_config)) return true;
    try {
        if (!m_config.api_url.empty() && !m_store->erase(session_key())) return false;
        const auto value = serialize_config(config);
        if (!m_store->save("configuration", value) ||
            m_store->load("configuration") != std::optional<std::string>(value))
            return false;
    } catch (...) {
        return false;
    }
    cancel_task();
    cancel_revoke();
    m_retry_enabled = false;
    m_session.reset();
    m_config = config;
    m_snapshot = {};
    share_configuration();
    return true;
}

void Session::start(TaskKind kind,
                    std::function<TaskResult(Cancellation)> operation)
{
    auto task = m_task = std::make_shared<Task>();
    task->kind = kind;
    task->generation = m_generation;
    std::thread([task, operation = std::move(operation)] {
        std::optional<TaskResult> result;
        Failure failure = Failure::Temporary;
        std::string message;
        try {
            result = operation(task->cancel);
        } catch (const Error& error) {
            failure = error.kind;
            message = error.what();
        } catch (...) {
            failure = Failure::Protocol;
            message = "平台账号请求未完成，请重试";
        }
        std::lock_guard<std::mutex> lock(task->mutex);
        if (!task->cancel->load()) {
            task->result = std::move(result);
            task->failure = failure;
            task->message = std::move(message);
            task->done = true;
        }
    }).detach();
}

bool Session::save_session(const PlatformSession& session)
{
    try {
        const auto value = serialize_session(session);
        return m_store->save(session_key(), value) &&
               m_store->load(session_key()) == std::optional<std::string>(value);
    } catch (...) {
        return false;
    }
}

bool Session::erase_session()
{
    try {
        return m_store->erase(session_key()) &&
               !m_store->load(session_key()).has_value();
    } catch (...) {
        return false;
    }
}

bool Session::start_refresh()
{
    if (!m_session) return false;
    if (m_session->refresh_expires_at <= now_seconds()) {
        const bool erased = erase_session();
        m_session.reset();
        m_snapshot = {};
        m_snapshot.state = erased ? State::SignedOut : State::Failed;
        m_snapshot.message = erased ? "平台登录已过期，请重新登录"
                                    : "平台登录已过期但无法安全清除，请重新打开软件后重试";
        return false;
    }
    try {
        const auto tombstone = serialize_refresh_tombstone();
        if (!m_store->save(session_key(), tombstone) ||
            m_store->load(session_key()) != std::optional<std::string>(tombstone)) {
            m_snapshot.state = State::Offline;
            m_snapshot.identity = m_session->identity;
            m_snapshot.message = "无法安全轮换平台会话，请稍后重试";
            m_retry_enabled = true;
            m_retry_at = now_seconds() + 60;
            return false;
        }
    } catch (...) {
        m_snapshot.state = State::Offline;
        m_snapshot.identity = m_session->identity;
        m_snapshot.message = "无法安全轮换平台会话，请稍后重试";
        m_retry_enabled = true;
        m_retry_at = now_seconds() + 60;
        return false;
    }

    const auto config = m_config;
    const auto previous = *m_session;
    const auto transport = m_transport;
    m_snapshot.state = State::Checking;
    m_snapshot.message = "正在刷新平台登录状态…";
    start(TaskKind::Refresh,
          [config, previous, transport](Cancellation cancel) -> TaskResult {
              return Client(transport).refresh(config, previous, cancel);
          });
    return true;
}

void Session::restore()
{
    if (m_snapshot.busy()) return;
    BOOST_LOG_TRIVIAL(info) << "[HonorAccount] stage=restore result=start";
    cancel_task();
    m_retry_enabled = false;
    if (const auto error = m_config.validate(); !error.empty()) {
        m_snapshot.state = State::SignedOut;
        m_snapshot.message = error;
        BOOST_LOG_TRIVIAL(warning)
            << "[HonorAccount] stage=restore result=config_invalid";
        return;
    }
    if (browser_reset_pending()) {
        if (!erase_session()) {
            m_snapshot = {};
            m_snapshot.state = State::Failed;
            m_snapshot.message = "本机登录清理状态无法读取，请重新打开软件后重试";
            return;
        }
        m_session.reset();
        m_snapshot = {};
        m_snapshot.message = "登录记录待清理，下次登录前将先完成清理";
        return;
    }

    if (!m_session) {
        try {
            if (const auto saved = m_store->load(session_key())) {
                if (is_refresh_tombstone(*saved)) {
                    m_snapshot = {};
                    if (erase_session()) {
                        m_snapshot.message = "上次会话刷新未完成，请重新登录";
                    } else {
                        m_snapshot.state = State::Failed;
                        m_snapshot.message = "上次会话刷新未完成且无法安全清除，请重新打开软件后重试";
                    }
                    return;
                }
                m_session = parse_session(*saved);
                m_snapshot.persistent = true;
            }
        } catch (...) {
            m_snapshot = {};
            m_snapshot.state = State::Failed;
            m_snapshot.message = erase_session()
                                     ? "本机平台会话无法读取，已安全清除，请重新登录"
                                     : "本机平台会话损坏且无法清除，请重新打开软件后重试";
            BOOST_LOG_TRIVIAL(warning)
                << "[HonorAccount] stage=restore result=storage_failed";
            return;
        }
    }
    if (!m_session) {
        m_snapshot = {};
        BOOST_LOG_TRIVIAL(info)
            << "[HonorAccount] stage=restore result=no_session";
        return;
    }
    if (m_session->refresh_expires_at <= now_seconds()) {
        const bool erased = erase_session();
        m_session.reset();
        m_snapshot = {};
        m_snapshot.state = erased ? State::SignedOut : State::Failed;
        m_snapshot.message = erased ? "平台登录已过期，请重新登录"
                                    : "平台登录已过期但无法安全清除，请重新打开软件后重试";
        return;
    }

    m_snapshot.state = State::Checking;
    m_snapshot.identity = m_session->identity;
    m_snapshot.message = "正在验证平台登录状态…";
    if (m_session->access_expires_at <= now_seconds() + 60) {
        start_refresh();
        return;
    }
    const auto config = m_config;
    const auto previous = *m_session;
    const auto transport = m_transport;
    start(TaskKind::Validate,
          [config, previous, transport](Cancellation cancel) -> TaskResult {
              return Client(transport).validate(config, previous, cancel);
          });
}

bool Session::login()
{
    BOOST_LOG_TRIVIAL(info)
        << "[HonorAccount] stage=login result=requested";
    if (m_snapshot.busy()) return false;
    cancel_task();
    m_retry_enabled = false;
    if (const auto error = m_config.validate(); !error.empty()) {
        m_snapshot.state = State::Failed;
        m_snapshot.message = error;
        return false;
    }
    try {
        m_verifier = pkce_verifier();
        const auto config = m_config;
        const auto verifier = m_verifier;
        const auto transport = m_transport;
        m_snapshot.state = State::Preparing;
        m_snapshot.message = "正在连接平台账号服务…";
        start(TaskKind::Authorize,
              [config, verifier, transport](Cancellation cancel) -> TaskResult {
                  return Client(transport).authorize(config, verifier, cancel);
              });
        return true;
    } catch (const Error& error) {
        m_snapshot.state = State::Failed;
        m_snapshot.message = error.what();
        m_verifier.clear();
        return false;
    }
}

std::optional<std::string> Session::take_authorization_url()
{
    auto result = std::move(m_authorization_url);
    m_authorization_url.reset();
    return result;
}

bool Session::callback(const std::string& url, std::optional<std::uint64_t> expected_generation)
{
    if (!is_callback(url, m_config.callback_uri)) return false;
    if (expected_generation && *expected_generation != m_generation) {
        BOOST_LOG_TRIVIAL(info)
            << "[HonorAccount] stage=callback result=stale generation=" << *expected_generation;
        return true;
    }
    if (m_snapshot.state != State::Authorizing) return true;
    try {
        if (m_authorization_expires_at <= now_seconds())
            throw Error(Failure::InvalidCredentials,
                        "登录请求已过期，请重新登录");
        const auto code = callback_code(url, m_config.callback_uri);
        const auto verifier = m_verifier;
        const auto config = m_config;
        const auto transport = m_transport;
        m_authorization_expires_at = 0;
        m_snapshot.state = State::Exchanging;
        m_snapshot.message = "正在确认平台账号身份…";
        start(TaskKind::Exchange,
              [config, code, verifier, transport](Cancellation cancel) -> TaskResult {
                  return Client(transport).exchange(config, code, verifier, cancel);
              });
        BOOST_LOG_TRIVIAL(info)
            << "[HonorAccount] stage=callback result=validated";
    } catch (const Error& error) {
        cancel_task();
        m_snapshot.state = State::Failed;
        m_snapshot.message = error.what();
        BOOST_LOG_TRIVIAL(warning)
            << "[HonorAccount] stage=callback result=rejected failure="
            << int(error.kind);
    }
    return true;
}

void Session::cancel()
{
    BOOST_LOG_TRIVIAL(info)
        << "[HonorAccount] stage=cancel result=requested";
    cancel_task();
    m_retry_enabled = false;
    m_snapshot.state = m_session ? State::Offline : State::SignedOut;
    m_snapshot.identity = m_session ? m_session->identity : Identity{};
    m_snapshot.message = "登录已取消";
}

void Session::fail_authorization(const std::string& message)
{
    cancel();
    m_snapshot.state = State::Failed;
    m_snapshot.message = message;
}

bool Session::browser_reset_pending() const
{
    if (m_browser_reset_unconfirmed) return true;
    try {
        return m_store->load(browser_reset_key()).has_value();
    } catch (...) {
        return true;
    }
}

bool Session::complete_browser_reset()
{
    try {
        if (!erase_session()) return false;
        if (m_store->load(browser_reset_key()).has_value() &&
            (!m_store->erase(browser_reset_key()) ||
             m_store->load(browser_reset_key()).has_value()))
            return false;
        m_browser_reset_unconfirmed = false;
        return true;
    } catch (...) {
        return false;
    }
}

void Session::start_revoke(const std::string& refresh_token)
{
    cancel_revoke();
    if (refresh_token.empty()) return;
    m_remote_revoke_pending = true;
    auto task = m_revoke = std::make_shared<RevokeTask>();
    task->generation = m_generation;
    const auto config = m_config;
    const auto transport = m_transport;
    std::thread([task, config, transport, refresh_token] {
        bool success = false;
        try {
            Client(transport).revoke(config, refresh_token, task->cancel);
            success = true;
        } catch (...) {
        }
        std::lock_guard<std::mutex> lock(task->mutex);
        if (!task->cancel->load()) {
            task->success = success;
            task->done = true;
        }
    }).detach();
}

bool Session::logout(bool clear_browser)
{
    bool browser_reset_requested = !clear_browser;
    if (clear_browser) {
        try {
            browser_reset_requested =
                m_store->save(browser_reset_key(), "pending") &&
                m_store->load(browser_reset_key()) ==
                    std::optional<std::string>("pending");
        } catch (...) {
            browser_reset_requested = false;
        }
        if (!browser_reset_requested)
            BOOST_LOG_TRIVIAL(warning)
                << "[HonorAccount] stage=browser_reset result=request_failed";
    }

    const auto refresh_token = m_session ? m_session->refresh_token : std::string();
    cancel_task();
    m_retry_enabled = false;
    if (!erase_session()) {
        m_snapshot.state = State::Failed;
        if (m_session) m_snapshot.identity = m_session->identity;
        m_snapshot.message = "系统未能清除已保存的平台会话，请重试退出";
        BOOST_LOG_TRIVIAL(warning)
            << "[HonorAccount] stage=logout result=storage_failed";
        return false;
    }

    m_session.reset();
    m_snapshot = {};
    m_browser_reset_unconfirmed = clear_browser && !browser_reset_requested;
    if (m_browser_reset_unconfirmed) {
        m_snapshot.message = "已退出；本机荣耀网页登录记录清理未确认";
    } else {
        m_snapshot.message = refresh_token.empty() ? "已退出"
                                                    : "已退出，正在撤销远端会话…";
    }
    start_revoke(refresh_token);
    BOOST_LOG_TRIVIAL(info)
        << "[HonorAccount] stage=logout result=local_completed";
    return true;
}

bool Session::poll()
{
    bool changed = false;
    if (m_revoke) {
        auto task = m_revoke;
        bool done = false;
        bool success = false;
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            done = task->done;
            success = task->success;
        }
        if (done) {
            m_revoke.reset();
            if (task->generation == m_generation &&
                m_snapshot.state == State::SignedOut) {
                m_remote_revoke_pending = !success;
                if (m_browser_reset_unconfirmed) {
                    m_snapshot.message = success
                                             ? "已退出；本机荣耀网页登录记录清理未确认"
                                             : "已退出；本机网页登录记录清理及远端撤销均未确认";
                } else {
                    m_snapshot.message =
                        success ? "已退出" : "已退出；远端撤销未确认";
                }
                changed = true;
            }
        }
    }

    if (m_task) {
        auto task = m_task;
        std::optional<TaskResult> result;
        Failure failure = Failure::Temporary;
        std::string message;
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            if (!task->done) return changed;
            result = task->result;
            failure = task->failure;
            message = task->message;
        }
        m_task.reset();
        if (task->generation != m_generation) return changed;

        if (result) {
            if (task->kind == TaskKind::Authorize) {
                const auto authorization = std::get<Authorization>(*result);
                m_authorization_url = authorization.url;
                m_authorization_expires_at = authorization.expires_at;
                m_snapshot.state = State::Authorizing;
                m_snapshot.message = "请在下方荣耀页面完成登录或注册";
                BOOST_LOG_TRIVIAL(info)
                    << "[HonorAccount] stage=authorize result=ready";
            } else {
                auto session = std::get<PlatformSession>(*result);
                const bool persistent = save_session(session);
                m_session = std::move(session);
                m_snapshot.identity = m_session->identity;
                m_snapshot.state = State::SignedIn;
                m_snapshot.persistent = persistent;
                m_snapshot.message =
                    persistent
                        ? "已登录，下次打开软件将自动恢复"
                        : "已登录，但系统安全存储不可用，关闭后需重新登录";
                m_retry_at =
                    std::max(now_seconds(), m_session->access_expires_at - 60);
                m_retry_enabled = true;
                m_verifier.clear();
                m_remote_revoke_pending = false;
                m_browser_reset_unconfirmed = false;
                BOOST_LOG_TRIVIAL(info)
                    << "[HonorAccount] stage="
                    << (task->kind == TaskKind::Exchange
                            ? "exchange"
                            : task->kind == TaskKind::Refresh ? "refresh"
                                                              : "restore")
                    << " result=verified persistent=" << persistent;
            }
        } else if (task->kind == TaskKind::Refresh) {
            erase_session();
            m_session.reset();
            m_snapshot = {};
            m_snapshot.state = State::Failed;
            m_snapshot.message =
                failure == Failure::InvalidCredentials
                    ? "平台登录已失效，请重新登录"
                    : "平台会话刷新结果未知，为保护账号请重新登录";
            m_remote_revoke_pending = true;
            m_retry_enabled = false;
            BOOST_LOG_TRIVIAL(warning)
                << "[HonorAccount] stage=refresh result=discarded failure="
                << int(failure);
        } else if (task->kind == TaskKind::Validate &&
                   failure == Failure::Temporary && m_session) {
            m_snapshot.state = State::Offline;
            m_snapshot.identity = m_session->identity;
            m_snapshot.message = "离线，待验证。" + message;
            m_retry_enabled = true;
            m_retry_at = std::min(m_session->access_expires_at - 60,
                                  now_seconds() + 60);
        } else {
            bool erased = true;
            if (task->kind == TaskKind::Validate) {
                erased = erase_session();
                m_session.reset();
            }
            m_snapshot.state = State::Failed;
            m_snapshot.identity = m_session ? m_session->identity : Identity{};
            m_snapshot.message = !erased
                                     ? "平台会话已失效但无法安全清除，请重新打开软件后重试"
                                     : message.empty()
                                           ? "平台账号请求未完成，请重试"
                                           : message;
            m_retry_enabled = false;
            BOOST_LOG_TRIVIAL(warning)
                << "[HonorAccount] stage=request result=failed failure="
                << int(failure);
        }
        changed = true;
    }

    if (m_snapshot.state == State::Authorizing &&
        m_authorization_expires_at > 0 &&
        now_seconds() >= m_authorization_expires_at) {
        cancel_task();
        m_snapshot.state = State::Failed;
        m_snapshot.message = "登录请求已过期，请重新登录";
        return true;
    }
    if (!m_task && m_retry_enabled && now_seconds() >= m_retry_at) {
        restore();
        return true;
    }
    return changed;
}

} // namespace Slic3r::HonorAccount
