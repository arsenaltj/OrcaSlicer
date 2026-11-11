#include "slic3r/Utils/HonorAccount/HonorSession.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r::HonorAccount;

namespace {

using Json = nlohmann::json;

constexpr const char* account_id = "22222222-2222-4222-8222-222222222222";
constexpr const char* other_account_id = "33333333-3333-4333-8333-333333333333";
constexpr const char* session_id = "11111111-1111-4111-8111-111111111111";
constexpr const char* rotated_session_id = "44444444-4444-4444-8444-444444444444";
constexpr const char* rfc_verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
constexpr const char* rfc_challenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";

std::string fixture_token(char value)
{
    return std::string(43, value);
}

Config configuration()
{
    Config result;
    result.api_url = "https://platform.example.test";
    return result;
}

Identity identity(const std::string& subject = account_id)
{
    Identity result;
    result.subject = subject;
    result.email = "maker@example.test";
    result.name = "Local Maker";
    result.picture = "https://images.example.test/avatar.png";
    result.display_name_customized = true;
    result.avatar_customized = false;
    result.status = "active";
    result.roles = {"user"};
    return result;
}

PlatformSession platform_session(std::int64_t access_expires_at = now_seconds() + 3600,
                                 std::int64_t refresh_expires_at = now_seconds() + 86400,
                                 const std::string& subject = account_id)
{
    PlatformSession result;
    result.session_id = session_id;
    result.access_token = fixture_token('A');
    result.refresh_token = fixture_token('R');
    result.access_expires_at = access_expires_at;
    result.refresh_expires_at = refresh_expires_at;
    result.identity = identity(subject);
    return result;
}

std::string iso_time(std::int64_t value)
{
    const std::time_t time = static_cast<std::time_t>(value);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &time) != 0) throw std::runtime_error("fixture timestamp");
#else
    if (!gmtime_r(&time, &utc)) throw std::runtime_error("fixture timestamp");
#endif
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
        throw std::runtime_error("fixture timestamp");
    return buffer;
}

Json account_json(const Identity& value)
{
    return Json{{"id", value.subject},
                {"email", value.email ? Json(*value.email) : Json(nullptr)},
                {"displayName", value.name},
                {"avatarUrl", value.picture ? Json(*value.picture) : Json(nullptr)},
                {"displayNameCustomized", value.display_name_customized},
                {"avatarCustomized", value.avatar_customized},
                {"status", value.status},
                {"roles", value.roles}};
}

std::string account_response(const Identity& value = identity())
{
    return account_json(value).dump();
}

std::string session_response(const PlatformSession& value)
{
    return Json{{"account", account_json(value.identity)},
                {"sessionId", value.session_id},
                {"kind", "mobile"},
                {"accessToken", value.access_token},
                {"refreshToken", value.refresh_token},
                {"accessExpiresAt", iso_time(value.access_expires_at)},
                {"refreshExpiresAt", iso_time(value.refresh_expires_at)}}
        .dump();
}

Response attempt_response(std::int64_t expires_at = now_seconds() + 600)
{
    return {201,
            Json{{"attemptId", fixture_token('I')},
                 {"expiresAt", iso_time(expires_at)},
                 {"sdkAllowed", false}}
                .dump()};
}

Response authorization_response()
{
    return {200,
            Json{{"authorizationUrl",
                  "https://id.example.test/authorize?state=fixture-state"}}
                .dump()};
}

std::string session_key(const Config& config)
{
    return "platform-session-" + config.key();
}

struct Memory {
    mutable std::mutex mutex;
    std::map<std::string, std::string> values;
    std::set<std::string> failed_saves;
    std::set<std::string> failed_erases;
    bool writable {true};
    bool readable {true};
    bool erasable {true};

    void put(const std::string& key, const std::string& value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        values[key] = value;
    }

    std::optional<std::string> get(const std::string& key) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = values.find(key);
        return found == values.end() ? std::nullopt
                                     : std::optional<std::string>(found->second);
    }

    bool has(const std::string& key) const { return get(key).has_value(); }
};

class MemoryStore final : public Store {
public:
    explicit MemoryStore(std::shared_ptr<Memory> memory) : m_memory(std::move(memory)) {}

    std::optional<std::string> load(const std::string& key) override
    {
        std::lock_guard<std::mutex> lock(m_memory->mutex);
        if (!m_memory->readable) throw std::runtime_error("fixture read failure");
        const auto found = m_memory->values.find(key);
        return found == m_memory->values.end() ? std::nullopt
                                               : std::optional<std::string>(found->second);
    }

    bool save(const std::string& key, const std::string& value) override
    {
        std::lock_guard<std::mutex> lock(m_memory->mutex);
        if (!m_memory->writable || m_memory->failed_saves.count(key)) return false;
        m_memory->values[key] = value;
        return true;
    }

    bool erase(const std::string& key) override
    {
        std::lock_guard<std::mutex> lock(m_memory->mutex);
        if (!m_memory->writable || !m_memory->erasable ||
            m_memory->failed_erases.count(key))
            return false;
        m_memory->values.erase(key);
        return true;
    }

private:
    std::shared_ptr<Memory> m_memory;
};

