#include <catch2/catch_all.hpp>

#include "orca/sdk/MobileSDK.hpp"
#include "orca/sdk/CommandEnvelope.hpp"
#include "orca/sdk/Connectivity.hpp"
#include "orca/sdk/ModelGeneration.hpp"
#include "orca/sdk/Slicing.hpp"

using namespace Orca::SDK;

namespace {

std::array<PathObservation, 3> all_paths(bool lan, bool p2p, bool relay)
{
    return {{
        {ConnectionPath::Lan, lan, lan, 4, 0},
        {ConnectionPath::P2p, p2p, p2p, 35, 0},
        {ConnectionPath::Relay, relay, relay, 120, 0},
    }};
}

} // namespace

TEST_CASE("MobileSDK exposes a shared desktop build identity", "[MobileSDK]")
{
    static_assert(kMobileSdkContractMajor == 1);
    static_assert(kMobileSdkContractMinor == 0);
    REQUIRE(std::string(kMobileSdkContractName) == "orca.mobile-sdk/v1");
    REQUIRE(std::string(mobile_sdk_source_commit()).size() > 0);
    REQUIRE(std::string(mobile_sdk_source_commit_short()).size() > 0);
}

TEST_CASE("Connection path selection follows LAN P2P relay priority", "[MobileSDK]")
{
    ConnectionPathSelector selector;

    auto selection = selector.select(all_paths(true, true, true));
    REQUIRE(selection.available);
    REQUIRE(selection.path == ConnectionPath::Lan);
    REQUIRE(selection.changed);
    REQUIRE(selection.requires_state_sync);

    selection = selector.select(all_paths(false, true, true));
    REQUIRE(selection.available);
    REQUIRE(selection.path == ConnectionPath::P2p);
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

TEST_CASE("Connection state machine exposes discovery probe switch and offline transitions", "[MobileSDK]")
{
    ConnectionStateMachine machine;
    machine.begin_discovery();
    REQUIRE(machine.state() == ConnectionState::Discovering);

    machine.begin_probe();
    REQUIRE(machine.state() == ConnectionState::Probing);
    REQUIRE(machine.select_path(all_paths(true, true, true)).path == ConnectionPath::Lan);
    REQUIRE(machine.state() == ConnectionState::Connecting);

    machine.mark_connected();
    REQUIRE(machine.state() == ConnectionState::Connected);
    REQUIRE(machine.select_path(all_paths(false, true, true)).path == ConnectionPath::P2p);
    REQUIRE(machine.state() == ConnectionState::Switching);

    REQUIRE(machine.select_path(all_paths(false, false, false)).requires_state_sync);
    REQUIRE(machine.state() == ConnectionState::Offline);
}

TEST_CASE("Connection path selection stays sticky while the active path is healthy", "[MobileSDK]")
{
    ConnectionPathSelector selector;
    REQUIRE(selector.select(all_paths(false, true, true)).path == ConnectionPath::P2p);

    const auto selection = selector.select(all_paths(true, true, true));
    REQUIRE(selection.path == ConnectionPath::P2p);
    REQUIRE_FALSE(selection.changed);
    REQUIRE_FALSE(selection.requires_state_sync);
}

TEST_CASE("Command envelope validates identity sequence and expiry", "[MobileSDK]")
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

    command.sequence = 0;
    REQUIRE(command.validate(100) == CommandValidationError::InvalidSequence);

    nlohmann::json incomplete = command.to_json();
    incomplete.erase("session_id");
    REQUIRE_FALSE(CommandEnvelope::from_json(incomplete).has_value());
}

TEST_CASE("AI and slicing DTOs round trip through versioned JSON", "[MobileSDK]")
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

TEST_CASE("Printer contracts serialize without desktop types", "[MobileSDK]")
{
    PrinterCapability capability;
    capability.status_stream = true;
    capability.upload_gcode = true;
    capability.start_print = true;

    PrinterStatus status;
    status.device_id = "printer-1";
    status.state = PrinterState::Printing;
    status.progress = 0.5;
    status.current_file = "plate_1.gcode";

    const auto capability_json = nlohmann::json(capability);
    const auto status_json = nlohmann::json(status);
    REQUIRE(capability_json.at("status_stream").get<bool>());
    REQUIRE(status_json.at("state").get<std::string>() == "printing");
    REQUIRE(status_json.at("current_file").get<std::string>() == "plate_1.gcode");

    const auto decoded = status_json.get<PrinterStatus>();
    REQUIRE(decoded.state == PrinterState::Printing);
    REQUIRE_THAT(decoded.progress, Catch::Matchers::WithinAbs(0.5, 1e-9));
}
