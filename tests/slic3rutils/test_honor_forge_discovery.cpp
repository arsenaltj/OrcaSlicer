#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/Utils/HonorForgeDiscovery.hpp"
#include "slic3r/Utils/HonorForgePrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"
#include "test_utils.hpp"

#include <nlohmann/json.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/cstdlib.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace Slic3r;

namespace {

PrinterDiscoveryCandidate candidate(PrinterDiscoverySource source, std::string host, uint16_t port = 7125)
{
    PrinterDiscoveryCandidate value;
    value.source = source;
    value.host = std::move(host);
    value.port = port;
    return value;
}

class FixedProvider final : public IPrinterDiscoveryProvider
{
public:
    FixedProvider(PrinterDiscoverySource source, std::vector<PrinterDiscoveryCandidate> candidates,
                  PrinterDiscoveryProviderError error = PrinterDiscoveryProviderError::None)
        : m_source(source), m_candidates(std::move(candidates)), m_error(error)
    {}

    PrinterDiscoverySource source() const override { return m_source; }
    PrinterDiscoveryProviderResult discover(const PrinterDiscoveryRequest&,
                                             const std::vector<PrinterNetworkInterface>&,
                                             const Cancelled& cancelled) override
    {
        ++calls;
        if (cancelled && cancelled())
            return {PrinterDiscoveryProviderError::Cancelled, {}};
        return {m_error, m_candidates};
    }

    std::atomic<int> calls {0};

private:
    PrinterDiscoverySource m_source;
    std::vector<PrinterDiscoveryCandidate> m_candidates;
    PrinterDiscoveryProviderError m_error;
};

class SupersededProvider final : public IPrinterDiscoveryProvider
{
public:
    PrinterDiscoverySource source() const override { return PrinterDiscoverySource::Mdns; }
    PrinterDiscoveryProviderResult discover(const PrinterDiscoveryRequest&,
                                             const std::vector<PrinterNetworkInterface>&,
                                             const Cancelled& cancelled) override
    {
        const int call = ++calls;
        if (call == 1) {
            first_started = true;
            while (!(cancelled && cancelled()))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return {PrinterDiscoveryProviderError::Cancelled, {}};
        }
        return {PrinterDiscoveryProviderError::None,
                {candidate(PrinterDiscoverySource::Mdns, "192.0.2.22")}};
    }

    std::atomic<int> calls {0};
    std::atomic<bool> first_started {false};
};

class ProbeAgent final : public HonorForgePrinterAgent
{
public:
    ProbeAgent(std::vector<std::shared_ptr<IPrinterDiscoveryProvider>> providers,
               ProbeError result = ProbeError::None,
               std::chrono::milliseconds budget = std::chrono::seconds(9), bool has_network = true)
        : HonorForgePrinterAgent("", std::move(providers), [has_network] {
              return has_network ? std::vector<PrinterNetworkInterface>{{"test-wifi", "192.0.2.4", 24}} :
                                   std::vector<PrinterNetworkInterface>{};
          }, budget)
        , m_result(result)
    {}

protected:
    ProbeResult probe_candidate(const PrinterDiscoveryCandidate& value,
                                const std::function<bool()>& cancelled) const override
    {
        ProbeResult result;
        result.candidate = value;
        if (cancelled && cancelled()) {
            result.error = ProbeError::Cancelled;
            return result;
        }
        ++probe_calls;
        result.error = m_result;
        result.error_code = m_result == ProbeError::None ? "" : "fixture_failure";
        result.dev_name = "Workshop Printer";
        result.version = "fixture-version";
        result.klippy_state = "ready";
        result.virtual_printer_uuid = value.instance_uuid;
        return result;
    }

public:
    mutable std::atomic<int> probe_calls {0};

private:
    ProbeError m_result;
};

PrinterDiscoveryBackendStatus wait_for_terminal(HonorForgePrinterAgent& agent)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    PrinterDiscoveryBackendStatus status;
    do {
        status = agent.discovery_status();
        if (status.phase == PrinterDiscoveryBackendPhase::Completed ||
            status.phase == PrinterDiscoveryBackendPhase::Failed ||
            status.phase == PrinterDiscoveryBackendPhase::Cancelled)
            return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    return status;
}

} // namespace