std::shared_ptr<Memory> configured_memory(
    const std::optional<PlatformSession>& saved_session = std::nullopt)
{
    auto memory = std::make_shared<Memory>();
    const auto config = configuration();
    memory->put("configuration", serialize_config(config));
    if (saved_session) memory->put(session_key(config), serialize_session(*saved_session));
    return memory;
}

template<class Predicate>
bool wait_for_session(Session& session, Predicate predicate,
                      std::chrono::milliseconds timeout = std::chrono::seconds(3))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        session.poll();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    session.poll();
    return predicate();
}

struct Gate {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered {false};
    bool released {false};
    bool returned {false};

    void block()
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [this] { return released; });
        returned = true;
        condition.notify_all();
    }

    bool wait_entered()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(2),
                                  [this] { return entered; });
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        condition.notify_all();
    }

    bool wait_returned()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(2),
                                  [this] { return returned; });
    }
};

class ScopedEnvironment {
public:
    ScopedEnvironment(std::string name, const std::string& value) : m_name(std::move(name))
    {
        if (const char* previous = std::getenv(m_name.c_str())) m_previous = previous;
        assign(value);
    }

    ~ScopedEnvironment()
    {
        if (m_previous) assign(*m_previous);
        else clear();
    }

private:
    void assign(const std::string& value)
    {
#ifdef _WIN32
        _putenv_s(m_name.c_str(), value.c_str());
#else
        setenv(m_name.c_str(), value.c_str(), 1);
#endif
    }

    void clear()
    {
#ifdef _WIN32
        _putenv_s(m_name.c_str(), "");
#else
        unsetenv(m_name.c_str());
#endif
    }

    std::string m_name;
    std::optional<std::string> m_previous;
};

} // namespace

TEST_CASE("Platform configuration accepts only a public HTTPS origin and fixed callback",
          "[HonorAccount]")
{
    auto config = configuration();
    REQUIRE(config.validate().empty());
    REQUIRE(config.callback_uri == "honorforge://auth/callback");

    const std::vector<std::string> rejected = {
        "http://platform.example.test",       "https://user@platform.example.test",
        "https://platform.example.test/path", "https://platform.example.test?query=1",
        "https://platform.example.test#part", "https://platform.example.test:0",
        "https://platform.example.test:70000", "https://999.1.1.1"};
    for (const auto& origin : rejected) {
        INFO(origin);
        config = configuration();
        config.api_url = origin;
        REQUIRE_FALSE(config.validate().empty());
    }

    config = configuration();
    config.callback_uri = "honorforge://auth/other";
    REQUIRE_FALSE(config.validate().empty());
    config = configuration();
    config.browser_profile_key = "not-a-fingerprint";
    REQUIRE_FALSE(config.validate().empty());

    config = configuration();
    config.browser_profile_key = digest("legacy profile");
    const auto restored = parse_config(serialize_config(config));
    REQUIRE(restored.api_url == config.api_url);
    REQUIRE(restored.callback_uri == config.callback_uri);
    REQUIRE(restored.browser_key() == config.browser_profile_key);
    auto invalid = Json::parse(serialize_config(config));
    invalid["unexpected"] = true;
    REQUIRE_THROWS_AS(parse_config(invalid.dump()), Error);
}

TEST_CASE("Platform API provisioning reads only ORCA_PLATFORM_API_URL", "[HonorAccount]")
{
    SECTION("valid origin") {
        ScopedEnvironment environment("ORCA_PLATFORM_API_URL", "https://provisioned.example.test");
        auto memory = std::make_shared<Memory>();
        Config provisioned;
        {
            Session session(std::make_unique<MemoryStore>(memory));
            provisioned = session.config();
            REQUIRE(provisioned.api_url == "https://provisioned.example.test");
            REQUIRE(provisioned.callback_uri == "honorforge://auth/callback");
            REQUIRE(provisioned.browser_profile_key.empty());
            REQUIRE(memory->get("configuration") == serialize_config(provisioned));
        }

        const auto saved = platform_session();
        memory->put(session_key(provisioned), serialize_session(saved));
        std::atomic_bool bearer_matches {false};
        Session reopened(std::make_unique<MemoryStore>(memory),
                         [&](const Request& request, Cancellation) {
                             bearer_matches.store(request.bearer == saved.access_token);
                             return Response{200, account_response(saved.identity)};
                         });
        REQUIRE(reopened.config().browser_profile_key.empty());
        REQUIRE(memory->has(session_key(provisioned)));
        reopened.restore();
        REQUIRE(wait_for_session(reopened, [&] {
            return reopened.snapshot().state == State::SignedIn;
        }));
        REQUIRE(bearer_matches.load());
    }
    SECTION("invalid origin") {
        ScopedEnvironment environment("ORCA_PLATFORM_API_URL", "https://provisioned.example.test/path");
        auto memory = std::make_shared<Memory>();
        Session session(std::make_unique<MemoryStore>(memory));
        REQUIRE_FALSE(session.config().validate().empty());
        REQUIRE_FALSE(memory->has("configuration"));
        REQUIRE_FALSE(session.login());
        REQUIRE(session.snapshot().state == State::Failed);
    }
}

