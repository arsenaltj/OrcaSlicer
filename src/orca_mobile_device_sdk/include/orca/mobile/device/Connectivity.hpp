#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace Orca::SDK {

enum class ConnectionPath : uint8_t {
    Lan = 0,
    RemoteBroker = 1,
    // Deprecated source compatibility alias. P2P remains a future path and is
    // never emitted by the v1.7 JSON contract.
    P2p = RemoteBroker,
    Relay = 2,
};

enum class ConnectionState : uint8_t {
    // Keep the original wire values stable. New v1.7 phases append below them.
    Disconnected = 0,
    Discovering = 1,
    Probing = 2,
    Connecting = 3,
    Connected = 4,
    Switching = 5,
    Offline = 6,
    Pairing = 7,
    Authenticating = 8,
    StateSyncing = 9,
    Expired = 10,
};

struct PathObservation {
    ConnectionPath path{ConnectionPath::Lan};
    bool reachable{false};
    bool authenticated{false};
    uint32_t latency_ms{0};
    uint32_t consecutive_failures{0};
};

struct PathSelection {
    ConnectionPath path{ConnectionPath::Lan};
    bool available{false};
    bool changed{false};
    bool requires_state_sync{false};
};

struct DeviceEndpoint {
    std::string scheme;
    std::string host;
    uint16_t port{0};
    bool tls{false};
    std::string certificate_fingerprint;
};

struct PrinterCapability {
    bool status_stream{false};
    bool device_pairing{false};
    bool certificate_authentication{false};
    bool camera{false};
    bool upload_gcode{false};
    bool resumable_upload{false};
    bool start_print{false};
    bool pause_print{false};
    bool resume_print{false};
    bool stop_print{false};
    bool local_discovery{false};

    // Retained for source and JSON compatibility with the original SDK.
    bool pause_resume{false};
    bool cancel{false};
};

enum class PrinterState : uint8_t {
    Unknown = 0,
    Idle = 1,
    Preparing = 2,
    Printing = 3,
    Paused = 4,
    Completed = 5,
    Error = 6,
    Offline = 7,
};

struct PrinterStatus {
    std::string device_id;
    PrinterState state{PrinterState::Unknown};
    double progress{0.0};
    double nozzle_temperature_c{0.0};
    double bed_temperature_c{0.0};
    std::string current_file;
    std::string diagnostic_code;

    // observed_at_epoch_seconds is retained for compatibility. The v1.7
    // monitoring contract uses device time and the last synchronization time.
    uint64_t observed_at_epoch_seconds{0};
    uint64_t device_time_epoch_seconds{0};
    uint64_t event_sequence{0};
    uint64_t last_sync_epoch_seconds{0};
    bool state_expired{false};
    ConnectionPath connection_path{ConnectionPath::Lan};
    std::string protocol_version;
};

enum class TransferJobState : uint8_t {
    Queued = 0,
    Uploading = 1,
    Paused = 2,
    Completed = 3,
    Failed = 4,
    Canceled = 5,
};

enum class TransferRetryPolicy : uint8_t {
    None = 0,
    Fixed = 1,
    ExponentialBackoff = 2,
};

enum class DeviceDownloadState : uint8_t {
    Unknown = 0,
    Pending = 1,
    Downloading = 2,
    Completed = 3,
    Failed = 4,
};

struct TransferJob {
    std::string file_id;
    uint64_t size_bytes{0};
    std::string digest_algorithm{"sha256"};
    std::string digest;
    uint64_t uploaded_offset{0};
    TransferJobState state{TransferJobState::Queued};
    TransferRetryPolicy retry_policy{TransferRetryPolicy::None};
    DeviceDownloadState device_download_state{DeviceDownloadState::Unknown};
};

struct RemoteSession {
    std::string device_id;
    std::string platform_session;
    std::string device_certificate_id;
    ConnectionPath connection_path{ConnectionPath::Lan};
    std::string provider;
    uint64_t lease_expires_at_epoch_seconds{0};
    bool state_sync_required{false};
    bool state_synced{false};
};

