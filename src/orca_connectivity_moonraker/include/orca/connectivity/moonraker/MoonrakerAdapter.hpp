#pragma once

#include "orca/sdk/Connectivity.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace Orca::SDK {

struct MoonrakerHttpRequest {
    std::string method;
    std::string url;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
    std::map<std::string, std::string> form_fields;
    std::string upload_source;
    std::string upload_filename;
    std::string upload_sha256;
};

struct MoonrakerHttpResponse {
    int status_code{0};
    std::string body;
    std::string error;
};

class IMoonrakerHttpTransport {
public:
    virtual ~IMoonrakerHttpTransport() = default;
    virtual MoonrakerHttpResponse perform(const MoonrakerHttpRequest& request) = 0;
    virtual void cancel() {}
};

class IMoonrakerWebSocketTransport {
public:
    using MessageHandler = std::function<void(const std::string&)>;
    using CloseHandler = std::function<void()>;

    virtual ~IMoonrakerWebSocketTransport() = default;
    virtual bool connect(const std::string& url,
                         const std::map<std::string, std::string>& headers,
                         MessageHandler on_message,
                         CloseHandler on_close) = 0;
    virtual bool send(const std::string& payload) = 0;
    virtual void close() = 0;
};

struct MoonrakerServerInfo {
    std::string klippy_state;
    std::string moonraker_version;
    std::string klipper_path;
    std::string hostname;
};

class MoonrakerAdapter final : public IPrinterProtocolAdapter {
public:
    MoonrakerAdapter(std::shared_ptr<IMoonrakerHttpTransport> http,
                     std::shared_ptr<IMoonrakerWebSocketTransport> websocket = nullptr,
                     std::string api_key = {});
    ~MoonrakerAdapter() override;

    const char* id() const override;
    bool probe(const DeviceEndpoint& endpoint) override;
    PrinterCapability capabilities() const override;
    PrinterStatus get_status() override;
    PrinterOperationResult upload_gcode(const GcodeUploadRequest& request) override;
    PrinterOperationResult start_print(const PrintCommand& command) override;
    PrinterOperationResult pause() override;
    PrinterOperationResult resume() override;
    PrinterOperationResult cancel() override;

    bool refresh_objects();
    bool subscribe_status();
    bool handle_websocket_message(const std::string& payload);
    bool reconnect_and_reconcile();
    bool poll_status();

    bool websocket_connected() const;
    bool polling_fallback() const;
    bool connected() const;
    std::set<std::string> available_objects() const;
    MoonrakerServerInfo server_info() const;
    PrinterOperationResult last_result() const;
    std::string last_uploaded_path() const;

private:
    MoonrakerAdapter(const MoonrakerAdapter&) = delete;
    MoonrakerAdapter& operator=(const MoonrakerAdapter&) = delete;

    struct CallbackGuard;
    struct ConnectionIdentity;

    static std::string endpoint_url(const DeviceEndpoint& endpoint, const std::string& path);
    static std::string websocket_url(const DeviceEndpoint& endpoint);
    static std::string endpoint_device_id(const DeviceEndpoint& endpoint);
    static std::string map_state(const std::string& state);
    static PrinterState map_printer_state(const std::string& state);
    static std::string error_message(const MoonrakerHttpResponse& response);
    static PrinterOperationResult success_result();
    static PrinterOperationResult failure_result(std::string code, std::string message, bool retryable);

    void reset_probe_state(const DeviceEndpoint& endpoint);
    bool validate_probe_prerequisites();
    bool ensure_ready();
    bool ensure_objects_loaded();
    bool enable_polling_fallback(std::string code, std::string message, bool retryable);

    MoonrakerHttpRequest make_request(std::string method, std::string path) const;
    MoonrakerHttpResponse perform(std::string method,
                                  std::string path,
                                  std::string body = {},
                                  std::map<std::string, std::string> form_fields = {},
                                  std::string upload_source = {},
                                  std::string upload_filename = {},
                                  std::string upload_sha256 = {}) const;
    PrinterOperationResult result_from_response(const MoonrakerHttpResponse& response,
                                                bool parse_json_envelope = true) const;
    bool parse_server_info(const MoonrakerHttpResponse& response);
    bool parse_object_list(const MoonrakerHttpResponse& response);
    bool parse_status(const MoonrakerHttpResponse& response);
    bool handle_websocket_message_for_generation(const std::string& payload,
                                                 std::uint64_t generation,
                                                 const std::shared_ptr<ConnectionIdentity>& identity = {});
    bool apply_status_update(const nlohmann::json& status);
    bool send_gcode(const std::string& script);
    bool send_print_start(const std::string& filename);
    bool set_failure(std::string code, std::string message, bool retryable);

    mutable std::recursive_mutex m_mutex;
    std::shared_ptr<CallbackGuard> m_callback_guard;
    std::shared_ptr<IMoonrakerHttpTransport> m_http;
    std::shared_ptr<IMoonrakerWebSocketTransport> m_websocket;
    DeviceEndpoint m_endpoint;
    std::string m_api_key;
    MoonrakerServerInfo m_server_info;
    PrinterStatus m_status;
    std::set<std::string> m_available_objects;
    std::string m_last_uploaded_path;
    PrinterOperationResult m_last_result{success_result()};
    std::uint64_t m_next_rpc_id{1};
    std::uint64_t m_websocket_generation{0};
    std::shared_ptr<ConnectionIdentity> m_websocket_connection_identity;
    bool m_has_endpoint{false};
    bool m_connected{false};
    bool m_websocket_connected{false};
    bool m_polling_fallback{false};
    bool m_objects_loaded{false};
};

} // namespace Orca::SDK
