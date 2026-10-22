#include <catch2/catch_all.hpp>

#include "orca/mobile/device/MobileDeviceSDK.hpp"
#include "orca/mobile/device/CommandEnvelope.hpp"
#include "orca/mobile/device/Connectivity.hpp"
#include "orca/mobile/device/ModelGeneration.hpp"
#include "orca/mobile/device/Slicing.hpp"

using namespace Orca::MobileDevice;

namespace {

std::array<PathObservation, 3> all_paths(bool lan, bool remote_broker, bool relay)
{
    return {{
        {ConnectionPath::Lan, lan, lan, 4, 0},
        {ConnectionPath::RemoteBroker, remote_broker, remote_broker, 35, 0},
        {ConnectionPath::Relay, relay, relay, 120, 0},
    }};
}

} // namespace

TEST_CASE("MobileDeviceSDK preserves legacy connection state numeric values", "[MobileDeviceSDK]")
{
    static_assert(static_cast<uint8_t>(ConnectionState::Connecting) == 3);
    static_assert(static_cast<uint8_t>(ConnectionState::Connected) == 4);
    static_assert(static_cast<uint8_t>(ConnectionState::Switching) == 5);
    static_assert(static_cast<uint8_t>(ConnectionState::Offline) == 6);
    static_assert(static_cast<uint8_t>(ConnectionState::Pairing) > static_cast<uint8_t>(ConnectionState::Offline));
    static_assert(static_cast<uint8_t>(ConnectionState::Authenticating) > static_cast<uint8_t>(ConnectionState::Offline));
    static_assert(static_cast<uint8_t>(ConnectionState::StateSyncing) > static_cast<uint8_t>(ConnectionState::Offline));
    static_assert(static_cast<uint8_t>(ConnectionState::Expired) > static_cast<uint8_t>(ConnectionState::Offline));
}

TEST_CASE("MobileDeviceSDK exposes a shared desktop build identity", "[MobileDeviceSDK]")
{
    static_assert(kMobileSdkContractMajor == 1);
    static_assert(kMobileSdkContractMinor == 0);
    REQUIRE(std::string(kMobileSdkContractName) == "orca.mobile-sdk/v1");
    REQUIRE(std::string(mobile_sdk_source_commit()).size() > 0);
    REQUIRE(std::string(mobile_sdk_source_commit_short()).size() > 0);
}

TEST_CASE("Connection path selection follows LAN RemoteBroker relay priority", "[MobileDeviceSDK]")
{
    ConnectionPathSelector selector;

    auto selection = selector.select(all_paths(true, true, true));
    REQUIRE(selection.available);
    REQUIRE(selection.path == ConnectionPath::Lan);
    REQUIRE(selection.changed);
    REQUIRE(selection.requires_state_sync);

    selection = selector.select(all_paths(false, true, true));
    REQUIRE(selection.available);
    REQUIRE(selection.path == ConnectionPath::RemoteBroker);
    REQUIRE(selection.changed);
    REQUIRE(selection.requires_state_sync);

    selection = selector.select(all_paths(false, false, true));
    REQUIRE(selection.path == ConnectionPath::Relay);
    REQUIRE(selection.changed);

    selection = selector.select(all_paths(false, false, false));
    REQUIRE_FALSE(selection.available);
    REQUIRE(selection.changed);
    REQUIRE(selection.requires_state_sync);
}

TEST_CASE("Connection path JSON emits RemoteBroker and accepts legacy P2P", "[MobileDeviceSDK]")
{
    nlohmann::json encoded = ConnectionPath::RemoteBroker;
    REQUIRE(encoded.get<std::string>() == "remote_broker");
    REQUIRE(nlohmann::json("p2p").get<ConnectionPath>() == ConnectionPath::RemoteBroker);
    REQUIRE(nlohmann::json("remote_broker").get<ConnectionPath>() == ConnectionPath::RemoteBroker);
}