TEST_CASE("PKCE uses the RFC 7636 S256 vector and bounded random verifiers", "[HonorAccount]")
{
    REQUIRE(pkce_challenge(rfc_verifier) == rfc_challenge);
    const auto verifier = pkce_verifier();
    REQUIRE(verifier.size() >= 43);
    REQUIRE(verifier.size() <= 128);
    REQUIRE_NOTHROW(pkce_challenge(verifier));
    REQUIRE_THROWS_AS(pkce_challenge("too-short"), Error);
}

TEST_CASE("Platform callback accepts one opaque code and rejects ambiguous input", "[HonorAccount]")
{
    const auto code = fixture_token('C');
    const auto callback = configuration().callback_uri;
    REQUIRE(is_callback(callback + "?code=" + code, callback));
    REQUIRE(callback_code(callback + "?code=" + code, callback) == code);
    REQUIRE(callback_code(callback + "?code=%43" + code.substr(1), callback) == code);

    REQUIRE_FALSE(is_callback("honorforge://auth.evil/callback?code=" + code, callback));
    REQUIRE_FALSE(is_callback("honorforge://auth/callback/extra?code=" + code, callback));
    REQUIRE_THROWS_AS(callback_code(callback + "?code=" + code + "&code=" + code, callback),
                      Error);
    REQUIRE_THROWS_AS(callback_code(callback + "?code=" + code + "&", callback), Error);
    REQUIRE_THROWS_AS(callback_code(callback + "?code=" + code + "&extra=1", callback), Error);
    REQUIRE_THROWS_AS(callback_code(callback + "?code=" + code + "#fragment", callback), Error);
    REQUIRE_THROWS_AS(callback_code(callback + "?code=%xx", callback), Error);
    REQUIRE_THROWS_AS(callback_code(callback + "?code=short", callback), Error);

    try {
        callback_code(callback + "?error=HONOR_ACCESS_DENIED", callback);
        FAIL("Expected a safe authorization failure");
    } catch (const Error& error) {
        REQUIRE(error.kind == Failure::Cancelled);
        REQUIRE(std::string(error.what()).find("HONOR_ACCESS_DENIED") == std::string::npos);
    }
    try {
        callback_code(callback + "?error=HONOR_PRIVATE_UPSTREAM_DETAIL", callback);
        FAIL("Expected a safe authorization failure");
    } catch (const Error& error) {
        REQUIRE(std::string(error.what()).find("PRIVATE_UPSTREAM_DETAIL") == std::string::npos);
    }
}

TEST_CASE("Only bounded HTTPS authorization pages are accepted", "[HonorAccount]")
{
    REQUIRE(valid_authorization_url("https://id.example.test/authorize?state=fixture"));
    REQUIRE_FALSE(valid_authorization_url("http://id.example.test/authorize"));
    REQUIRE_FALSE(valid_authorization_url("https://user@id.example.test/authorize"));
    REQUIRE_FALSE(valid_authorization_url("https://id.example.test/authorize#code=private"));
    REQUIRE_FALSE(valid_authorization_url("https://id.example.test/authorize value"));
}

TEST_CASE("Authorization diagnostics classify pages without exposing their contents",
          "[HonorAccount]")
{
    const auto callback = configuration().callback_uri;
    struct Case {
        const char* url;
        AuthorizationPage page;
        const char* name;
        bool submit_allowed;
    };
    const std::vector<Case> cases = {
        {"about:blank", AuthorizationPage::Blank, "blank", false},
        {"https://hnid-test-drcn.cloud.honor.com/CAS/portal/login",
         AuthorizationPage::CasPortal, "cas_portal", true},
        {"https://hnoauth-login-test-drcn.cloud.honor.com/oauth2/v3/login",
         AuthorizationPage::OAuthLogin, "oauth_login", true},
        {"https://hnoauth-login.cloud.honor.com:443/oauth2/v3/authorize",
         AuthorizationPage::OAuthAuthorize, "oauth_authorize", true},
        {"honorforge://auth/callback?code=CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC",
         AuthorizationPage::Callback, "callback", false},
        {"https://id.example.test/authorize?private=query",
         AuthorizationPage::OtherHttps, "other_https", false},
        {"https://hnoauth-login.cloud.honor.com:444/oauth2/v3/login",
         AuthorizationPage::OtherHttps, "other_https", false},
        {"https://hnoauth-login.cloud.honor.com.evil.test/oauth2/v3/login",
         AuthorizationPage::OtherHttps, "other_https", false},
        {"http://hnoauth-login.cloud.honor.com/oauth2/v3/login",
         AuthorizationPage::Blocked, "blocked", false},
    };
    for (const auto& item : cases) {
        INFO(item.url);
        const auto page = classify_authorization_page(item.url, callback);
        REQUIRE(page == item.page);
        REQUIRE(std::string(authorization_page_name(page)) == item.name);
        REQUIRE(authorization_submit_allowed(page) == item.submit_allowed);
    }
}

