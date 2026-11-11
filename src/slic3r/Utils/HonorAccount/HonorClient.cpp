#include "HonorClient.hpp"
#include "../Http.hpp"

#include <algorithm>
#include <memory>
#include <regex>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace Slic3r::HonorAccount {
namespace {

using Json = nlohmann::json;

struct Transfer {
    std::string body;
    Cancellation cancel;
};

size_t receive(char* data, size_t size, size_t count, void* context)
{
    auto& transfer = *static_cast<Transfer*>(context);
    constexpr size_t limit = 1024 * 1024;
    if (transfer.cancel->load() || (count != 0 && size > limit / count)) return 0;
    const size_t bytes = size * count;
    if (transfer.body.size() > limit || bytes > limit - transfer.body.size()) return 0;
    transfer.body.append(data, bytes);
    return bytes;
}

int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<Transfer*>(context)->cancel->load() ? 1 : 0;
}

std::string error_code(const std::string& body)
{
    const auto json = Json::parse(body, nullptr, false);
    if (!json.is_object() || !json.contains("error") || !json["error"].is_object() ||
        !json["error"].contains("code") || !json["error"]["code"].is_string())
        return {};
    const auto code = json["error"]["code"].get<std::string>();
    static const std::regex safe(R"(^[A-Z][A-Z0-9_]{1,79}$)");
    return std::regex_match(code, safe) ? code : std::string();
}

[[noreturn]] void response_error(unsigned status, const std::string& body)
{
    const auto code = error_code(body);
    if (code == "HONOR_NOT_CONFIGURED" || code == "HONOR_REAUTH_UNAVAILABLE")
        throw Error(Failure::Configuration, "平台荣耀账号服务尚未配置");
    if (code == "SESSION_REVOKED" || code == "ACCOUNT_DISABLED" ||
        code == "EXCHANGE_CODE_INVALID" || status == 401)
        throw Error(Failure::InvalidCredentials, "平台登录已失效，请重新登录");
    if (code == "HONOR_ACCESS_DENIED")
        throw Error(Failure::Cancelled, "授权未完成，可重新登录");
    if (status == 408 || status == 429 || status >= 500)
        throw Error(Failure::Temporary, "平台账号服务暂时无法连接，请稍后重试");
    if (code == "MOBILE_ATTEMPT_INVALID")
        throw Error(Failure::Protocol, "登录请求已过期，请重新登录");
    if (code == "MOBILE_UPGRADE_REQUIRED")
        throw Error(Failure::Protocol, "平台浏览器登录暂不可用，请联系软件提供方");
    if (code == "HONOR_IDENTITY_CONFLICT")
        throw Error(Failure::Protocol, "荣耀身份与平台账号冲突，请联系支持人员");
    throw Error(Failure::Protocol,
                "平台账号请求未完成（HTTP " + std::to_string(status) + "）");
}

Json json_response(const Response& response, unsigned expected)
{
    if (response.status != expected) response_error(response.status, response.body);
    auto json = Json::parse(response.body, nullptr, false);
    if (!json.is_object()) throw Error(Failure::Protocol, "平台账号响应格式无效");
    return json;
}

bool exact(const Json& value, std::initializer_list<const char*> keys)
{
    if (!value.is_object() || value.size() != keys.size()) return false;
    for (const char* key : keys)
        if (!value.contains(key)) return false;
    return true;
}

} // namespace

Response https_request(const Request& request, Cancellation cancel)
{
    Http::tls_global_init();
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw Error(Failure::Temporary, "无法创建平台账号网络请求");

    Transfer transfer{{}, cancel};
    curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef _WIN32
    curl_easy_setopt(curl.get(), CURLOPT_SSL_OPTIONS, long(CURLSSLOPT_NATIVE_CA));
#endif
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);

    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Accept: application/json");
    if (request.method == Request::Method::Post) {
        raw_headers = curl_slist_append(raw_headers, "Content-Type: application/json");
        curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.json.data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, long(request.json.size()));
    }
    if (!request.bearer.empty()) {
        const std::string authorization = "Authorization: Bearer " + request.bearer;
        raw_headers = curl_slist_append(raw_headers, authorization.c_str());
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers,
                                                                        curl_slist_free_all);
    if (headers) curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());

    const auto result = curl_easy_perform(curl.get());
    if (cancel->load()) throw Error(Failure::Cancelled, "登录已取消");
    if (result != CURLE_OK)
        throw Error(Failure::Temporary, "平台账号服务暂时无法连接，请检查网络后重试");
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    return {unsigned(status), std::move(transfer.body)};
}