TEST_CASE("Connection state machine covers pairing authentication sync expiration and offline transitions", "[MobileDeviceSDK]")
{
    ConnectionStateMachine machine;
    machine.begin_discovery();
    REQUIRE(machine.state() == ConnectionState::Discovering);

    machine.begin_probe();
    REQUIRE(machine.state() == ConnectionState::Probing);
    machine.begin_pairing();
    REQUIRE(machine.state() == ConnectionState::Pairing);
    machine.begin_authentication();
    REQUIRE(machine.state() == ConnectionState::Authenticating);
    REQUIRE(machine.select_path(all_paths(true, true, true)).path == ConnectionPath::Lan);
    REQUIRE(machine.state() == ConnectionState::Connecting);

    machine.mark_connected();
    REQUIRE(machine.state() == ConnectionState::Connected);
    REQUIRE(machine.select_path(all_paths(false, true, true)).path == ConnectionPath::RemoteBroker);
    REQUIRE(machine.state() == ConnectionState::Switching);
    REQUIRE(machine.state_sync_required());

    machine.begin_state_sync();
    REQUIRE(machine.state() == ConnectionState::StateSyncing);
    machine.mark_state_synchronized();
    REQUIRE(machine.state() == ConnectionState::Connected);
    REQUIRE_FALSE(machine.state_sync_required());

    machine.mark_state_expired();
    REQUIRE(machine.state() == ConnectionState::Expired);
    REQUIRE(machine.state_sync_required());
    REQUIRE(machine.select_path(all_paths(false, true, true)).path == ConnectionPath::RemoteBroker);
    REQUIRE(machine.state() == ConnectionState::Expired);

    REQUIRE(machine.select_path(all_paths(false, false, false)).requires_state_sync);
    REQUIRE(machine.state() == ConnectionState::Offline);
}

TEST_CASE("Connection path selection stays sticky while the active path is healthy", "[MobileDeviceSDK]")
{
    ConnectionPathSelector selector;
    REQUIRE(selector.select(all_paths(false, true, true)).path == ConnectionPath::RemoteBroker);

    const auto selection = selector.select(all_paths(true, true, true));
    REQUIRE(selection.path == ConnectionPath::RemoteBroker);
    REQUIRE_FALSE(selection.changed);
    REQUIRE_FALSE(selection.requires_state_sync);
}

TEST_CASE("Connection state JSON round trips every v1.7 state", "[MobileDeviceSDK]")
{
    const std::array<std::pair<ConnectionState, const char*>, 11> states{{
        {ConnectionState::Disconnected, "disconnected"},
        {ConnectionState::Discovering, "discovering"},
        {ConnectionState::Probing, "probing"},
        {ConnectionState::Connecting, "connecting"},
        {ConnectionState::Connected, "connected"},
        {ConnectionState::Switching, "switching"},
        {ConnectionState::Offline, "offline"},
        {ConnectionState::Pairing, "pairing"},
        {ConnectionState::Authenticating, "authenticating"},
        {ConnectionState::StateSyncing, "state_syncing"},
        {ConnectionState::Expired, "expired"},
    }};

    for (const auto& [state, name] : states) {
        const nlohmann::json encoded = state;
        REQUIRE(encoded.get<std::string>() == name);
        REQUIRE(encoded.get<ConnectionState>() == state);
    }
}

TEST_CASE("Command envelope validates identity sequence and expiry", "[MobileDeviceSDK]")
{
    CommandEnvelope command;
    command.session_id = "session-1";
    command.device_id = "printer-1";
    command.command_id = "command-1";
    command.sequence = 7;
    command.expires_at_epoch_seconds = 120;
    command.type = "printer.pause";
    command.payload = {{"reason", "user_request"}};

    std::string detail;
    REQUIRE(command.validate(100, &detail) == CommandValidationError::None);
    REQUIRE(detail.empty());

    const auto encoded = command.to_json();
    const auto decoded = CommandEnvelope::from_json(encoded, &detail);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->is_retry_of(command));

    REQUIRE(command.validate(120, &detail) == CommandValidationError::Expired);
    REQUIRE(detail == "command has expired");

    auto invalid = command;
    invalid.sequence = 0;
    REQUIRE(invalid.validate(100) == CommandValidationError::InvalidSequence);

    invalid = command;
    invalid.protocol_version = "orca.command/v2";
    REQUIRE(invalid.validate(100) == CommandValidationError::UnsupportedProtocolVersion);

    invalid = command;
    invalid.protocol_version.clear();
    REQUIRE(invalid.validate(100) == CommandValidationError::MissingProtocolVersion);

    invalid = command;
    invalid.device_id.clear();
    REQUIRE(invalid.validate(100) == CommandValidationError::MissingDeviceId);

    invalid = command;
    invalid.command_id.clear();
    REQUIRE(invalid.validate(100) == CommandValidationError::MissingCommandId);

    invalid = command;
    invalid.type.clear();
    REQUIRE(invalid.validate(100) == CommandValidationError::MissingType);

    nlohmann::json incomplete = command.to_json();
    incomplete.erase("session_id");
    REQUIRE_FALSE(CommandEnvelope::from_json(incomplete, &detail).has_value());
    REQUIRE(detail == "session_id is required");

    auto different_command = command;
    different_command.command_id = "command-2";
    auto different_session = command;
    different_session.session_id = "session-2";
    auto different_device = command;
    different_device.device_id = "printer-2";
    auto different_payload = command;
    different_payload.payload["reason"] = "system_request";
    REQUIRE_FALSE(command.is_retry_of(different_command));
    REQUIRE_FALSE(command.is_retry_of(different_session));
    REQUIRE_FALSE(command.is_retry_of(different_device));
    REQUIRE_FALSE(command.is_retry_of(different_payload));
}

