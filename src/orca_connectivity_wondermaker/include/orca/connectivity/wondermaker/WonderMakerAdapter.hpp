#pragma once

#include "orca/sdk/Connectivity.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Orca::SDK::WonderMaker {

// These values describe canonical operations at the adapter boundary. A concrete
// transport owns all wire paths, headers, topics, and authentication details.
enum class HttpResource : uint8_t {
    Pair = 0,
    Authenticate = 1,
    Capabilities = 2,
    Status = 3,
    Control = 4,
    FileTransfer = 5,
};

enum class ControlOperation : uint8_t {
    Start = 0,
    Pause = 1,
    Resume = 2,
    Stop = 3,
};

enum class MonitorState : uint8_t {
    Disconnected = 0,
    Discovering = 1,
    Probing = 2,
    Pairing = 3,
    Authenticating = 4,
    Connecting = 5,
    Syncing = 6,
    Connected = 7,
    Reconnecting = 8,
    Stale = 9,
    Offline = 10,
};

struct DiscoveryCandidate {
    DeviceEndpoint endpoint;
    std::string device_id;
};

struct WonderMakerHttpRequest {
    HttpResource resource{HttpResource::Status};
    DeviceEndpoint endpoint;
    std::string credential;
    std::string certificate_fingerprint;
    std::string command_id;
    ControlOperation control_operation{ControlOperation::Start};
    std::string body;
    std::string source_uri;
    std::string filename;
    std::string sha256;
    uint64_t offset{0};
    uint64_t total_size{0};
};

struct WonderMakerHttpResponse {
    int status_code{0};
    std::string body;
    std::string error;
    bool transport_ok{true};
};

class IWonderMakerHttpTransport {
public:
    virtual ~IWonderMakerHttpTransport() = default;
    virtual WonderMakerHttpResponse request(const WonderMakerHttpRequest& request) = 0;
};

class IWonderMakerWebSocketTransport {
public:
    using MessageHandler = std::function<void(const std::string&)>;
    using CloseHandler = std::function<void()>;

    virtual ~IWonderMakerWebSocketTransport() = default;
    virtual bool connect(const DeviceEndpoint& endpoint,
                         MessageHandler on_message,
                         CloseHandler on_close) = 0;
    virtual bool send(const std::string& payload) = 0;
    virtual void close() = 0;
};

class IWonderMakerDiscoveryTransport {
public:
    virtual ~IWonderMakerDiscoveryTransport() = default;
    virtual std::vector<DiscoveryCandidate> discover() = 0;
};

struct StatusSnapshot {
    PrinterStatus status;
    uint64_t event_sequence{0};
    uint64_t last_synced_at_epoch_seconds{0};
    bool stale{true};
};

struct ControlRequest {
    std::string command_id;
    ControlOperation operation{ControlOperation::Start};
    std::string file_uri;
    std::string filename;
    std::string file_sha256;
};

struct ControlReceipt {
    std::string command_id;
    PrinterOperationResult result;
    bool duplicate{false};
};

struct FileTransferRequest {
    std::string source_uri;
    std::string filename;
    std::string sha256;
    std::string chunk;
    uint64_t offset{0};
    uint64_t total_size{0};
};

struct FileTransferReceipt {
    bool success{false};
    bool complete{false};
    bool digest_verified{false};
    uint64_t next_offset{0};
    std::string code;
    std::string message;
};

class WonderMakerAdapter final : public IPrinterProtocolAdapter {
public:
    using Clock = std::function<uint64_t()>;

    WonderMakerAdapter(std::shared_ptr<IWonderMakerHttpTransport> http,
                       std::shared_ptr<IWonderMakerWebSocketTransport> websocket = nullptr,
                       std::shared_ptr<IWonderMakerDiscoveryTransport> discovery = nullptr,
                       Clock clock = {});
    ~WonderMakerAdapter() override;

    WonderMakerAdapter(const WonderMakerAdapter&) = delete;
    WonderMakerAdapter& operator=(const WonderMakerAdapter&) = delete;

    const char* id() const override;
    bool probe(const DeviceEndpoint& endpoint) override;
    PrinterCapability capabilities() const override;
    PrinterStatus get_status() override;
    PrinterOperationResult upload_gcode(const GcodeUploadRequest& request) override;
    PrinterOperationResult start_print(const PrintCommand& command) override;
    PrinterOperationResult pause() override;
    PrinterOperationResult resume() override;
    PrinterOperationResult cancel() override;

    std::vector<DiscoveryCandidate> discover();
    void set_manual_endpoint(DeviceEndpoint endpoint);
    const std::optional<DeviceEndpoint>& endpoint() const { return m_endpoint; }

    PrinterOperationResult pair(const std::string& opaque_credential);
    PrinterOperationResult authenticate(const std::string& opaque_credential,
                                        const std::string& certificate_fingerprint = {});
    PrinterOperationResult connect();
    void disconnect();
    bool reconnect_and_reconcile();

    bool subscribe_status();
    bool handle_websocket_message(const std::string& payload);
    bool poll_status();
    bool reconcile_status();
    void tick();

    ControlReceipt send_control(const ControlRequest& request);
    FileTransferReceipt upload_file(const FileTransferRequest& request);

