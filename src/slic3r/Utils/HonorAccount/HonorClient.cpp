#include "HonorClient.hpp"
#include "../Http.hpp"
#include "libslic3r_version.h"

#include <algorithm>
#include <memory>
#include <regex>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <wx/utils.h>
#ifdef _WIN32
#include <wx/msw/wrapwin.h>
#endif

namespace Slic3r::HonorAccount {
namespace {

using Json = nlohmann::json;

struct Transfer {
    std::string body;
    Cancellation cancel;
    Cancellation account_lifetime;
    size_t limit;

    bool cancelled() const
    {
        return (cancel && cancel->load()) || (account_lifetime && account_lifetime->load());
    }
};

size_t receive(char* data, size_t size, size_t count, void* context)
{
    auto& transfer = *static_cast<Transfer*>(context);
    if (transfer.cancelled() || (count != 0 && size > transfer.limit / count)) return 0;
    const size_t bytes = size * count;
    if (bytes > transfer.limit - transfer.body.size()) return 0;
    try { transfer.body.append(data, bytes); } catch (...) { return 0; }
    return bytes;
}

int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<Transfer*>(context)->cancelled() ? 1 : 0;
}

std::string error_code(const std::string& body)
{
    if (body.size() > 65536) return {};
    const auto json = Json::parse(body, nullptr, false);
    if (!json.is_object() || !json.contains("error") || !json["error"].is_object() ||
        !json["error"].contains("code") || !json["error"]["code"].is_string())
        return {};
    const auto code = json["error"]["code"].get<std::string>();
    static const std::regex safe(R"(^[A-Z][A-Z0-9_]{1,79}$)");
    return std::regex_match(code, safe) ? code : std::string();
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

const char* desktop_platform()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

const Headers& desktop_client_headers()
{
    static const Headers headers = [] {
        int major = 0;
#ifdef _WIN32
        using GetVersion = LONG (WINAPI*)(OSVERSIONINFOW*);
        const auto get_version = reinterpret_cast<GetVersion>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (get_version && get_version(&version) == 0)
            major = version.dwMajorVersion == 10 && version.dwBuildNumber >= 22000 ? 11 : int(version.dwMajorVersion);
#else
        int minor = 0, micro = 0;
        wxGetOsVersion(&major, &minor, &micro);
#endif
        return Headers{{"X-Orca-Client", "desktop"}, {"X-Orca-Platform", desktop_platform()},
                       {"X-Orca-OS-Major-Version", major > 0 ? std::to_string(major) : "unknown"},
                       {"X-Orca-App-Version", HONOR_PRODUCT_VERSION}, {"X-Orca-App-Build", HONOR_PRODUCT_BUILD}};
    }();
    return headers;
}

void response_error(unsigned status, const std::string& body)
{
    const auto code = error_code(body);
    if (code == "HONOR_NOT_CONFIGURED" || code == "HONOR_REAUTH_UNAVAILABLE")
        throw Error(Failure::Configuration, "平台荣耀账号服务尚未配置", status, code);
    if (code == "SESSION_REVOKED" || code == "ACCOUNT_DISABLED" || status == 401)
        throw Error(Failure::InvalidCredentials, "平台登录已失效，请重新登录", status, code);
    if (code == "HONOR_ACCESS_DENIED")
        throw Error(Failure::Cancelled, "授权未完成，可重新登录", status, code);
    if (status == 408 || status == 429 || status >= 500)
        throw Error(Failure::Temporary, "平台账号服务暂时无法连接，请稍后重试", status, code);
    if (code == "HONOR_IDENTITY_CONFLICT")
        throw Error(Failure::Protocol, "荣耀身份与平台账号冲突，请联系支持人员", status, code);
    throw Error(Failure::Protocol, "平台请求未完成（HTTP " + std::to_string(status) + "）", status, code);
}

Response https_request(const Request& request, Cancellation cancel)
{
    Transfer transfer{{}, cancel, request.account_lifetime, request.response_limit};
    if (transfer.cancelled()) throw Error(Failure::Cancelled, "请求已取消");
    Http::tls_global_init();
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw Error(Failure::Temporary, "无法创建平台账号网络请求");

    curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef _WIN32
    curl_easy_setopt(curl.get(), CURLOPT_SSL_OPTIONS, long(CURLSSLOPT_NATIVE_CA));
#endif
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, request.upload || request.response_limit > 4 * 1024 * 1024 ? 180L : 20L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 64L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);

    curl_slist* raw_headers = nullptr;
    const auto append = [&raw_headers](const std::string& header) {
        auto* added = curl_slist_append(raw_headers, header.c_str());
        if (!added) {
            curl_slist_free_all(raw_headers);
            throw Error(Failure::Temporary, "无法创建平台请求头");
        }
        raw_headers = added;
    };
    append("Accept: " + request.accept);
    if (request.method == Request::Method::Post || request.method == Request::Method::Patch) {
        append("Content-Type: application/json");
        if (request.method == Request::Method::Patch)
            curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, "PATCH");
        else
            curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.json.data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, long(request.json.size()));
    } else if (request.method == Request::Method::Put && request.upload) {
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.upload->data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(request.upload->size()));
    } else if (request.method == Request::Method::Delete) {
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    if (!request.bearer.empty()) append("Authorization: Bearer " + request.bearer);
    for (const auto& [name, value] : request.headers) append(name + ": " + value);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers,
                                                                        curl_slist_free_all);
    if (headers) curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());

    const auto result = curl_easy_perform(curl.get());
    if (transfer.cancelled()) throw Error(Failure::Cancelled, "请求已取消");
    if (result != CURLE_OK)
        throw Error(Failure::Temporary, "平台账号服务暂时无法连接，请检查网络后重试");
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    return {unsigned(status), std::move(transfer.body)};
}