TEST_CASE("AI and slicing DTOs round trip through versioned JSON", "[MobileDeviceSDK]")
{
    ModelJob job;
    job.request_id = "request-1";
    job.job_id = "job-1";
    job.source = ModelInputSource::Image;
    job.state = ModelJobState::Generating;
    job.progress = 42;
    job.options.provider = "gateway";
    job.options.output_format = ModelArtifactFormat::Obj;
    job.artifact_sha256 = "abc";

    auto job_json = nlohmann::json(job);
    job_json["future_field"] = true;
    const ModelJob decoded_job = job_json.get<ModelJob>();
    REQUIRE(decoded_job.protocol_version == "orca.model-job/v1");
    REQUIRE(decoded_job.source == ModelInputSource::Image);
    REQUIRE(decoded_job.state == ModelJobState::Generating);
    REQUIRE(decoded_job.options.output_format == ModelArtifactFormat::Obj);

    SlicingRequest request;
    request.request_id = "slice-1";
    request.source_uri = "object://models/example.3mf";
    request.input_format = SlicingInputFormat::ThreeMf;
    request.execution_mode = SlicingExecutionMode::Remote;
    request.plate_index = 2;
    const SlicingRequest decoded_request = nlohmann::json(request).get<SlicingRequest>();
    REQUIRE(decoded_request.protocol_version == "orca.slicing/v1");
    REQUIRE(decoded_request.execution_mode == SlicingExecutionMode::Remote);
    REQUIRE(decoded_request.plate_index == 2);
}

TEST_CASE("Printer contracts serialize without desktop types", "[MobileDeviceSDK]")
{
    PrinterCapability capability;
    capability.status_stream = true;
    capability.device_pairing = true;
    capability.certificate_authentication = true;
    capability.upload_gcode = true;
    capability.resumable_upload = true;
    capability.start_print = true;
    capability.pause_print = true;
    capability.resume_print = true;
    capability.stop_print = true;

    PrinterStatus status;
    status.device_id = "printer-1";
    status.state = PrinterState::Printing;
    status.progress = 0.5;
    status.current_file = "plate_1.gcode";
    status.device_time_epoch_seconds = 1700000000;
    status.event_sequence = 42;
    status.last_sync_epoch_seconds = 1700000001;
    status.state_expired = true;
    status.connection_path = ConnectionPath::Lan;
    status.protocol_version = "wondermaker.lan/v1";

    const auto capability_json = nlohmann::json(capability);
    const auto status_json = nlohmann::json(status);
    REQUIRE(capability_json.at("status_stream").get<bool>());
    REQUIRE(capability_json.at("device_pairing").get<bool>());
    REQUIRE(capability_json.at("certificate_authentication").get<bool>());
    REQUIRE(capability_json.at("resumable_upload").get<bool>());
    REQUIRE(capability_json.at("pause_print").get<bool>());
    REQUIRE(capability_json.at("resume_print").get<bool>());
    REQUIRE(capability_json.at("stop_print").get<bool>());
    REQUIRE(capability_json.at("pause_resume").get<bool>());
    REQUIRE(capability_json.at("cancel").get<bool>() == capability.stop_print);

    const auto decoded_capability = capability_json.get<PrinterCapability>();
    REQUIRE(decoded_capability.status_stream);
    REQUIRE(decoded_capability.device_pairing);
    REQUIRE(decoded_capability.certificate_authentication);
    REQUIRE(decoded_capability.upload_gcode);
    REQUIRE(decoded_capability.resumable_upload);
    REQUIRE(decoded_capability.start_print);
    REQUIRE(decoded_capability.pause_print);
    REQUIRE(decoded_capability.resume_print);
    REQUIRE(decoded_capability.stop_print);
    REQUIRE(decoded_capability.pause_resume);
    REQUIRE(decoded_capability.cancel);

    const nlohmann::json legacy_capability = {
        {"status_stream", true},
        {"upload_gcode", true},
        {"pause_resume", true},
        {"cancel", true},
    };
    const auto decoded_legacy_capability = legacy_capability.get<PrinterCapability>();
    REQUIRE(decoded_legacy_capability.pause_resume);
    REQUIRE(decoded_legacy_capability.pause_print);
    REQUIRE(decoded_legacy_capability.resume_print);
    REQUIRE(decoded_legacy_capability.cancel);
    REQUIRE(decoded_legacy_capability.stop_print);

    const auto legacy_round_trip = nlohmann::json(decoded_legacy_capability).get<PrinterCapability>();
    REQUIRE(legacy_round_trip.pause_resume);
    REQUIRE(legacy_round_trip.pause_print);
    REQUIRE(legacy_round_trip.resume_print);
    REQUIRE(legacy_round_trip.cancel);
    REQUIRE(legacy_round_trip.stop_print);

    REQUIRE(status_json.at("state").get<std::string>() == "printing");
    REQUIRE(status_json.at("current_file").get<std::string>() == "plate_1.gcode");

    const auto decoded = status_json.get<PrinterStatus>();
    REQUIRE(decoded.state == PrinterState::Printing);
    REQUIRE_THAT(decoded.progress, Catch::Matchers::WithinAbs(0.5, 1e-9));
    REQUIRE(decoded.device_time_epoch_seconds == 1700000000);
    REQUIRE(decoded.event_sequence == 42);
    REQUIRE(decoded.last_sync_epoch_seconds == 1700000001);
    REQUIRE(decoded.state_expired);
    REQUIRE(decoded.connection_path == ConnectionPath::Lan);
    REQUIRE(decoded.protocol_version == "wondermaker.lan/v1");

    auto future_status = status_json;
    future_status["future_field"] = true;
    REQUIRE(future_status.get<PrinterStatus>().event_sequence == 42);
}

