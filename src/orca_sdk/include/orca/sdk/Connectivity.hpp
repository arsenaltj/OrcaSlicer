#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace Orca::SDK {

enum class ConnectionPath : uint8_t {
    Lan = 0,
    P2p = 1,
    Relay = 2,
};

enum class ConnectionState : uint8_t {
    Disconnected = 0,
    Discovering = 1,
    Probing = 2,
    Connecting = 3,
    Connected = 4,
    Switching = 5,
    Offline = 6,
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
    bool camera{false};
    bool upload_gcode{false};
    bool start_print{false};
    bool pause_resume{false};
    bool cancel{false};
    bool local_discovery{false};
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
    uint64_t observed_at_epoch_seconds{0};
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
    PathSelection select_path(const std::array<PathObservation, 3>& observations);
    void mark_connecting();
    void mark_connected();
    void mark_disconnected();
    void mark_offline();

    ConnectionState state() const { return m_state; }
    ConnectionPath current_path() const { return m_selector.current_path(); }
    bool has_path() const { return m_selector.has_path(); }

private:
    ConnectionPathSelector m_selector;
    ConnectionState m_state{ConnectionState::Disconnected};
};

const char* connection_path_name(ConnectionPath path);
const char* connection_state_name(ConnectionState state);
const char* printer_state_name(PrinterState state);

void to_json(nlohmann::json& json, ConnectionPath value);
void from_json(const nlohmann::json& json, ConnectionPath& value);
void to_json(nlohmann::json& json, const DeviceEndpoint& value);
void from_json(const nlohmann::json& json, DeviceEndpoint& value);
void to_json(nlohmann::json& json, const PrinterCapability& value);
void from_json(const nlohmann::json& json, PrinterCapability& value);
void to_json(nlohmann::json& json, const PrinterStatus& value);
void from_json(const nlohmann::json& json, PrinterStatus& value);

} // namespace Orca::SDK
