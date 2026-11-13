#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <memory>
#include <vector>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/HonorForgePrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

using namespace Slic3r;

namespace {

class RecordingAgent : public MoonrakerPrinterAgent
{
public:
    RecordingAgent() : MoonrakerPrinterAgent("") {}
    int         connections = 0;
    std::string port;
    int connect_printer(const PrinterConnectionParams& params) override
    {
        ++connections;
        port = params.port;
        return 0;
    }
    int disconnect_printer() override { return 0; }
};

} // namespace

TEST_CASE("Moonraker permits an empty access code", "[DeviceAccess]")
{
    MachineObject machine(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    machine.printer_agent_id = MOONRAKER_PRINTER_AGENT_ID;
    machine.set_access_code("", false);

    REQUIRE(machine.has_access_right());
}

TEST_CASE("Honor Forge permits an empty access code", "[DeviceAccess][HonorForgeDiscovery]")
{
    MachineObject machine(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    machine.printer_agent_id = HONOR_FORGE_PRINTER_AGENT_ID;
    machine.set_access_code("", false);

    REQUIRE(machine.has_access_right());
}

TEST_CASE("Only real network callbacks mark a printer as discovered", "[DeviceAccess][HonorForgeDiscovery]")
{
    AppConfig config;
    auto printer_agent = std::make_shared<MoonrakerPrinterAgent>("");
    NetworkAgent network(nullptr, printer_agent);
    DeviceManager manager(&network, false, &config);
    const std::string configured = R"({"dev_name":"Configured","dev_id":"192.0.2.20:7125","dev_ip":"192.0.2.20:7125",
        "dev_type":"","dev_signal":"0","connect_type":"lan","bind_state":"free","discovery_source":"configured_host"})";

    manager.on_machine_alive(configured);
    auto* machine = manager.get_local_machine("192.0.2.20:7125");
    REQUIRE(machine != nullptr);
    CHECK_FALSE(machine->discovered_on_network);

    manager.on_machine_alive(R"({"dev_name":"Discovered","dev_id":"192.0.2.20:7125","dev_ip":"192.0.2.20:7125",
        "dev_type":"","dev_signal":"0","connect_type":"lan","bind_state":"free","discovery_source":"mdns"})");
    CHECK(machine->discovered_on_network);
}

TEST_CASE("Honor Forge device lists collapse cached endpoints for one host", "[DeviceAccess][HonorForgeDiscovery]")
{
    AppConfig config;
    auto printer_agent = std::make_shared<HonorForgePrinterAgent>(
        "", std::vector<std::shared_ptr<IPrinterDiscoveryProvider>>{},
        [] { return std::vector<PrinterNetworkInterface>{}; });
    NetworkAgent network(nullptr, printer_agent);
    DeviceManager manager(&network, false, &config);

    manager.on_machine_alive(R"({"dev_name":"Proxy","dev_id":"192.0.2.40:80","dev_ip":"192.0.2.40:80",
        "dev_type":"","dev_signal":"0","connect_type":"lan","bind_state":"free","discovery_source":"mdns"})");
    manager.on_machine_alive(R"({"dev_name":"Moonraker","dev_id":"192.0.2.40:7125","dev_ip":"192.0.2.40:7125",
        "dev_type":"","dev_signal":"0","connect_type":"lan","bind_state":"free","discovery_source":"subnet_probe"})");

    auto visible = manager.get_my_machine_list(HONOR_FORGE_PRINTER_AGENT_ID);
    REQUIRE(visible.size() == 1);
    CHECK(visible.count("192.0.2.40:7125") == 1);

    auto* proxy = manager.get_local_machine("192.0.2.40:80");
    REQUIRE(proxy != nullptr);
    proxy->last_update_time = std::chrono::system_clock::now();
    proxy->m_push_count = 1;
    visible = manager.get_my_machine_list(HONOR_FORGE_PRINTER_AGENT_ID);
    REQUIRE(visible.size() == 1);
    CHECK(visible.count("192.0.2.40:80") == 1);
}