TEST_CASE("Honor Forge stops after a validated mDNS device", "[HonorForgeDiscovery]")
{
    auto mdns = std::make_shared<FixedProvider>(PrinterDiscoverySource::Mdns,
        std::vector<PrinterDiscoveryCandidate>{candidate(PrinterDiscoverySource::Mdns, "192.0.2.10")});
    auto subnet = std::make_shared<FixedProvider>(PrinterDiscoverySource::SubnetProbe,
        std::vector<PrinterDiscoveryCandidate>{candidate(PrinterDiscoverySource::SubnetProbe, "192.0.2.11")});
    ProbeAgent agent({mdns, subnet});
    std::vector<nlohmann::json> emitted;
    agent.set_on_ssdp_msg_fn([&](std::string payload) { emitted.push_back(nlohmann::json::parse(payload)); });

    REQUIRE(agent.start_discovery(true, false));
    const auto status = wait_for_terminal(agent);

    REQUIRE(status.phase == PrinterDiscoveryBackendPhase::Completed);
    REQUIRE(status.result == PrinterDiscoveryBackendResult::DevicesFound);
    REQUIRE(status.validated_count == 1);
    REQUIRE(status.emitted_count == 1);
    REQUIRE(mdns->calls == 1);
    REQUIRE(subnet->calls == 0);
    REQUIRE(emitted.size() == 1);
    CHECK(emitted.front().at("dev_name") == "Workshop Printer");
    CHECK(emitted.front().at("dev_id") == "192.0.2.10:7125");
    CHECK(emitted.front().at("discovery_source") == "mdns");
}

TEST_CASE("Honor Forge emits one physical printer when two endpoints validate", "[HonorForgeDiscovery]")
{
    auto mdns = std::make_shared<FixedProvider>(PrinterDiscoverySource::Mdns,
        std::vector<PrinterDiscoveryCandidate>{
            candidate(PrinterDiscoverySource::Mdns, "192.0.2.31", 80),
            candidate(PrinterDiscoverySource::Mdns, "192.0.2.31", 7125)});
    ProbeAgent agent({mdns});
    std::vector<nlohmann::json> emitted;
    agent.set_on_ssdp_msg_fn([&](std::string payload) { emitted.push_back(nlohmann::json::parse(payload)); });

    REQUIRE(agent.start_discovery(true, false));
    const auto status = wait_for_terminal(agent);

    REQUIRE(status.result == PrinterDiscoveryBackendResult::DevicesFound);
    CHECK(status.validated_count == 2);
    CHECK(status.emitted_count == 1);
    REQUIRE(emitted.size() == 1);
    CHECK(emitted.front().at("dev_id") == "192.0.2.31:7125");
    CHECK(emitted.front().at("dev_ip") == "192.0.2.31:7125");
}

TEST_CASE("Honor Forge falls back to the subnet after an empty mDNS result", "[HonorForgeDiscovery]")
{
    auto mdns = std::make_shared<FixedProvider>(PrinterDiscoverySource::Mdns,
                                                std::vector<PrinterDiscoveryCandidate>{});
    auto subnet = std::make_shared<FixedProvider>(PrinterDiscoverySource::SubnetProbe,
        std::vector<PrinterDiscoveryCandidate>{candidate(PrinterDiscoverySource::SubnetProbe, "192.0.2.12")});
    ProbeAgent agent({mdns, subnet});
    unsigned emitted = 0;
    agent.set_on_ssdp_msg_fn([&](std::string) { ++emitted; });

    REQUIRE(agent.start_discovery(true, false));
    const auto status = wait_for_terminal(agent);

    REQUIRE(status.result == PrinterDiscoveryBackendResult::DevicesFound);
    REQUIRE(mdns->calls == 1);
    REQUIRE(subnet->calls == 1);
    REQUIRE(agent.probe_calls == 1);
    REQUIRE(emitted == 1);
}

TEST_CASE("Honor Forge reports why candidates were rejected", "[HonorForgeDiscovery]")
{
    const auto error = GENERATE(HonorForgePrinterAgent::ProbeError::AuthenticationRequired,
                                HonorForgePrinterAgent::ProbeError::ProtocolMismatch,
                                HonorForgePrinterAgent::ProbeError::NetworkFailure,
                                HonorForgePrinterAgent::ProbeError::Timeout);
    auto provider = std::make_shared<FixedProvider>(PrinterDiscoverySource::Mdns,
        std::vector<PrinterDiscoveryCandidate>{candidate(PrinterDiscoverySource::Mdns, "192.0.2.13")});
    ProbeAgent agent({provider}, error);
    agent.set_on_ssdp_msg_fn([](std::string) {});

    REQUIRE(agent.start_discovery(true, false));
    const auto status = wait_for_terminal(agent);

    const auto expected = error == HonorForgePrinterAgent::ProbeError::AuthenticationRequired ?
                              PrinterDiscoveryBackendResult::AuthenticationRequired :
                          error == HonorForgePrinterAgent::ProbeError::ProtocolMismatch ?
                              PrinterDiscoveryBackendResult::ProtocolMismatch :
                          error == HonorForgePrinterAgent::ProbeError::Timeout ?
                              PrinterDiscoveryBackendResult::Timeout :
                              PrinterDiscoveryBackendResult::NetworkFailure;
    REQUIRE(status.result == expected);
    REQUIRE(status.emitted_count == 0);
}