TEST_CASE("Platform account and session JSON is strict and versioned", "[HonorAccount]")
{
    const auto original = platform_session();
    const auto saved = serialize_session(original);
    const auto restored = parse_session(saved);
    REQUIRE(restored.session_id == original.session_id);
    REQUIRE(restored.access_token == original.access_token);
    REQUIRE(restored.refresh_token == original.refresh_token);
    REQUIRE(restored.identity.subject == original.identity.subject);
    REQUIRE(restored.identity.email == original.identity.email);
    REQUIRE(restored.identity.roles == original.identity.roles);

    const auto response = parse_session_response(session_response(original));
    REQUIRE(response.session_id == original.session_id);
    REQUIRE(response.identity.name == original.identity.name);
    REQUIRE(parse_account_response(account_response()).subject == account_id);

    auto local = Json::parse(saved);
    local["unexpected"] = true;
    REQUIRE_THROWS_AS(parse_session(local.dump()), Error);
    local = Json::parse(saved);
    local["kind"] = "web";
    REQUIRE_THROWS_AS(parse_session(local.dump()), Error);

    auto remote = Json::parse(session_response(original));
    remote["account"]["status"] = "disabled";
    REQUIRE_THROWS_AS(parse_session_response(remote.dump()), Error);
    remote = Json::parse(session_response(original));
    remote["account"]["roles"] = Json::array({"user", "user"});
    REQUIRE_THROWS_AS(parse_session_response(remote.dump()), Error);
    remote = Json::parse(session_response(original));
    remote["account"]["id"] = "not-a-uuid";
    REQUIRE_THROWS_AS(parse_session_response(remote.dump()), Error);
    remote = Json::parse(session_response(original));
    remote["accessExpiresAt"] = "2099-01-01T00:00:00+08:00";
    REQUIRE_THROWS_AS(parse_session_response(remote.dump()), Error);
    remote = Json::parse(session_response(original));
    remote["unexpected"] = true;
    REQUIRE_THROWS_AS(parse_session_response(remote.dump()), Error);

    REQUIRE(is_refresh_tombstone(serialize_refresh_tombstone()));
    REQUIRE_FALSE(is_refresh_tombstone(saved));
    REQUIRE(parse_timestamp("2028-02-29T12:34:56.123Z") > 0);
    REQUIRE_THROWS_AS(parse_timestamp("2027-02-29T12:34:56Z"), Error);
}

TEST_CASE("Platform client follows the existing mobile browser request contract", "[HonorAccount]")
{
    const auto config = configuration();
    auto issued = platform_session();
    auto rotated = platform_session(now_seconds() + 7200, now_seconds() + 172800);
    rotated.session_id = rotated_session_id;
    rotated.access_token = fixture_token('B');
    rotated.refresh_token = fixture_token('S');
    std::vector<Request> requests;
    Transport transport = [&](const Request& request, Cancellation) {
        requests.push_back(request);
        switch (requests.size()) {
        case 1: return attempt_response();
        case 2: return authorization_response();
        case 3: return Response{201, session_response(issued)};
        case 4: return Response{200, account_response(issued.identity)};
        case 5: return Response{201, session_response(rotated)};
        case 6: return Response{201, R"({"ok":true})"};
        default: return Response{500, R"({"error":{"code":"UNEXPECTED_REQUEST"}})"};
        }
    };
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    Client client(transport);
    const auto authorization = client.authorize(config, rfc_verifier, cancel);
    const auto exchanged = client.exchange(config, fixture_token('C'), rfc_verifier, cancel);
    const auto validated = client.validate(config, exchanged, cancel);
    const auto refreshed = client.refresh(config, validated, cancel);
    client.revoke(config, refreshed.refresh_token, cancel);

    REQUIRE(authorization.url == "https://id.example.test/authorize?state=fixture-state");
    REQUIRE(exchanged.identity.subject == account_id);
    REQUIRE(refreshed.session_id == rotated_session_id);
    REQUIRE(requests.size() == 6);

    REQUIRE(requests[0].method == Request::Method::Post);
    REQUIRE(requests[0].url == config.api_url + "/v1/identity/honor/mobile/attempts");
    REQUIRE((Json::parse(requests[0].json) ==
             Json{{"platform", "ios"}, {"challenge", rfc_challenge}}));
    REQUIRE(requests[0].bearer.empty());
    REQUIRE(requests[1].method == Request::Method::Get);
    REQUIRE(requests[1].url == config.api_url +
                                   "/v1/identity/honor/mobile/authorize?attemptId=" +
                                   fixture_token('I'));
    REQUIRE(requests[1].json.empty());
    REQUIRE(requests[2].url == config.api_url + "/v1/identity/exchange");
    REQUIRE((Json::parse(requests[2].json) ==
             Json{{"code", fixture_token('C')},
                  {"verifier", rfc_verifier},
                  {"kind", "mobile"}}));
    REQUIRE(requests[2].url.find(fixture_token('C')) == std::string::npos);
    REQUIRE(requests[3].url == config.api_url + "/v1/me");
    REQUIRE(requests[3].bearer == issued.access_token);
    REQUIRE((Json::parse(requests[4].json) ==
             Json{{"refreshToken", issued.refresh_token}}));
    REQUIRE((Json::parse(requests[5].json) ==
             Json{{"refreshToken", rotated.refresh_token}}));
}

