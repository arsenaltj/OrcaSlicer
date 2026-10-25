#pragma once

#include "orca/connectivity/moonraker/MoonrakerAdapter.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <string>

namespace Orca::MobileDevice {

struct MoonrakerTransportOptions {
    std::chrono::milliseconds connect_timeout{std::chrono::seconds(10)};
    std::chrono::milliseconds total_timeout{std::chrono::seconds(30)};
    // Zero means an open-ended read. A WebSocket may legitimately be quiet
    // for longer than the request timeout; callers can opt into an idle
    // timeout when the printer policy requires one.
    std::chrono::milliseconds read_idle_timeout{0};
    bool verify_tls{true};
    std::string ca_file;
    std::string api_key;
};

class MoonrakerHttpTransport final : public IMoonrakerHttpTransport {
public:
    explicit MoonrakerHttpTransport(MoonrakerTransportOptions options = {});
    ~MoonrakerHttpTransport() override;

    MoonrakerHttpResponse perform(const MoonrakerHttpRequest& request) override;
    void cancel() override;

    std::string last_error() const;

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

class MoonrakerWebSocketTransport final : public IMoonrakerWebSocketTransport {
public:
    explicit MoonrakerWebSocketTransport(MoonrakerTransportOptions options = {});
    ~MoonrakerWebSocketTransport() override;

    bool connect(const std::string& url,
                 const std::map<std::string, std::string>& headers,
                 MessageHandler on_message,
                 CloseHandler on_close) override;
    bool send(const std::string& payload) override;
    void close() override;

    std::string last_error() const;

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace Orca::MobileDevice
