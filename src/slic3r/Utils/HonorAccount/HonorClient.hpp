#pragma once

#include "HonorOAuth.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::HonorAccount {

using Cancellation = std::shared_ptr<std::atomic_bool>;
using Headers = std::vector<std::pair<std::string, std::string>>;

struct SessionAccess {
    std::string api_url, access_token, subject;
    std::uint64_t generation {0};
    Cancellation lifetime;
};

struct Request {
    enum class Method { Get, Post, Put, Delete, Patch };
    Method method {Method::Get};
    std::string url;
    std::string json;
    std::string bearer;
    Headers headers;
    size_t response_limit {1024 * 1024};
    std::string accept {"application/json"};
    std::shared_ptr<const std::string> upload;
    Cancellation account_lifetime;
};

struct Response {
    unsigned status {0};
    std::string body;
};

using Transport = std::function<Response(const Request&, Cancellation)>;
Response https_request(const Request&, Cancellation);
const char* desktop_platform();
const Headers& desktop_client_headers();
[[noreturn]] void response_error(unsigned status, const std::string& body);

class Client {
public:
    explicit Client(Transport transport = https_request) : m_transport(std::move(transport)) {}

    Authorization authorize(const Config&, const std::string& verifier, Cancellation);
    PlatformSession exchange(const Config&, const std::string& code,
                             const std::string& verifier, Cancellation);
    PlatformSession validate(const Config&, const PlatformSession&, Cancellation);
    void update_avatar(const Config&, const PlatformSession&, const std::string& avatar_url, Cancellation);
    PlatformSession refresh(const Config&, const PlatformSession&, Cancellation);
    void revoke(const Config&, const std::string& refresh_token, Cancellation);
    Response request(const Config&, Request::Method, const std::string& path,
                     const std::string& json, const std::string& bearer,
                     Cancellation, Cancellation account_lifetime = {},
                     size_t response_limit = 1024 * 1024, std::string accept = "application/json");
private:
    Transport m_transport;
};

} // namespace Slic3r::HonorAccount