TEST_CASE("Platform errors expose only safe local messages", "[HonorAccount]")
{
    Client client([](const Request&, Cancellation) {
        return Response{418,
                        R"({"error":{"code":"PRIVATE_UPSTREAM_FAILURE","message":"fixture-code-and-token"}})"};
    });
    try {
        client.exchange(configuration(), fixture_token('C'), rfc_verifier,
                        std::make_shared<std::atomic_bool>(false));
        FAIL("Expected a protocol error");
    } catch (const Error& error) {
        const std::string message = error.what();
        REQUIRE(message.find("HTTP 418") != std::string::npos);
        REQUIRE(message.find("PRIVATE_UPSTREAM_FAILURE") == std::string::npos);
        REQUIRE(message.find("fixture-code-and-token") == std::string::npos);
    }
}

TEST_CASE("Slow platform attempt can be cancelled without publishing a late URL",
          "[HonorAccount]")
{
    auto memory = configured_memory();
    Gate gate;
    std::atomic_int requests {0};
    Session session(std::make_unique<MemoryStore>(memory),
                    [&](const Request&, Cancellation) {
                        ++requests;
                        gate.block();
                        return attempt_response();
                    });
    session.restore();
    REQUIRE(session.snapshot().state == State::SignedOut);
    REQUIRE(session.login());
    REQUIRE(session.snapshot().state == State::Preparing);
    REQUIRE(gate.wait_entered());
    session.cancel();
    REQUIRE(session.snapshot().state == State::SignedOut);
    gate.release();
    REQUIRE(gate.wait_returned());
    session.poll();
    REQUIRE(session.snapshot().state == State::SignedOut);
    REQUIRE_FALSE(session.take_authorization_url().has_value());
    REQUIRE(requests.load() == 1);
}

TEST_CASE("Authorization URL is consumed once and duplicate callbacks exchange once",
          "[HonorAccount]")
{
    auto memory = configured_memory();
    Gate exchange_gate;
    std::atomic_int exchanges {0};
    Transport transport = [&](const Request& request, Cancellation) {
        if (request.url.find("/mobile/attempts") != std::string::npos)
            return attempt_response();
        if (request.url.find("/mobile/authorize") != std::string::npos)
            return authorization_response();
        if (request.url.find("/identity/exchange") != std::string::npos) {
            ++exchanges;
            exchange_gate.block();
            return Response{201, session_response(platform_session())};
        }
        return Response{500, {}};
    };
    Session session(std::make_unique<MemoryStore>(memory), transport);
    REQUIRE(session.login());
    REQUIRE(wait_for_session(session, [&] {
        return session.snapshot().state == State::Authorizing;
    }));
    REQUIRE(session.take_authorization_url().has_value());
    REQUIRE_FALSE(session.take_authorization_url().has_value());
    const auto callback = configuration().callback_uri + "?code=" + fixture_token('C');
    REQUIRE(session.callback(callback));
    REQUIRE(exchange_gate.wait_entered());
    REQUIRE(session.callback(callback));
    REQUIRE(exchanges.load() == 1);

    REQUIRE(session.logout());
    REQUIRE(session.snapshot().state == State::SignedOut);
    exchange_gate.release();
    REQUIRE(exchange_gate.wait_returned());
    session.poll();
    REQUIRE(session.snapshot().state == State::SignedOut);
    REQUIRE_FALSE(memory->has(session_key(configuration())));
    REQUIRE(session.callback(callback));
    REQUIRE(exchanges.load() == 1);
}

TEST_CASE("A stale authorization generation cannot complete a newer login",
          "[HonorAccount]")
{
    auto memory = configured_memory();
    std::atomic_int exchanges {0};
    Transport transport = [&](const Request& request, Cancellation) {
        if (request.url.find("/mobile/attempts") != std::string::npos)
            return attempt_response();
        if (request.url.find("/mobile/authorize") != std::string::npos)
            return authorization_response();
        if (request.url.find("/identity/exchange") != std::string::npos) {
            ++exchanges;
            return Response{201, session_response(platform_session())};
        }
        return Response{500, {}};
    };
    Session session(std::make_unique<MemoryStore>(memory), transport);
    REQUIRE(session.login());
    REQUIRE(wait_for_session(session, [&] {
        return session.snapshot().state == State::Authorizing;
    }));
    const auto generation_a = session.generation();
    REQUIRE(session.take_authorization_url().has_value());

    session.cancel();
    REQUIRE(session.snapshot().state == State::SignedOut);
    REQUIRE(session.generation() != generation_a);
    REQUIRE(session.login());
    REQUIRE(wait_for_session(session, [&] {
        return session.snapshot().state == State::Authorizing;
    }));
    const auto generation_b = session.generation();
    REQUIRE(generation_b != generation_a);
    REQUIRE(session.take_authorization_url().has_value());

    const auto callback = configuration().callback_uri + "?code=" + fixture_token('C');
    REQUIRE(session.callback(callback, generation_a));
    REQUIRE(session.snapshot().state == State::Authorizing);
    REQUIRE(session.generation() == generation_b);
    REQUIRE(exchanges.load() == 0);

    REQUIRE(session.callback(callback, generation_b));
    REQUIRE(wait_for_session(session, [&] {
        return session.snapshot().state == State::SignedIn;
    }));
    REQUIRE(session.generation() == generation_b);
    REQUIRE(exchanges.load() == 1);
    REQUIRE(session.callback(callback, generation_b));
    REQUIRE(exchanges.load() == 1);
}

