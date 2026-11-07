#include "HonorOAuth.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <initializer_list>
#include <map>
#include <regex>
#include <set>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace Slic3r::HonorAccount {
namespace {

using Json = nlohmann::json;

[[noreturn]] void protocol(const char* message)
{
    throw Error(Failure::Protocol, message);
}

struct Uri {
    std::string scheme;
    std::string authority;
    std::string path;
    std::string query;
    std::string fragment;
};

Uri uri(const std::string& value)
{
    if (value.empty() || value.size() > 65536 ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) {
            return c <= 0x20 || c == 0x7f || c == '\\';
        }))
        protocol("地址格式无效");
    static const std::regex pattern(
        R"(^([A-Za-z][A-Za-z0-9+.-]*):(?://([^/?#]*))?([^?#]*)(?:\?([^#]*))?(?:#(.*))?$)");
    std::smatch match;
    if (!std::regex_match(value, match, pattern)) protocol("地址格式无效");
    std::string scheme = match[1].str();
    std::string authority = match[2].str();
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    std::transform(authority.begin(), authority.end(), authority.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (authority.find('@') != std::string::npos) protocol("地址不能包含用户凭据");
    return {scheme, authority, match[3].str(), match[4].str(), match[5].str()};
}

bool valid_port(std::string_view value)
{
    if (value.empty() || value.size() > 5) return false;
    unsigned port = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') return false;
        port = port * 10 + unsigned(c - '0');
    }
    return port > 0 && port <= 65535;
}

bool valid_ipv4(std::string_view value)
{
    unsigned parts = 0;
    size_t begin = 0;
    while (begin < value.size()) {
        const auto end = value.find('.', begin);
        const auto part = value.substr(begin, end == std::string_view::npos
                                                  ? value.size() - begin
                                                  : end - begin);
        if (part.empty() || part.size() > 3) return false;
        unsigned number = 0;
        for (const char c : part) {
            if (c < '0' || c > '9') return false;
            number = number * 10 + unsigned(c - '0');
        }
        if (number > 255 || (part.size() > 1 && part.front() == '0')) return false;
        ++parts;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return parts == 4;
}

bool valid_ipv6(std::string_view value)
{
    if (value.empty() || value.find('.') != std::string_view::npos) return false;
    const auto compressed = value.find("::");
    if (compressed != std::string_view::npos &&
        value.find("::", compressed + 2) != std::string_view::npos)
        return false;
    if ((value.front() == ':' && compressed != 0) ||
        (value.back() == ':' && compressed != value.size() - 2))
        return false;

    unsigned groups = 0;
    size_t begin = 0;
    while (begin < value.size()) {
        const auto end = value.find(':', begin);
        const auto group = value.substr(begin, end == std::string_view::npos
                                                   ? value.size() - begin
                                                   : end - begin);
        if (!group.empty()) {
            if (group.size() > 4 ||
                !std::all_of(group.begin(), group.end(), [](unsigned char c) {
                    return std::isxdigit(c) != 0;
                }))
                return false;
            ++groups;
        }
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return compressed == std::string_view::npos ? groups == 8 : groups < 8;
}

bool valid_host(std::string_view value)
{
    if (value.empty() || value.size() > 253 || value.front() == '.' || value.back() == '.')
        return false;
    const bool numeric = std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) != 0 || c == '.';
    });
    if (numeric) return value.find('.') != std::string_view::npos && valid_ipv4(value);

    size_t begin = 0;
    while (begin < value.size()) {
        const auto end = value.find('.', begin);
        const auto label = value.substr(begin, end == std::string_view::npos
                                                   ? value.size() - begin
                                                   : end - begin);
        if (label.empty() || label.size() > 63 ||
            !std::isalnum(static_cast<unsigned char>(label.front())) ||
            !std::isalnum(static_cast<unsigned char>(label.back())) ||
            !std::all_of(label.begin(), label.end(), [](unsigned char c) {
                return std::isalnum(c) != 0 || c == '-';
            }))
            return false;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return true;
}

bool valid_authority(const std::string& authority)
{
    if (authority.empty() || authority.size() > 320 || authority.find('@') != std::string::npos)
        return false;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos || !valid_ipv6(
                std::string_view(authority).substr(1, close - 1)))
            return false;
        if (close + 1 == authority.size()) return true;
        return authority[close + 1] == ':' &&
               valid_port(std::string_view(authority).substr(close + 2));
    }
    const auto colon = authority.find(':');
    if (colon == std::string::npos) return valid_host(authority);
    if (authority.find(':', colon + 1) != std::string::npos) return false;
    return valid_host(std::string_view(authority).substr(0, colon)) &&
           valid_port(std::string_view(authority).substr(colon + 1));
}

