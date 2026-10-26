#include <catch2/catch_all.hpp>

#include "orca/connectivity/wondermaker/WonderMakerAdapter.hpp"

#include <cstdint>
#include <memory>
#include <string>

using namespace Orca::MobileDevice;
using namespace Orca::MobileDevice::WonderMaker;

namespace {

constexpr uint16_t kFixturePort = 49152;

DeviceEndpoint fixture_endpoint()
{
    // Test-only endpoint identity. No supplier port or wire path is implied.
    return {"http", "simulator.local", kFixturePort, false, {}};
}

struct Fixture {
    uint64_t now{1000};
    DeviceEndpoint endpoint{fixture_endpoint()};
    std::shared_ptr<WonderMakerSimulator> simulator{std::make_shared<WonderMakerSimulator>(endpoint, "sim-printer-1")};
    WonderMakerAdapter adapter{
        simulator,
        simulator,
        simulator,
        [this]() { return now; },
    };

    void onboard()
    {
        const auto candidates = adapter.discover();
        REQUIRE(candidates.size() == 1);
        REQUIRE(candidates.front().device_id == "sim-printer-1");
        adapter.set_manual_endpoint(candidates.front().endpoint);
        REQUIRE(adapter.probe(endpoint));
        REQUIRE(adapter.pair("opaque-pairing-input").success);
        REQUIRE(adapter.authenticate("opaque-auth-input", "opaque-certificate-fingerprint").success);
        REQUIRE(adapter.connect().success);
    }
};

PrinterStatus printing_status()
{
    PrinterStatus status;
    status.device_id = "sim-printer-1";
    status.state = PrinterState::Printing;
    status.progress = 0.42;
    status.nozzle_temperature_c = 205.0;
    status.bed_temperature_c = 60.0;
    status.current_file = "benchy.gcode";
    status.observed_at_epoch_seconds = 1001;
    return status;
}

} // namespace

TEST_CASE("WonderMaker simulator supports discovery, manual endpoint and canonical onboarding", "[WonderMaker][MobileDeviceSDK][Simulator]")
{
    Fixture fixture;
    const auto candidates = fixture.adapter.discover();
    REQUIRE(candidates.size() == 1);
    REQUIRE(candidates.front().endpoint.host == "simulator.local");
    REQUIRE(candidates.front().endpoint.port == kFixturePort);

    fixture.adapter.set_manual_endpoint(fixture.endpoint);
    REQUIRE(fixture.adapter.probe(fixture.endpoint));
    REQUIRE(fixture.adapter.capabilities().status_stream);
    REQUIRE(fixture.adapter.capabilities().upload_gcode);
    REQUIRE(fixture.adapter.pair("opaque-pairing-input").success);
    REQUIRE(fixture.adapter.authenticate("opaque-auth-input", "opaque-certificate-fingerprint").success);
    REQUIRE(fixture.adapter.connect().success);
    REQUIRE(fixture.adapter.monitor_state() == MonitorState::Connected);
    REQUIRE(fixture.simulator->status_subscription_active());
}

TEST_CASE("WonderMaker simulator maps status events and HTTP query fallback", "[WonderMaker][MobileDeviceSDK][Simulator]")
{
    Fixture fixture;
    fixture.onboard();
    const auto initial_queries = fixture.simulator->status_queries();

    fixture.simulator->set_status(printing_status());
    fixture.simulator->emit_status();
    REQUIRE(fixture.adapter.status_snapshot().status.state == PrinterState::Printing);
    REQUIRE_THAT(fixture.adapter.status_snapshot().status.progress, Catch::Matchers::WithinAbs(0.42, 1e-9));
    REQUIRE(fixture.adapter.status_snapshot().event_sequence == fixture.simulator->status_sequence());
    REQUIRE_FALSE(fixture.adapter.status_snapshot().stale);

    fixture.simulator->drop_websocket();
    REQUIRE(fixture.adapter.polling_fallback());
    REQUIRE(fixture.adapter.monitor_state() == MonitorState::Reconnecting);
    fixture.now += 1;
    fixture.adapter.tick();
    REQUIRE(fixture.simulator->status_queries() > initial_queries);
    REQUIRE(fixture.adapter.monitor_state() == MonitorState::Connected);
    REQUIRE_FALSE(fixture.adapter.status_snapshot().stale);
}