TEST_CASE("Expired browser callback fails without exchanging its code", "[HonorAccount]")
{
    auto memory = configured_memory();
    const auto expires_at = now_seconds() + 3;
    std::atomic_int exchanges {0};
    Session session(std::make_unique<MemoryStore>(memory),
                    [&](const Request& request, Cancellation) {
                        if (request.url.find("/mobile/attempts") != std::string::npos)
                            return attempt_response(expires_at);
                        if (request.url.find("/mobile/authorize") != std::string::npos)
                            return authorization_response();
                        ++exchanges;
                        return Response{201, session_response(platform_session())};
                    });
    REQUIRE(session.login());
    REQUIRE(wait_for_session(session, [&] {
        return session.snapshot().state == State::Authorizing;
    }));
    while (now_seconds() < expires_at)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(session.callback(configuration().callback_uri + "?code=" + fixture_token('C')));
    REQUIRE(session.snapshot().state == State::Failed);
    REQUIRE(exchanges.load() == 0);
    REQUIRE_FALSE(session.take_authorization_url().has_value());
}

TEST_CASE("Startup validates the saved account and handles offline or mismatched identity",
          "[HonorAccount]")
{
    SECTION("matching account") {
        const auto saved = platform_session();
        auto memory = configured_memory(saved);
        std::atomic_bool bearer_matches {false};
        Session session(std::make_unique<MemoryStore>(memory),
                        [&](const Request& request, Cancellation) {
                            bearer_matches.store(request.bearer == saved.access_token);
                            return Response{200, account_response(saved.identity)};
                        });
        session.restore();
        REQUIRE(wait_for_session(session, [&] {
            return session.snapshot().state == State::SignedIn;
        }));
        REQUIRE(session.snapshot().identity.subject == account_id);
        REQUIRE(session.snapshot().persistent);
        REQUIRE(bearer_matches.load());
    }

    SECTION("temporary outage") {
        const auto saved = platform_session();
        auto memory = configured_memory(saved);
        Session session(std::make_unique<MemoryStore>(memory),
                        [](const Request&, Cancellation) {
                            return Response{503,
                                            R"({"error":{"code":"UPSTREAM_PRIVATE_DETAIL"}})"};
                        });
        session.restore();
        REQUIRE(wait_for_session(session, [&] {
            return session.snapshot().state == State::Offline;
        }));
        REQUIRE(session.snapshot().identity.subject == account_id);
        REQUIRE(memory->has(session_key(configuration())));
        REQUIRE(session.snapshot().message.find("UPSTREAM_PRIVATE_DETAIL") == std::string::npos);
    }

    SECTION("account mismatch") {
        auto memory = configured_memory(platform_session());
        Session session(std::make_unique<MemoryStore>(memory),
                        [](const Request&, Cancellation) {
                            return Response{200, account_response(identity(other_account_id))};
                        });
        session.restore();
        REQUIRE(wait_for_session(session, [&] {
            return session.snapshot().state == State::Failed;
        }));
        REQUIRE_FALSE(session.snapshot().has_identity());
        REQUIRE_FALSE(memory->has(session_key(configuration())));
    }

    SECTION("revoked access") {
        auto memory = configured_memory(platform_session());
        Session session(std::make_unique<MemoryStore>(memory),
                        [](const Request&, Cancellation) {
                            return Response{401,
                                            R"({"error":{"code":"SESSION_REVOKED"}})"};
                        });
        session.restore();
        REQUIRE(wait_for_session(session, [&] {
            return session.snapshot().state == State::Failed;
        }));
        REQUIRE_FALSE(memory->has(session_key(configuration())));
    }
}

TEST_CASE("Refresh writes a tombstone before rotating both platform tokens", "[HonorAccount]")
{
    auto previous = platform_session(now_seconds() + 30, now_seconds() + 86400);
    auto rotated = platform_session(now_seconds() + 3600, now_seconds() + 172800);
    rotated.session_id = rotated_session_id;
    rotated.access_token = fixture_token('B');
    rotated.refresh_token = fixture_token('S');
    auto memory = configured_memory(previous);
    std::atomic_bool saw_tombstone {false};
    Session session(std::make_unique<MemoryStore>(memory),
                    [&](const Request& request, Cancellation) {
                        const auto stored = memory->get(session_key(configuration()));
                        saw_tombstone.store(stored && is_refresh_tombstone(*stored));
                        if (Json::parse(request.json)["refreshToken"] != previous.refresh_token)
                            return Response{400, {}};
                        return Response{201, session_response(rotated)};
                    });
    session.restore();
    REQUIRE(wait_for_session(session, [&] {
        return session.snapshot().state == State::SignedIn;
    }));
    REQUIRE(saw_tombstone.load());
    const auto stored = memory->get(session_key(configuration()));
    REQUIRE(stored.has_value());
    const auto saved = parse_session(*stored);
    REQUIRE(saved.session_id == rotated_session_id);
    REQUIRE(saved.access_token == rotated.access_token);
    REQUIRE(saved.refresh_token == rotated.refresh_token);
}

