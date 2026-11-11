#pragma once

#include "HonorOAuth.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace Slic3r::HonorAccount {

using Cancellation = std::shared_ptr<std::atomic_bool>;

struct Request {
    enum class Method { Get, Post };
    Method method {Method::Get};
    std::string url;
    std::string json;
    std::string bearer;
};

struct Response {
    unsigned status {0};
    std::string body;
};

using Transport = std::function<Response(const Request&, Cancellation)>;
Response https_request(const Request&, Cancellation);

class Client {
public:
    explicit Client(Transport transport = https_request) : m_transport(std::move(transport)) {}

    Authorization authorize(const Config&, const std::string& verifier, Cancellation);
    PlatformSession exchange(const Config&, const std::string& code,
                             const std::string& verifier, Cancellation);
    PlatformSession validate(const Config&, const PlatformSession&, Cancellation);
    PlatformSession refresh(const Config&, const PlatformSession&, Cancellation);
    void revoke(const Config&, const std::string& refresh_token, Cancellation);

private:
    Response request(const Config&, Request::Method, const std::string& path,
                     const std::string& json, const std::string& bearer,
                     Cancellation);
    Transport m_transport;
};

} // namespace Slic3r::HonorAccount