TEST_CASE("Moonraker logout revokes access until explicitly rebound", "[DeviceAccess]")
{
    MachineObject machine(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    machine.printer_agent_id = MOONRAKER_PRINTER_AGENT_ID;
    const std::string code = GENERATE("", "configured-key");
    machine.set_access_code(code, false);
    REQUIRE(machine.has_access_right());

    machine.revoke_access();
    REQUIRE(machine.get_access_code().empty());
    REQUIRE_FALSE(machine.has_access_right());

    machine.set_access_code(code, false);
    REQUIRE(machine.has_access_right());
    REQUIRE(machine.get_access_code() == code);
}

TEST_CASE("Revoked access prevents calls to the printer agent", "[DeviceAccess]")
{
    auto agent = std::make_shared<RecordingAgent>();
    NetworkAgent network(nullptr, agent);
    MachineObject machine(nullptr, &network, "test", "test_dev", "127.0.0.1");
    machine.printer_agent_id = GENERATE(BBL_PRINTER_AGENT_ID, ORCA_PRINTER_AGENT_ID, MOONRAKER_PRINTER_AGENT_ID);
    machine.set_access_code("configured-key", false);
    REQUIRE(machine.connect() == 0);
    REQUIRE(agent->connections == 1);

    machine.revoke_access();
    REQUIRE(machine.connect() == -1);
    REQUIRE(agent->connections == 1);

    machine.set_access_code("configured-key", false);
    REQUIRE(machine.connect() == 0);
    REQUIRE(agent->connections == 2);
}

TEST_CASE("A saved print host without a port connects on the port in its dev_id", "[DeviceAccess]")
{
    auto agent = std::make_shared<RecordingAgent>();
    NetworkAgent network(nullptr, agent);
    MachineObject machine(nullptr, &network, "test", "192.0.2.10:7125", "192.0.2.10");
    machine.set_access_code("configured-key", false);

    SECTION("Agents that connect like Moonraker repair the record")
    {
        machine.printer_agent_id = GENERATE(as<std::string>{}, MOONRAKER_PRINTER_AGENT_ID, QIDI_PRINTER_AGENT_ID,
                                            SNAPMAKER_PRINTER_AGENT_ID, CREALITYPRINT_PRINTER_AGENT_ID);
        REQUIRE(machine.connect() == 0);
        REQUIRE(agent->port == "7125");
    }

    SECTION("Other agents keep the saved address as it is")
    {
        machine.printer_agent_id = BBL_PRINTER_AGENT_ID;
        REQUIRE(machine.connect() == 0);
        REQUIRE(agent->port.empty());
    }
}

TEST_CASE("Other printer agents require an access code", "[DeviceAccess]")
{
    MachineObject machine(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    machine.printer_agent_id = GENERATE(
        BBL_PRINTER_AGENT_ID, ORCA_PRINTER_AGENT_ID, QIDI_PRINTER_AGENT_ID, SNAPMAKER_PRINTER_AGENT_ID, CREALITYPRINT_PRINTER_AGENT_ID, "plugin",
        "unknown");

    machine.set_access_code("", false);
    REQUIRE_FALSE(machine.has_access_right());

    machine.set_access_code("88888888", false);
    REQUIRE(machine.has_access_right());
}

TEST_CASE("Rebinding Moonraker with an empty key saves the machine", "[DeviceAccess]")
{
    AppConfig config;
    DeviceManager manager(nullptr, false, &config);
    MachineObject machine(&manager, nullptr, "test", "test_dev", "127.0.0.1");
    machine.printer_agent_id    = MOONRAKER_PRINTER_AGENT_ID;
    machine.dev_connection_type = "lan";
    machine.revoke_access();

    machine.set_access_code("");
    const auto& machines = config.get_local_machines();
    REQUIRE(machines.count("test_dev") == 1);
    REQUIRE(machines.at("test_dev").printer_agent_id == MOONRAKER_PRINTER_AGENT_ID);
    REQUIRE(machines.at("test_dev").access_code.empty());
}

TEST_CASE("Address-derived discovery leaves another agent's saved machine alone", "[DeviceAccess]")
{
    AppConfig config;
    const std::string active_agent = GENERATE(as<std::string>{}, MOONRAKER_PRINTER_AGENT_ID, HONOR_FORGE_PRINTER_AGENT_ID);
    std::shared_ptr<IPrinterAgent> printer_agent;
    if (active_agent == MOONRAKER_PRINTER_AGENT_ID)
        printer_agent = std::make_shared<MoonrakerPrinterAgent>("");
    else
        printer_agent = std::make_shared<HonorForgePrinterAgent>("", std::vector<std::shared_ptr<IPrinterDiscoveryProvider>>{},
                                                                  [] { return std::vector<PrinterNetworkInterface>{}; });
    NetworkAgent network(nullptr, std::move(printer_agent));
    DeviceManager manager(&network, false, &config);

    const std::string owner = GENERATE(as<std::string>{}, ORCA_PRINTER_AGENT_ID,
                                       MOONRAKER_PRINTER_AGENT_ID, HONOR_FORGE_PRINTER_AGENT_ID);
    BBLocalMachine saved;
    saved.dev_id           = "192.0.2.10:7125";
    saved.dev_ip           = saved.dev_id;
    saved.printer_agent_id = owner;
    config.update_local_machine(saved);

    manager.on_machine_alive(R"({"dev_name":"Klipper","dev_id":"192.0.2.10:7125","dev_ip":"192.0.2.10:7125",
        "dev_type":"","dev_signal":"0","connect_type":"lan","bind_state":"free"})");
    REQUIRE((manager.get_local_machine(saved.dev_id) != nullptr) == (owner == active_agent));
}