TEST_CASE("Unknown refresh result clears the local session instead of reusing a rotated token",
          "[HonorAccount]")
{
    auto previous = platform_session(now_seconds() + 30, now_seconds() + 86400);
    auto memory = configured_memory(previous);
    std::atomic_bool saw_tombstone {false};
    Session session(std::make_unique<MemoryStore>(memory),
                    [&](const Request&, Cancellation) {
                        const auto stored = memory->get(session_key(configuration()));
                        saw_tombstone.store(stored && is_refresh_tombstone(*stored));
                        return Response{503,
                                        R"({"error":{"code":"PRIVATE_REFRESH_RESULT"}})"};
                    });
    session.restore();
    REQUIRE(wait_for_session(session, [&] {
        return session.snapshot().state == State::Failed;
    }));
    REQUIRE(saw_tombstone.load());
    REQUIRE_FALSE(session.snapshot().has_identity());
    REQUIRE_FALSE(memory->has(session_key(configuration())));
    REQUIRE(session.snapshot().message.find("PRIVATE_REFRESH_RESULT") == std::string::npos);
}

TEST_CASE("Refresh never starts unless its crash tombstone is durably stored", "[HonorAccount]")
{
    auto previous = platform_session(now_seconds() + 30, now_seconds() + 86400);
    auto memory = configured_memory(previous);
    memory->failed_saves.insert(session_key(configuration()));
    std::atomic_int requests {0};
    Session session(std::make_unique<MemoryStore>(memory),
                    [&](const Request&, Cancellation) {
                        ++requests;
                        return Response{};
                    });
    session.restore();
    REQUIRE(session.snapshot().state == State::Offline);
    REQUIRE(session.snapshot().has_identity());
    REQUIRE(requests.load() == 0);
    REQUIRE(memory->get(session_key(configuration())) == serialize_session(previous));
}

TEST_CASE("Startup discards an interrupted refresh tombstone and requires login",
          "[HonorAccount]")
{
    auto memory = configured_memory();
    memory->put(session_key(configuration()), serialize_refresh_tombstone());
    std::atomic_int requests {0};
    Session session(std::make_unique<MemoryStore>(memory),
                    [&](const Request&, Cancellation) {
                        ++requests;
                        return Response{};
                    });
    session.restore();
    REQUIRE(session.snapshot().state == State::SignedOut);
    REQUIRE_FALSE(session.snapshot().has_identity());
    REQUIRE_FALSE(memory->has(session_key(configuration())));
    REQUIRE(requests.load() == 0);
}

TEST_CASE("Logout clears locally before best effort remote revocation", "[HonorAccount]")
{
    const auto saved = platform_session();
    auto memory = configured_memory(saved);
    std::atomic_bool cleared_before_revoke {false};
    Session session(std::make_unique<MemoryStore>(memory),
                    [&](const Request& request, Cancellation) {
                        if (request.url.find("/v1/me") != std::string::npos)
                            return Response{200, account_response(saved.identity)};
                        cleared_before_revoke.store(!memory->has(session_key(configuration())));
                        return Response{503,
                                        R"({"error":{"code":"PRIVATE_REVOKE_FAILURE"}})"};
                    });
    session.restore();
    REQUIRE(wait_for_session(session, [&] {
        return session.snapshot().state == State::SignedIn;
    }));
    REQUIRE(session.logout());
    REQUIRE(session.snapshot().state == State::SignedOut);
    REQUIRE_FALSE(session.snapshot().has_identity());
    REQUIRE_FALSE(memory->has(session_key(configuration())));
    REQUIRE(wait_for_session(session, [&] {
        return session.snapshot().message.find("远端撤销未确认") != std::string::npos;
    }));
    REQUIRE(cleared_before_revoke.load());

    std::atomic_int requests {0};
    Session reopened(std::make_unique<MemoryStore>(memory),
                     [&](const Request&, Cancellation) {
                         ++requests;
                         return Response{};
                     });
    reopened.restore();
    REQUIRE(reopened.snapshot().state == State::SignedOut);
    REQUIRE(requests.load() == 0);
}