    MonitorState monitor_state() const { return m_monitor_state; }
    StatusSnapshot status_snapshot() const { return m_snapshot; }
    const PrinterOperationResult& last_result() const { return m_last_result; }
    bool connected() const { return m_connected; }
    bool websocket_connected() const { return m_websocket_connected; }
    bool polling_fallback() const { return m_polling_fallback; }
    uint64_t status_age_seconds() const;
    uint64_t last_reconciled_at_epoch_seconds() const { return m_last_reconciled_at_epoch_seconds; }

private:
    static uint64_t default_now();
    static const char* control_operation_name(ControlOperation operation);
    static PrinterOperationResult success_result();
    static PrinterOperationResult failure_result(std::string code,
                                                 std::string message,
                                                 bool retryable);
    static bool response_succeeded(const WonderMakerHttpResponse& response);
    static PrinterOperationResult result_from_response(const WonderMakerHttpResponse& response);
    static std::string stable_command_id(const char* operation, const std::string& suffix);

    WonderMakerHttpResponse request(WonderMakerHttpRequest request);
    bool apply_status_body(const std::string& body, bool reconciled);
    bool apply_capabilities_body(const std::string& body);
    void on_websocket_closed();
    void mark_status_received(uint64_t event_sequence, bool reconciled);
    bool ready_for_http() const;
    bool ready_for_control() const;
    ControlReceipt duplicate_receipt(const ControlRequest& request) const;

    std::shared_ptr<IWonderMakerHttpTransport> m_http;
    std::shared_ptr<IWonderMakerWebSocketTransport> m_websocket;
    std::shared_ptr<IWonderMakerDiscoveryTransport> m_discovery;
    Clock m_clock;
    std::optional<DeviceEndpoint> m_endpoint;
    PrinterCapability m_capabilities;
    StatusSnapshot m_snapshot;
    PrinterOperationResult m_last_result;
    MonitorState m_monitor_state{MonitorState::Disconnected};
    bool m_paired{false};
    bool m_authenticated{false};
    bool m_connected{false};
    bool m_websocket_connected{false};
    bool m_polling_fallback{false};
    uint64_t m_last_reconciled_at_epoch_seconds{0};
    uint64_t m_next_poll_at_epoch_seconds{0};
    std::unordered_map<std::string, ControlReceipt> m_command_receipts;
};

// A deterministic in-process transport implementing the same abstract boundary
// used by a real LAN integration. It deliberately exposes only canonical SDK
// values and is intended for simulator-only tests.
class WonderMakerSimulator final : public IWonderMakerHttpTransport,
                                   public IWonderMakerWebSocketTransport,
                                   public IWonderMakerDiscoveryTransport {
public:
    explicit WonderMakerSimulator(DeviceEndpoint endpoint = {},
                                  std::string device_id = "simulator-device");

    WonderMakerHttpResponse request(const WonderMakerHttpRequest& request) override;
    bool connect(const DeviceEndpoint& endpoint,
                 MessageHandler on_message,
                 CloseHandler on_close) override;
    bool send(const std::string& payload) override;
    void close() override;
    std::vector<DiscoveryCandidate> discover() override;

    const DeviceEndpoint& endpoint() const { return m_endpoint; }
    const std::string& device_id() const { return m_device_id; }
    const PrinterStatus& status() const { return m_status; }
    const PrinterCapability& capability() const { return m_capability; }
    uint64_t status_sequence() const { return m_status_sequence; }
    uint64_t control_invocations(const std::string& command_id) const;
    uint64_t status_queries() const { return m_status_queries; }
    uint64_t file_transfer_requests() const { return m_file_transfer_requests; }
    uint64_t accepted_file_offset() const { return m_accepted_file_offset; }
    bool status_subscription_active() const { return m_status_subscription_active; }
    bool websocket_open() const { return m_websocket_open; }
    const std::vector<WonderMakerHttpRequest>& requests() const { return m_requests; }

    void set_status(PrinterStatus status);
    void set_expected_file_digest(std::string sha256, uint64_t total_size);
    void emit_status();
    void drop_websocket();
    void restart_device();
    void set_http_available(bool available) { m_http_available = available; }
    void set_websocket_available(bool available) { m_websocket_available = available; }

private:
    static std::string status_event(const PrinterStatus& status, uint64_t sequence);
    WonderMakerHttpResponse response(int status_code, const std::string& body) const;
    WonderMakerHttpResponse failure(int status_code, const std::string& code) const;

    DeviceEndpoint m_endpoint;
    std::string m_device_id;
    PrinterCapability m_capability;
    PrinterStatus m_status;
    uint64_t m_status_sequence{0};
    uint64_t m_status_queries{0};
    uint64_t m_file_transfer_requests{0};
    uint64_t m_accepted_file_offset{0};
    uint64_t m_expected_file_size{0};
    std::string m_expected_file_digest;
    bool m_paired{false};
    bool m_authenticated{false};
    bool m_websocket_open{false};
    bool m_status_subscription_active{false};
    bool m_http_available{true};
    bool m_websocket_available{true};
    MessageHandler m_on_message;
    CloseHandler m_on_close;
    std::map<std::string, uint64_t> m_control_counts;
    std::map<std::string, ControlReceipt> m_control_receipts;
    std::vector<WonderMakerHttpRequest> m_requests;
};

const char* monitor_state_name(MonitorState state);
const char* control_operation_name(ControlOperation operation);

} // namespace Orca::SDK::WonderMaker