struct PrinterOperationResult {
    bool success{false};
    bool retryable{false};
    std::string code;
    std::string message;
};

struct GcodeUploadRequest {
    std::string source_uri;
    std::string sha256;
    std::string filename;
};

struct PrintCommand {
    std::string file_uri;
    std::string file_sha256;
    std::string filename;
};

class IPrinterProtocolAdapter {
public:
    virtual ~IPrinterProtocolAdapter() = default;

    virtual const char* id() const = 0;
    virtual bool probe(const DeviceEndpoint& endpoint) = 0;
    virtual PrinterCapability capabilities() const = 0;
    virtual PrinterStatus get_status() = 0;
    virtual PrinterOperationResult upload_gcode(const GcodeUploadRequest& request) = 0;
    virtual PrinterOperationResult start_print(const PrintCommand& command) = 0;
    virtual PrinterOperationResult pause() = 0;
    virtual PrinterOperationResult resume() = 0;
    virtual PrinterOperationResult cancel() = 0;
};

class ConnectionPathSelector {
public:
    ConnectionPathSelector() = default;

    void reset();
    PathSelection select(const std::array<PathObservation, 3>& observations);
    ConnectionPath current_path() const { return m_current_path; }
    bool has_path() const { return m_has_path; }

private:
    static const PathObservation* find(const std::array<PathObservation, 3>& observations,
                                       ConnectionPath path);

    ConnectionPath m_current_path{ConnectionPath::Lan};
    bool m_has_path{false};
};

class ConnectionStateMachine {
public:
    void reset();
    void begin_discovery();
    void begin_probe();
    void begin_pairing();
    void begin_authentication();
    PathSelection select_path(const std::array<PathObservation, 3>& observations);
    void mark_connecting();
    void mark_connected();
    void begin_state_sync();
    void mark_state_synchronized();
    void mark_state_expired();
    void mark_disconnected();
    void mark_offline();

    ConnectionState state() const { return m_state; }
    ConnectionPath current_path() const { return m_selector.current_path(); }
    bool has_path() const { return m_selector.has_path(); }
    bool state_sync_required() const { return m_state_sync_required; }

private:
    ConnectionPathSelector m_selector;
    ConnectionState m_state{ConnectionState::Disconnected};
    bool m_state_sync_required{false};
};

const char* connection_path_name(ConnectionPath path);
const char* connection_state_name(ConnectionState state);
const char* printer_state_name(PrinterState state);
const char* transfer_job_state_name(TransferJobState state);
const char* transfer_retry_policy_name(TransferRetryPolicy policy);
const char* device_download_state_name(DeviceDownloadState state);

void to_json(nlohmann::json& json, ConnectionPath value);
void from_json(const nlohmann::json& json, ConnectionPath& value);
void to_json(nlohmann::json& json, ConnectionState value);
void from_json(const nlohmann::json& json, ConnectionState& value);
void to_json(nlohmann::json& json, PrinterState value);
void from_json(const nlohmann::json& json, PrinterState& value);
void to_json(nlohmann::json& json, TransferJobState value);
void from_json(const nlohmann::json& json, TransferJobState& value);
void to_json(nlohmann::json& json, TransferRetryPolicy value);
void from_json(const nlohmann::json& json, TransferRetryPolicy& value);
void to_json(nlohmann::json& json, DeviceDownloadState value);
void from_json(const nlohmann::json& json, DeviceDownloadState& value);
void to_json(nlohmann::json& json, const DeviceEndpoint& value);
void from_json(const nlohmann::json& json, DeviceEndpoint& value);
void to_json(nlohmann::json& json, const PrinterCapability& value);
void from_json(const nlohmann::json& json, PrinterCapability& value);
void to_json(nlohmann::json& json, const PrinterStatus& value);
void from_json(const nlohmann::json& json, PrinterStatus& value);
void to_json(nlohmann::json& json, const TransferJob& value);
void from_json(const nlohmann::json& json, TransferJob& value);
void to_json(nlohmann::json& json, const RemoteSession& value);
void from_json(const nlohmann::json& json, RemoteSession& value);

} // namespace Orca::SDK