TEST_CASE("WonderMaker simulator enters stale state after ten seconds and reconciles within fifteen", "[WonderMaker][MobileDeviceSDK][Simulator]")
{
    Fixture fixture;
    fixture.onboard();
    fixture.simulator->drop_websocket();
    fixture.simulator->set_http_available(false);

    fixture.now += 10;
    fixture.adapter.tick();
    REQUIRE(fixture.adapter.monitor_state() == MonitorState::Stale);
    REQUIRE(fixture.adapter.status_snapshot().stale);
    REQUIRE(fixture.adapter.status_age_seconds() >= 10);

    fixture.simulator->set_http_available(true);
    fixture.simulator->set_websocket_available(true);
    fixture.now += 4;
    REQUIRE(fixture.adapter.reconnect_and_reconcile());
    REQUIRE(fixture.adapter.monitor_state() == MonitorState::Connected);
    REQUIRE_FALSE(fixture.adapter.status_snapshot().stale);
    REQUIRE(fixture.adapter.last_reconciled_at_epoch_seconds() - 1000 <= 15);
}

TEST_CASE("WonderMaker simulator deduplicates non-idempotent control command ids", "[WonderMaker][MobileDeviceSDK][Simulator]")
{
    Fixture fixture;
    fixture.onboard();

    const ControlRequest request{"command-1", ControlOperation::Start, "object://file", "benchy.gcode", "digest-1"};
    const auto first = fixture.adapter.send_control(request);
    const auto second = fixture.adapter.send_control(request);
    REQUIRE(first.result.success);
    REQUIRE_FALSE(first.duplicate);
    REQUIRE(second.result.success);
    REQUIRE(second.duplicate);
    REQUIRE(fixture.simulator->control_invocations("command-1") == 1);
    REQUIRE(fixture.simulator->status().state == PrinterState::Printing);
}

TEST_CASE("WonderMaker simulator resumes file transfer and verifies canonical digest", "[WonderMaker][MobileDeviceSDK][Simulator]")
{
    Fixture fixture;
    fixture.onboard();
    fixture.simulator->set_expected_file_digest("sha256-file", 10);

    const auto first = fixture.adapter.upload_file({"object://benchy", "benchy.gcode", "sha256-file", "12345", 0, 10});
    REQUIRE(first.success);
    REQUIRE_FALSE(first.complete);
    REQUIRE_FALSE(first.digest_verified);
    REQUIRE(first.next_offset == 5);

    const auto second = fixture.adapter.upload_file({"object://benchy", "benchy.gcode", "sha256-file", "67890", 5, 10});
    REQUIRE(second.success);
    REQUIRE(second.complete);
    REQUIRE(second.digest_verified);
    REQUIRE(second.next_offset == 10);
    REQUIRE(fixture.simulator->accepted_file_offset() == 10);
}

TEST_CASE("WonderMaker simulator restart requires reconnect and preserves canonical status", "[WonderMaker][MobileDeviceSDK][Simulator]")
{
    Fixture fixture;
    fixture.onboard();
    fixture.simulator->set_status(printing_status());
    fixture.simulator->emit_status();
    const auto sequence_before_restart = fixture.adapter.status_snapshot().event_sequence;

    fixture.simulator->restart_device();
    REQUIRE(fixture.adapter.monitor_state() == MonitorState::Reconnecting);
    REQUIRE(fixture.adapter.reconnect_and_reconcile());
    REQUIRE(fixture.adapter.monitor_state() == MonitorState::Connected);
    REQUIRE(fixture.adapter.status_snapshot().event_sequence >= sequence_before_restart);
    REQUIRE(fixture.adapter.status_snapshot().status.device_id == "sim-printer-1");
}