std::string decode(const std::string& value)
{
    std::string out;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%') {
            if (i + 2 >= value.size() || hex(value[i + 1]) < 0 || hex(value[i + 2]) < 0)
                protocol("回调参数编码无效");
            const char c = char(hex(value[i + 1]) * 16 + hex(value[i + 2]));
            if (!c || c == '\r' || c == '\n') protocol("回调参数无效");
            out += c;
            i += 2;
        } else {
            out += value[i] == '+' ? ' ' : value[i];
        }
    }
    return out;
}

std::map<std::string, std::string> query(const std::string& value)
{
    std::map<std::string, std::string> result;
    if (value.empty()) return result;
    size_t begin = 0;
    while (begin < value.size()) {
        auto end = value.find('&', begin);
        if (end == std::string::npos) end = value.size();
        const auto field = value.substr(begin, end - begin);
        if (field.empty()) protocol("回调参数无效");
        const auto equal = field.find('=');
        if (equal == std::string::npos) protocol("回调参数无效");
        const auto name = decode(field.substr(0, equal));
        if (name.empty() || !result.emplace(name, decode(field.substr(equal + 1))).second)
            protocol("回调含重复参数");
        if (end == value.size()) break;
        begin = end + 1;
        if (begin == value.size()) protocol("回调参数无效");
    }
    return result;
}

std::string base64url(const unsigned char* data, size_t size)
{
    std::string encoded(4 * ((size + 2) / 3), '\0');
    const int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()), data, int(size));
    if (length <= 0) protocol("无法生成安全授权请求");
    encoded.resize(size_t(length));
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    for (char& c : encoded) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return encoded;
}

bool exact_keys(const Json& value, std::initializer_list<const char*> keys)
{
    if (!value.is_object() || value.size() != keys.size()) return false;
    for (const char* key : keys)
        if (!value.contains(key)) return false;
    return true;
}

bool uuid(const std::string& value)
{
    static const std::regex pattern(
        R"(^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$)");
    return std::regex_match(value, pattern);
}

bool token(const std::string& value)
{
    static const std::regex pattern(R"(^[A-Za-z0-9_-]{32,256}$)");
    return std::regex_match(value, pattern);
}

bool leap(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    return days[month - 1] + (month == 2 && leap(year) ? 1 : 0);
}

std::int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = unsigned(year - era * 400);
    const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
    const unsigned doy = (153 * adjusted_month + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return std::int64_t(era) * 146097 + std::int64_t(doe) - 719468;
}

std::int64_t parse_time(const std::string& value)
{
    static const std::regex pattern(
        R"(^([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})(?:\.[0-9]{1,9})?Z$)");
    std::smatch match;
    if (!std::regex_match(value, match, pattern)) protocol("平台会话有效期格式无效");
    const int year = std::stoi(match[1].str());
    const int month = std::stoi(match[2].str());
    const int day = std::stoi(match[3].str());
    const int hour = std::stoi(match[4].str());
    const int minute = std::stoi(match[5].str());
    const int second = std::stoi(match[6].str());
    if (year < 1970 || year > 9999 || day < 1 || day > days_in_month(year, month) ||
        hour > 23 || minute > 59 || second > 59)
        protocol("平台会话有效期格式无效");
    return days_from_civil(year, unsigned(month), unsigned(day)) * 86400 +
           hour * 3600 + minute * 60 + second;
}

