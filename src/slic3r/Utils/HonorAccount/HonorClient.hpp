#pragma once
#include "HonorOAuth.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace Slic3r::HonorAccount {
using Cancellation = std::shared_ptr<std::atomic_bool>;
struct Response { unsigned status {0}; std::string body; };
using Transport = std::function<Response(const std::string& url, const std::string& form, Cancellation)>;
Response https_request(const std::string&, const std::string& form, Cancellation);

class Client {
public:
    struct KeyCache { std::mutex mutex; std::string url, json; std::int64_t fetched {0}; };
    explicit Client(Transport transport = https_request, std::shared_ptr<KeyCache> cache = std::make_shared<KeyCache>())
        : m_transport(std::move(transport)), m_cache(std::move(cache)) {}
    Credentials exchange(const Config&, const std::string& code, const std::string& nonce, Cancellation);
    Credentials restore(const Config&, const Credentials&, Cancellation);
private:
    Credentials tokens(const Config&, const std::string& form, const std::string& nonce,
                       const Credentials* previous, Cancellation);
    std::string keys(const Config&, Cancellation, bool force = false);
    Transport m_transport;
    std::shared_ptr<KeyCache> m_cache;
};
} // namespace Slic3r::HonorAccount