TEST_CASE("A newer Honor Forge request cancels the previous generation", "[HonorForgeDiscovery]")
{
    auto provider = std::make_shared<SupersededProvider>();
    ProbeAgent agent({provider});
    unsigned emitted = 0;
    agent.set_on_ssdp_msg_fn([&](std::string) { ++emitted; });

    REQUIRE(agent.start_discovery(true, false));
    const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!provider->first_started && std::chrono::steady_clock::now() < start_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(provider->first_started);

    REQUIRE(agent.start_discovery(true, false));
    const auto status = wait_for_terminal(agent);

    REQUIRE(provider->calls == 2);
    REQUIRE(status.request_id == 2);
    REQUIRE(status.result == PrinterDiscoveryBackendResult::DevicesFound);
    REQUIRE(emitted == 1);
}

TEST_CASE("Honor Forge probe work stops at the discovery deadline", "[HonorForgeDiscovery]")
{
    class DeadlineProbeAgent final : public HonorForgePrinterAgent
    {
    public:
        explicit DeadlineProbeAgent(std::shared_ptr<IPrinterDiscoveryProvider> provider)
            : HonorForgePrinterAgent("", {std::move(provider)}, [] {
                  return std::vector<PrinterNetworkInterface>{{"test-wifi", "192.0.2.4", 24}};
              }, std::chrono::milliseconds(40))
        {}

    protected:
        ProbeResult probe_candidate(const PrinterDiscoveryCandidate& value,
                                    const std::function<bool()>& cancelled) const override
        {
            while (!(cancelled && cancelled()))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ProbeResult result;
            result.candidate = value;
            result.error = ProbeError::Cancelled;
            return result;
        }
    };

    auto provider = std::make_shared<FixedProvider>(PrinterDiscoverySource::Mdns,
        std::vector<PrinterDiscoveryCandidate>{candidate(PrinterDiscoverySource::Mdns, "192.0.2.30")});
    DeadlineProbeAgent agent(provider);
    agent.set_on_ssdp_msg_fn([](std::string) {});

    const auto started = std::chrono::steady_clock::now();
    REQUIRE(agent.start_discovery(true, false));
    const auto status = wait_for_terminal(agent);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(status.phase == PrinterDiscoveryBackendPhase::Completed);
    CHECK(status.result == PrinterDiscoveryBackendResult::Timeout);
    CHECK(elapsed < std::chrono::seconds(1));
}

TEST_CASE("Subnet discovery stays inside the bounded local prefix", "[HonorForgeDiscovery]")
{
    auto provider = make_subnet_printer_discovery_provider();
    PrinterDiscoveryRequest request;
    request.request_id = 1;
    request.agent_id = HONOR_FORGE_PRINTER_AGENT_ID;
    request.probe_ports = {7125, 80};
    request.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    request.max_hosts_per_interface = 254;

    const auto result = provider->discover(request, {{"test-wifi", "192.168.7.42", 16}}, [] { return false; });

    REQUIRE(result.error == PrinterDiscoveryProviderError::None);
    REQUIRE(result.candidates.size() == 506);
    for (size_t index = 0; index < result.candidates.size(); ++index) {
        const auto& value = result.candidates[index];
        CHECK(value.host.rfind("192.168.7.", 0) == 0);
        CHECK(value.host != "192.168.7.42");
        CHECK(value.port == (index < 253 ? 7125 : 80));
    }
}

TEST_CASE("Local virtual printer declarations reject unrelated addresses and malformed identities", "[HonorForgeDiscovery]")
{
    ScopedTemporaryFile file(".json");
    const nlohmann::json valid{{"schema", 1}, {"host", "127.0.0.1"}, {"port", 7128},
                               {"state", "ready"}, {"instance_uuid", "3cc075a7-e25d-4de1-b433-dd6981eca8ad"}};
    auto provider = make_local_virtual_printer_discovery_provider(file.string());
    PrinterDiscoveryRequest request;
    request.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto check = [&](const nlohmann::json& value) {
        { boost::nowide::ofstream stream(file.string()); stream << value.dump(); }
        return provider->discover(request, {}, [] { return false; });
    };
    const auto result = check(valid);
    REQUIRE(result.candidates.size() == 1);
    CHECK(result.candidates.front().host == "127.0.0.1");
    CHECK(result.candidates.front().port == 7128);
    CHECK(result.candidates.front().instance_uuid == valid["instance_uuid"]);
    const auto [key, value] = GENERATE(table<std::string, nlohmann::json>({
        {"host", "192.0.2.10"}, {"host", "localhost"}, {"port", 80}, {"port", 7136},
        {"port", "7125"}, {"state", "starting"}, {"schema", 2}, {"instance_uuid", "not-a-uuid"}}));
    auto invalid = valid;
    invalid[key] = value;
    CHECK(check(invalid).candidates.empty());
    CHECK(provider->discover(request, {}, [] { return true; }).error == PrinterDiscoveryProviderError::Cancelled);
}