Identity parse_account(const Json& account)
{
    if (!exact_keys(account, {"id", "email", "displayName", "avatarUrl",
                              "displayNameCustomized", "avatarCustomized", "status", "roles"}))
        protocol("平台账号响应不完整，请重新登录");
    if (!account["id"].is_string() || !account["displayName"].is_string() ||
        !(account["email"].is_null() || account["email"].is_string()) ||
        !(account["avatarUrl"].is_null() || account["avatarUrl"].is_string()) ||
        !account["displayNameCustomized"].is_boolean() ||
        !account["avatarCustomized"].is_boolean() || !account["status"].is_string() ||
        !account["roles"].is_array())
        protocol("平台账号响应不完整，请重新登录");
    std::vector<std::string> roles;
    std::set<std::string> unique_roles;
    for (const auto& role : account["roles"]) {
        if (!role.is_string()) protocol("平台账号响应不完整，请重新登录");
        const auto value = role.get<std::string>();
        if ((value != "user" && value != "admin" && value != "operator") ||
            !unique_roles.insert(value).second)
            protocol("平台账号响应无效，请重新登录");
        roles.push_back(value);
    }
    const auto subject = account["id"].get<std::string>();
    const auto email = account["email"].is_string()
                           ? std::optional<std::string>(account["email"].get<std::string>())
                           : std::nullopt;
    const auto name = account["displayName"].get<std::string>();
    const auto picture = account["avatarUrl"].is_string()
                             ? std::optional<std::string>(account["avatarUrl"].get<std::string>())
                             : std::nullopt;
    const auto status = account["status"].get<std::string>();
    const auto bounded_text = [](const std::optional<std::string>& value, size_t limit) {
        return !value || (value->size() <= limit &&
                          std::none_of(value->begin(), value->end(), [](unsigned char c) {
                              return c < 0x20 || c == 0x7f;
                          }));
    };
    const bool valid_name = !name.empty() && name.size() <= 512 &&
                            std::none_of(name.begin(), name.end(), [](unsigned char c) {
                                return c < 0x20 || c == 0x7f;
                            });
    if (!uuid(subject) || !valid_name ||
        !bounded_text(email, 320) || !bounded_text(picture, 65536) ||
        status != "active" || roles.empty())
        protocol("平台账号响应无效，请重新登录");
    Identity identity;
    identity.subject = subject;
    identity.email = email;
    identity.name = name;
    identity.picture = picture;
    identity.display_name_customized = account["displayNameCustomized"].get<bool>();
    identity.avatar_customized = account["avatarCustomized"].get<bool>();
    identity.status = status;
    identity.roles = std::move(roles);
    return identity;
}

} // namespace

std::int64_t now_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t parse_timestamp(const std::string& value)
{
    return parse_time(value);
}

std::string digest(const std::string& value)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(SHA256_DIGEST_LENGTH * 2);
    for (unsigned char c : hash) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}

std::string random_value()
{
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) protocol("无法生成安全授权请求");
    return digest(std::string(reinterpret_cast<char*>(bytes), sizeof(bytes)));
}

std::string pkce_verifier()
{
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) protocol("无法生成安全授权请求");
    return base64url(bytes, sizeof(bytes));
}

std::string pkce_challenge(const std::string& verifier)
{
    static const std::regex pattern(R"(^[A-Za-z0-9_-]{43,128}$)");
    if (!std::regex_match(verifier, pattern)) protocol("授权校验参数无效");
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(), hash);
    return base64url(hash, sizeof(hash));
}

std::string encode(const std::string& value)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string Config::validate() const
{
    if (api_url.empty()) return "平台账号服务尚未配置，请联系软件提供方";
    try {
        const auto parsed = uri(api_url);
        if (parsed.scheme != "https" || !valid_authority(parsed.authority) || !parsed.path.empty() ||
            !parsed.query.empty() || !parsed.fragment.empty())
            return "平台账号服务地址必须是 HTTPS origin";
    } catch (...) {
        return "平台账号服务地址格式无效";
    }
    if (callback_uri != "honorforge://auth/callback")
        return "平台账号回调地址无效";
    if (!browser_profile_key.empty() &&
        !std::regex_match(browser_profile_key, std::regex("^[0-9a-f]{64}$")))
        return "账号浏览器配置标识无效";
    return {};
}

std::string Config::key() const
{
    return digest("orca-platform-session-v2\n" + api_url + "\n" + callback_uri);
}

std::string Config::browser_key() const
{
    return browser_profile_key.empty() ? key() : browser_profile_key;
}

bool is_callback(const std::string& url, const std::string& callback)
{
    try {
        const auto actual = uri(url);
        const auto expected = uri(callback);
        return actual.scheme == expected.scheme && actual.authority == expected.authority &&
               actual.path == expected.path;
    } catch (...) {
        return false;
    }
}

std::string callback_code(const std::string& url, const std::string& callback)
{
    if (callback != "honorforge://auth/callback" || !is_callback(url, callback))
        protocol("授权回调地址不匹配");
    const auto actual = uri(url);
    if (!actual.fragment.empty()) protocol("授权回调格式无效");
    const auto params = query(actual.query);
    if (params.size() != 1 || (!params.count("code") && !params.count("error")))
        protocol("授权回调参数无效");
    if (params.count("error")) {
        const auto& error = params.at("error");
        static const std::regex safe(R"(^[A-Z][A-Z0-9_]{1,79}$)");
        if (!std::regex_match(error, safe)) protocol("授权回调参数无效");
        throw Error(Failure::Cancelled,
                    error == "HONOR_ACCESS_DENIED" ? "授权未完成，可重新登录"
                                                   : "荣耀授权未完成，请重试");
    }
    const auto& code = params.at("code");
    if (!token(code))
        protocol("授权回调缺少有效平台码");
    return code;
}