Response Client::request(const Config& config, Request::Method method, const std::string& path,
                         const std::string& json, const std::string& bearer,
                         Cancellation cancel, Cancellation account_lifetime,
                         size_t response_limit, std::string accept)
{
    if (const auto error = config.validate(); !error.empty())
        throw Error(Failure::Configuration, error);
    if (path.rfind("/v1/", 0) != 0 || path.find_first_of("\r\n") != std::string::npos)
        throw Error(Failure::Protocol, "平台账号请求路径无效");
    const auto ensure_active = [&] {
        if ((cancel && cancel->load()) || (account_lifetime && account_lifetime->load()))
            throw Error(Failure::Cancelled, "请求已取消");
    };
    ensure_active();
    Request request{method, config.api_url + path, json, bearer};
    request.headers = desktop_client_headers();
    request.response_limit = response_limit;
    request.accept = std::move(accept);
    request.account_lifetime = account_lifetime;
    if (!cancel) cancel = std::make_shared<std::atomic_bool>(false);
    auto response = m_transport(request, cancel);
    ensure_active();
    return response;
}

Authorization Client::authorize(const Config& config, const std::string& verifier,
                                Cancellation cancel)
{
    const auto challenge = pkce_challenge(verifier);
    const auto response = request(config, Request::Method::Post, "/v1/identity/honor/desktop/authorize",
        Json{{"platform", desktop_platform()}, {"challenge", challenge}}.dump(), {}, cancel);
    const auto authorize = json_response(response, 201);
    if (!exact(authorize, {"authorizationUrl", "expiresAt"}) ||
        !authorize["authorizationUrl"].is_string() || !authorize["expiresAt"].is_string())
        throw Error(Failure::Protocol, "平台登录请求响应无效");
    const auto expires_at = parse_timestamp(authorize["expiresAt"].get<std::string>());
    const auto now = now_seconds();
    if (expires_at <= now || expires_at > now + 15 * 60)
        throw Error(Failure::Protocol, "平台登录请求响应无效");
    const auto url = authorize["authorizationUrl"].get<std::string>();
    if (!valid_authorization_url(url))
        throw Error(Failure::Protocol, "平台返回的授权地址无效");
    return {url, expires_at};
}

PlatformSession Client::exchange(const Config& config, const std::string& code,
                                 const std::string& verifier, Cancellation cancel)
{
    const auto response = request(config, Request::Method::Post, "/v1/identity/exchange",
                                  Json{{"code", code}, {"verifier", verifier}, {"kind", "desktop"}}.dump(),
                                  {}, cancel);
    if (response.status == 400 && error_code(response.body) == "EXCHANGE_CODE_INVALID")
        throw Error(Failure::InvalidCredentials, "授权已失效，请重新登录");
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

void Client::update_avatar(const Config& config, const PlatformSession& session,
                           const std::string& avatar_url, Cancellation cancel)
{
    const auto response = request(config, Request::Method::Patch, "/v1/me",
                                  Json{{"avatarUrl", avatar_url}}.dump(), session.access_token, cancel);
    const auto result = json_response(response, 200);
    if (!exact(result, {"ok"}) || !result["ok"].is_boolean() || result["ok"] != true)
        throw Error(Failure::Protocol, "平台未确认头像保存，请重试");
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