TEST_CASE("TransferJob and RemoteSession round trip with unknown extensions", "[MobileDeviceSDK]")
{
    TransferJob transfer;
    transfer.file_id = "file-1";
    transfer.size_bytes = 4096;
    transfer.digest_algorithm = "sha256";
    transfer.digest = "abc123";
    transfer.uploaded_offset = 2048;
    transfer.state = TransferJobState::Uploading;
    transfer.retry_policy = TransferRetryPolicy::ExponentialBackoff;
    transfer.device_download_state = DeviceDownloadState::Downloading;

    auto transfer_json = nlohmann::json(transfer);
    transfer_json["future_field"] = "ignored";
    const auto decoded_transfer = transfer_json.get<TransferJob>();
    REQUIRE(decoded_transfer.file_id == "file-1");
    REQUIRE(decoded_transfer.size_bytes == 4096);
    REQUIRE(decoded_transfer.digest_algorithm == "sha256");
    REQUIRE(decoded_transfer.digest == "abc123");
    REQUIRE(decoded_transfer.uploaded_offset == 2048);
    REQUIRE(decoded_transfer.state == TransferJobState::Uploading);
    REQUIRE(decoded_transfer.retry_policy == TransferRetryPolicy::ExponentialBackoff);
    REQUIRE(decoded_transfer.device_download_state == DeviceDownloadState::Downloading);

    RemoteSession session;
    session.device_id = "printer-1";
    session.platform_session = "platform-session-1";
    session.device_certificate_id = "certificate-1";
    session.connection_path = ConnectionPath::RemoteBroker;
    session.provider = "broker-provider";
    session.lease_expires_at_epoch_seconds = 1700000300;
    session.state_sync_required = true;
    session.state_synced = false;

    auto session_json = nlohmann::json(session);
    session_json["future_field"] = 7;
    const auto decoded_session = session_json.get<RemoteSession>();
    REQUIRE(decoded_session.device_id == "printer-1");
    REQUIRE(decoded_session.platform_session == "platform-session-1");
    REQUIRE(decoded_session.device_certificate_id == "certificate-1");
    REQUIRE(decoded_session.connection_path == ConnectionPath::RemoteBroker);
    REQUIRE(decoded_session.provider == "broker-provider");
    REQUIRE(decoded_session.lease_expires_at_epoch_seconds == 1700000300);
    REQUIRE(decoded_session.state_sync_required);
    REQUIRE_FALSE(decoded_session.state_synced);
}