bool valid_authorization_url(const std::string& value)
{
    try {
        const auto parsed = uri(value);
        return parsed.scheme == "https" && valid_authority(parsed.authority) &&
               parsed.fragment.empty();
    } catch (...) {
        return false;
    }
}

AuthorizationPage classify_authorization_page(const std::string& value,
                                                const std::string& callback)
{
    if (value == "about:blank") return AuthorizationPage::Blank;
    if (is_callback(value, callback)) return AuthorizationPage::Callback;
    try {
        const auto parsed = uri(value);
        if (parsed.scheme != "https" || !valid_authority(parsed.authority))
            return AuthorizationPage::Blocked;
        const auto authority_is = [&parsed](const char* host) {
            return parsed.authority == host || parsed.authority == std::string(host) + ":443";
        };
        if (authority_is("hnid-test-drcn.cloud.honor.com") &&
            parsed.path.rfind("/CAS/portal/", 0) == 0)
            return AuthorizationPage::CasPortal;
        if ((authority_is("hnoauth-login-test-drcn.cloud.honor.com") ||
             authority_is("hnoauth-login.cloud.honor.com"))) {
            if (parsed.path == "/oauth2/v3/login") return AuthorizationPage::OAuthLogin;
            if (parsed.path == "/oauth2/v3/authorize") return AuthorizationPage::OAuthAuthorize;
        }
        return AuthorizationPage::OtherHttps;
    } catch (const Error&) {
        return AuthorizationPage::Blocked;
    }
}

const char* authorization_page_name(AuthorizationPage page) noexcept
{
    switch (page) {
    case AuthorizationPage::Blank: return "blank";
    case AuthorizationPage::CasPortal: return "cas_portal";
    case AuthorizationPage::OAuthLogin: return "oauth_login";
    case AuthorizationPage::OAuthAuthorize: return "oauth_authorize";
    case AuthorizationPage::Callback: return "callback";
    case AuthorizationPage::OtherHttps: return "other_https";
    case AuthorizationPage::Blocked: return "blocked";
    }
    return "blocked";
}

std::string serialize_config(const Config& config)
{
    return Json{{"version", 2},
                {"api_url", config.api_url},
                {"callback_uri", config.callback_uri},
                {"browser_profile_key", config.browser_profile_key}}
        .dump();
}

Config parse_config(const std::string& value)
{
    try {
        const auto json = Json::parse(value);
        if (!exact_keys(json, {"version", "api_url", "callback_uri", "browser_profile_key"}) ||
            !json["version"].is_number_integer() || json["version"] != 2 ||
            !json["api_url"].is_string() || !json["callback_uri"].is_string() ||
            !json["browser_profile_key"].is_string())
            protocol("平台账号配置无效");
        Config config;
        config.api_url = json["api_url"].get<std::string>();
        config.callback_uri = json["callback_uri"].get<std::string>();
        config.browser_profile_key = json["browser_profile_key"].get<std::string>();
        if (!config.api_url.empty() && !config.validate().empty()) protocol("平台账号配置无效");
        if (config.api_url.empty() && config.callback_uri != "honorforge://auth/callback")
            protocol("平台账号配置无效");
        if (!config.browser_profile_key.empty() &&
            !std::regex_match(config.browser_profile_key, std::regex("^[0-9a-f]{64}$")))
            protocol("平台账号配置无效");
        return config;
    } catch (const Error&) {
        throw;
    } catch (...) {
        protocol("平台账号配置无效");
    }
}

std::optional<std::string> legacy_profile_key(const std::string& value)
{
    try {
        const auto json = Json::parse(value);
        const std::vector<const char*> fields = {"environment", "client_id", "client_secret",
                                                  "redirect_uri", "authorize_url", "token_url",
                                                  "jwks_url", "issuer"};
        if (!json.is_object()) return {};
        for (const char* field : fields)
            if (!json.contains(field) || !json[field].is_string()) return {};
        return digest(json["environment"].get<std::string>() + "\n" +
                      json["client_id"].get<std::string>() + "\n" +
                      json["redirect_uri"].get<std::string>() + "\n" +
                      json["authorize_url"].get<std::string>() + "\n" +
                      json["token_url"].get<std::string>() + "\n" +
                      json["jwks_url"].get<std::string>() + "\n" +
                      json["issuer"].get<std::string>());
    } catch (...) {
        return {};
    }
}