TEST_CASE("Browser cleanup requests persist without restoring the platform session",
          "[HonorAccount]")
{
    const auto saved = platform_session();
    auto memory = configured_memory(saved);
    Session session(std::make_unique<MemoryStore>(memory),
                    [&](const Request& request, Cancellation) {
                        if (request.url.find("/v1/me") != std::string::npos)
                            return Response{200, account_response(saved.identity)};
                        return Response{201, R"({"ok":true})"};
                    });
    session.restore();
    REQUIRE(wait_for_session(session, [&] {
        return session.snapshot().state == State::SignedIn;
    }));
    REQUIRE(session.logout(true));
    REQUIRE(session.browser_reset_pending());
    REQUIRE_FALSE(memory->has(session_key(configuration())));
    REQUIRE(memory->has("browser-reset-platform-v2"));
    REQUIRE(session.complete_browser_reset());
    REQUIRE_FALSE(session.browser_reset_pending());
    REQUIRE_FALSE(memory->has("browser-reset-platform-v2"));
}

TEST_CASE("A failed browser cleanup request remains mandatory before a new login",
          "[HonorAccount]")
{
    const auto saved = platform_session();
    auto memory = configured_memory(saved);
    memory->failed_saves.insert("browser-reset-platform-v2");
    Session session(std::make_unique<MemoryStore>(memory),
                    [&](const Request& request, Cancellation) {
                        if (request.url.find("/v1/me") != std::string::npos)
                            return Response{200, account_response(saved.identity)};
                        if (request.url.find("/session/revoke") != std::string::npos)
                            return Response{201, R"({"ok":true})"};
                        return attempt_response();
                    });
    session.restore();
    REQUIRE(wait_for_session(session, [&] {
        return session.snapshot().state == State::SignedIn;
    }));

    REQUIRE(session.logout(true));
    REQUIRE_FALSE(memory->has("browser-reset-platform-v2"));
    REQUIRE(session.browser_reset_pending());
    REQUIRE(session.login());
    REQUIRE(session.browser_reset_pending());
    session.cancel();
}

TEST_CASE("Old direct credentials are erased while the browser profile is retained",
          "[HonorAccount]")
{
    const auto legacy = Json{{"environment", "test"},
                             {"client_id", "legacy-client"},
                             {"client_secret", "legacy-secret-must-disappear"},
                             {"redirect_uri", "https://legacy.example.test/callback"},
                             {"authorize_url", "https://legacy.example.test/authorize"},
                             {"token_url", "https://legacy.example.test/token"},
                             {"jwks_url", "https://legacy.example.test/certs"},
                             {"issuer", "https://legacy.example.test"}}
                            .dump();
    const auto profile = legacy_profile_key(legacy);
    REQUIRE(profile.has_value());
    auto memory = std::make_shared<Memory>();
    memory->put("configuration", legacy);
    memory->put("session-" + *profile, "legacy-honor-access-and-refresh-token");
    memory->put("browser-reset-" + *profile, "pending");

    Session session(std::make_unique<MemoryStore>(memory));
    REQUIRE(session.config().api_url.empty());
    REQUIRE(session.config().browser_profile_key == *profile);
    REQUIRE_FALSE(memory->has("session-" + *profile));
    REQUIRE_FALSE(memory->has("browser-reset-" + *profile));
    REQUIRE(memory->has("browser-reset-platform-v2"));
    const auto migrated = memory->get("configuration");
    REQUIRE(migrated.has_value());
    REQUIRE(migrated->find("legacy-secret-must-disappear") == std::string::npos);
    REQUIRE(migrated->find("legacy-honor-access-and-refresh-token") == std::string::npos);
    REQUIRE(parse_config(*migrated).browser_profile_key == *profile);
}

TEST_CASE("Application profiles share only the public platform origin", "[HonorAccount]")
{
    auto profile = configured_memory();
    auto configured = configuration();
    configured.browser_profile_key = digest("profile-isolation");
    profile->put("configuration", serialize_config(configured));
    auto application = std::make_shared<Memory>();
    Session first(std::make_unique<MemoryStore>(profile), https_request,
                  std::make_unique<MemoryStore>(application));
    const auto shared = parse_config(*application->get("configuration"));
    REQUIRE(shared.api_url == configured.api_url);
    REQUIRE(shared.browser_profile_key.empty());

    auto second_profile = std::make_shared<Memory>();
    Session second(std::make_unique<MemoryStore>(second_profile), https_request,
                   std::make_unique<MemoryStore>(application));
    REQUIRE(second.config().api_url == configured.api_url);
    REQUIRE(second.config().browser_profile_key.empty());
    REQUIRE_FALSE(second_profile->has(session_key(configured)));
}

#ifdef __WXMSW__
TEST_CASE("Windows protected storage atomically rotates long UTF8 platform sessions",
          "[HonorAccount]")
{
    const auto profile = "offline-platform-test-" + random_value();
    auto store = system_store(profile);
    std::string value;
    for (int i = 0; i < 2000; ++i) value += "\xE6\xB5\x8B\xE8\xAF\x95";
    REQUIRE(store->save("long", value));
    REQUIRE(store->load("long") == value);
    REQUIRE(store->save("long", value + "-rotated"));
    REQUIRE(store->load("long") == value + "-rotated");
    REQUIRE(store->erase("long"));
    REQUIRE_FALSE(store->load("long").has_value());
}
#endif