Response Client::request(const Config& config, Request::Method method, const std::string& path,
                         const std::string& json, const std::string& bearer,
                         Cancellation cancel)
{
    if (const auto error = config.validate(); !error.empty())
        throw Error(Failure::Configuration, error);
    if (path.rfind("/v1/", 0) != 0 || path.find_first_of("\r\n") != std::string::npos)
        throw Error(Failure::Protocol, "平台账号请求路径无效");
    if (cancel->load()) throw Error(Failure::Cancelled, "登录已取消");
    auto response = m_transport({method, config.api_url + path, json, bearer}, cancel);
    if (cancel->load()) throw Error(Failure::Cancelled, "登录已取消");
    return response;
}

Authorization Client::authorize(const Config& config, const std::string& verifier,
                                Cancellation cancel)
{
    const auto challenge = pkce_challenge(verifier);
    const auto attempt_response = request(
        config, Request::Method::Post, "/v1/identity/honor/mobile/attempts",
        Json{{"platform", "ios"}, {"challenge", challenge}}.dump(), {}, cancel);
    const auto attempt = json_response(attempt_response, 201);
    if (!exact(attempt, {"attemptId", "expiresAt", "sdkAllowed"}) ||
        !attempt["attemptId"].is_string() || !attempt["expiresAt"].is_string() ||
        !attempt["sdkAllowed"].is_boolean() || attempt["sdkAllowed"] != false)
        throw Error(Failure::Protocol, "平台登录请求响应无效");
    const auto attempt_id = attempt["attemptId"].get<std::string>();
    static const std::regex id_pattern(R"(^[A-Za-z0-9_-]{43}$)");
    const auto expires_at = parse_timestamp(attempt["expiresAt"].get<std::string>());
    if (!std::regex_match(attempt_id, id_pattern) || expires_at <= now_seconds() ||
        expires_at > now_seconds() + 15 * 60)
        throw Error(Failure::Protocol, "平台登录请求响应无效");

    const auto authorize_response = request(
        config, Request::Method::Get,
        "/v1/identity/honor/mobile/authorize?attemptId=" + encode(attempt_id), {}, {}, cancel);
    const auto authorize = json_response(authorize_response, 200);
    if (!exact(authorize, {"authorizationUrl"}) || !authorize["authorizationUrl"].is_string())
        throw Error(Failure::Protocol, "平台授权地址响应无效");
    const auto url = authorize["authorizationUrl"].get<std::string>();
    if (!valid_authorization_url(url))
        throw Error(Failure::Protocol, "平台返回的授权地址无效");
    return {url, expires_at};
}

PlatformSession Client::exchange(const Config& config, const std::string& code,
                                 const std::string& verifier, Cancellation cancel)
{
    const auto response = request(config, Request::Method::Post, "/v1/identity/exchange",
                                  Json{{"code", code}, {"verifier", verifier}, {"kind", "mobile"}}.dump(),
                                  {}, cancel);
    if (response.status != 201) response_error(response.status, response.body);
    return parse_session_response(response.body);
}

PlatformSession Client::validate(const Config& config, const PlatformSession& previous,
                                 Cancellation cancel)
{
    const auto response = request(config, Request::Method::Get, "/v1/me", {},
                                  previous.access_token, cancel);
    if (response.status != 200) response_error(response.status, response.body);
    auto result = previous;
    result.identity = parse_account_response(response.body);
    if (result.identity.subject != previous.identity.subject)
        throw Error(Failure::InvalidCredentials,
                    "平台账号与本机保存的身份不一致，请重新登录");
    return result;
}

PlatformSession Client::refresh(const Config& config, const PlatformSession& previous,
                                Cancellation cancel)
{
    const auto response = request(
        config, Request::Method::Post, "/v1/identity/session/refresh",
        Json{{"refreshToken", previous.refresh_token}}.dump(), {}, cancel);
    if (response.status != 201) response_error(response.status, response.body);
    auto result = parse_session_response(response.body);
    if (result.identity.subject != previous.identity.subject)
        throw Error(Failure::InvalidCredentials,
                    "刷新的平台账号与本机身份不一致，请重新登录");
    return result;
}

void Client::revoke(const Config& config, const std::string& refresh_token,
                    Cancellation cancel)
{
    const auto response = request(config, Request::Method::Post,
                                  "/v1/identity/session/revoke",
                                  Json{{"refreshToken", refresh_token}}.dump(), {}, cancel);
    const auto json = json_response(response, 201);
    if (!exact(json, {"ok"}) || !json["ok"].is_boolean() || json["ok"] != true)
        throw Error(Failure::Protocol, "平台会话撤销响应无效");
}

} // namespace Slic3r::HonorAccount