std::string serialize_session(const PlatformSession& session)
{
    Json account{{"id", session.identity.subject},
                 {"email", session.identity.email ? Json(*session.identity.email) : Json(nullptr)},
                 {"displayName", session.identity.name},
                 {"avatarUrl", session.identity.picture ? Json(*session.identity.picture) : Json(nullptr)},
                 {"displayNameCustomized", session.identity.display_name_customized},
                 {"avatarCustomized", session.identity.avatar_customized},
                 {"status", session.identity.status},
                 {"roles", session.identity.roles}};
    return Json{{"version", 2},
                {"kind", "mobile"},
                {"session_id", session.session_id},
                {"access_token", session.access_token},
                {"refresh_token", session.refresh_token},
                {"access_expires_at", session.access_expires_at},
                {"refresh_expires_at", session.refresh_expires_at},
                {"account", std::move(account)}}
        .dump();
}

PlatformSession parse_session(const std::string& value)
{
    try {
        const auto json = Json::parse(value);
        if (!exact_keys(json, {"version", "kind", "session_id", "access_token", "refresh_token",
                               "access_expires_at", "refresh_expires_at", "account"}) ||
            !json["version"].is_number_integer() || json["version"] != 2 ||
            !json["kind"].is_string() || json["kind"] != "mobile" ||
            !json["session_id"].is_string() || !json["access_token"].is_string() ||
            !json["refresh_token"].is_string() || !json["access_expires_at"].is_number_integer() ||
            !json["refresh_expires_at"].is_number_integer())
            protocol("本机平台会话不完整，请重新登录");
        PlatformSession session;
        session.session_id = json["session_id"].get<std::string>();
        session.access_token = json["access_token"].get<std::string>();
        session.refresh_token = json["refresh_token"].get<std::string>();
        session.access_expires_at = json["access_expires_at"].get<std::int64_t>();
        session.refresh_expires_at = json["refresh_expires_at"].get<std::int64_t>();
        session.identity = parse_account(json["account"]);
        if (!uuid(session.session_id) || !uuid(session.identity.subject) ||
            !token(session.access_token) || !token(session.refresh_token) ||
            session.access_expires_at <= 0 ||
            session.refresh_expires_at <= session.access_expires_at)
            protocol("本机平台会话不完整，请重新登录");
        return session;
    } catch (const Error&) {
        throw;
    } catch (...) {
        protocol("本机平台会话不完整，请重新登录");
    }
}

std::string serialize_refresh_tombstone()
{
    return Json{{"version", 2}, {"refresh_pending", true}}.dump();
}

bool is_refresh_tombstone(const std::string& value)
{
    const auto json = Json::parse(value, nullptr, false);
    return exact_keys(json, {"version", "refresh_pending"}) &&
           json["version"].is_number_integer() && json["version"] == 2 &&
           json["refresh_pending"].is_boolean() && json["refresh_pending"] == true;
}

PlatformSession parse_session_response(const std::string& value)
{
    try {
        const auto json = Json::parse(value);
        if (!exact_keys(json, {"account", "sessionId", "kind", "accessToken", "refreshToken",
                               "accessExpiresAt", "refreshExpiresAt"}) ||
            !json["sessionId"].is_string() || !json["kind"].is_string() ||
            json["kind"] != "mobile" || !json["accessToken"].is_string() ||
            !json["refreshToken"].is_string() || !json["accessExpiresAt"].is_string() ||
            !json["refreshExpiresAt"].is_string())
            protocol("平台会话响应不完整，请重新登录");
        PlatformSession session;
        session.session_id = json["sessionId"].get<std::string>();
        session.access_token = json["accessToken"].get<std::string>();
        session.refresh_token = json["refreshToken"].get<std::string>();
        session.access_expires_at = parse_time(json["accessExpiresAt"].get<std::string>());
        session.refresh_expires_at = parse_time(json["refreshExpiresAt"].get<std::string>());
        session.identity = parse_account(json["account"]);
        if (!uuid(session.session_id) || !token(session.access_token) ||
            !token(session.refresh_token) || session.access_expires_at <= now_seconds() ||
            session.refresh_expires_at <= session.access_expires_at)
            protocol("平台会话响应无效，请重新登录");
        return session;
    } catch (const Error&) {
        throw;
    } catch (...) {
        protocol("平台会话响应不完整，请重新登录");
    }
}

Identity parse_account_response(const std::string& value)
{
    try {
        return parse_account(Json::parse(value));
    } catch (const Error&) {
        throw;
    } catch (...) {
        protocol("平台账号响应不完整，请重新登录");
    }
}

} // namespace Slic3r::HonorAccount