TEST_CASE("A validated local virtual printer remains discoverable without a network interface", "[HonorForgeDiscovery]")
{
    auto local = candidate(PrinterDiscoverySource::LocalVirtualPrinter, "127.0.0.1", 7129);
    local.instance_uuid = "3cc075a7-e25d-4de1-b433-dd6981eca8ad";
    auto provider = std::make_shared<FixedProvider>(PrinterDiscoverySource::LocalVirtualPrinter,
        std::vector<PrinterDiscoveryCandidate>{local});
    ProbeAgent agent({provider}, HonorForgePrinterAgent::ProbeError::None, std::chrono::seconds(2), false);
    nlohmann::json emitted;
    agent.set_on_ssdp_msg_fn([&](std::string payload) { emitted = nlohmann::json::parse(payload); });
    REQUIRE(agent.start_discovery(true, false));
    const auto status = wait_for_terminal(agent);
    REQUIRE(status.result == PrinterDiscoveryBackendResult::DevicesFound);
    REQUIRE(emitted.is_object());
    CHECK(emitted.at("dev_ip") == "127.0.0.1:7129");
    CHECK(emitted.at("discovery_source") == "local_virtual_printer");
}

TEST_CASE("A virtual printer keeps its local endpoint while mDNS can still find other printers", "[HonorForgeDiscovery]")
{
    auto local = candidate(PrinterDiscoverySource::LocalVirtualPrinter, "127.0.0.1", 7130);
    local.instance_uuid = "3cc075a7-e25d-4de1-b433-dd6981eca8ad";
    auto lan = candidate(PrinterDiscoverySource::Mdns, "192.0.2.44", 7130);
    lan.instance_uuid = local.instance_uuid;
    auto local_provider = std::make_shared<FixedProvider>(PrinterDiscoverySource::LocalVirtualPrinter,
        std::vector<PrinterDiscoveryCandidate>{local});
    auto mdns = std::make_shared<FixedProvider>(PrinterDiscoverySource::Mdns,
        std::vector<PrinterDiscoveryCandidate>{lan, candidate(PrinterDiscoverySource::Mdns, "192.0.2.45")});
    auto subnet = std::make_shared<FixedProvider>(PrinterDiscoverySource::SubnetProbe,
        std::vector<PrinterDiscoveryCandidate>{});
    ProbeAgent agent({local_provider, mdns, subnet});
    std::vector<nlohmann::json> emitted;
    agent.set_on_ssdp_msg_fn([&](std::string payload) { emitted.push_back(nlohmann::json::parse(payload)); });
    REQUIRE(agent.start_discovery(true, false));
    const auto status = wait_for_terminal(agent);
    CHECK(status.validated_count == 3);
    CHECK(status.emitted_count == 2);
    REQUIRE(emitted.size() == 2);
    CHECK(emitted.front().at("dev_ip") == "127.0.0.1:7130");
    CHECK(mdns->calls == 1);
    CHECK(subnet->calls == 0);
}

// Explicit opt-in: this is a real service launched with isolated task data, never a printer on the LAN.
TEST_CASE("Honor Forge verifies the running local simulator identity over HTTP", "[.VirtualPrinterLive]")
{
    const char* runtime_path = boost::nowide::getenv("HF_VP_RUNTIME");
    REQUIRE(runtime_path != nullptr);
    const std::string path(runtime_path);
    boost::nowide::ifstream input(path);
    const auto runtime = nlohmann::json::parse(input);
    class LiveAgent final : public HonorForgePrinterAgent {
    public:
        using HonorForgePrinterAgent::probe_candidate;
        explicit LiveAgent(const std::string& runtime)
            : HonorForgePrinterAgent("", {make_local_virtual_printer_discovery_provider(runtime)}, [] {
                  return std::vector<PrinterNetworkInterface>{};
              }) {}
    } agent(path);
    std::vector<nlohmann::json> emitted;
    agent.set_on_ssdp_msg_fn([&](std::string payload) { emitted.push_back(nlohmann::json::parse(payload)); });
    REQUIRE(agent.start_discovery(true, false));
    REQUIRE(wait_for_terminal(agent).result == PrinterDiscoveryBackendResult::DevicesFound);
    REQUIRE(emitted.size() == 1);
    auto wrong = candidate(PrinterDiscoverySource::LocalVirtualPrinter, "127.0.0.1", runtime["port"].get<uint16_t>());
    wrong.instance_uuid = "00000000-0000-0000-0000-000000000000";
    CHECK(agent.probe_candidate(wrong, [] { return false; }).error == HonorForgePrinterAgent::ProbeError::ProtocolMismatch);
}
