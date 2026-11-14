#include "HonorForgePrinterAgent.hpp"

#include "NetworkAgentFactory.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace Slic3r {
namespace {

const char* probe_error_name(HonorForgePrinterAgent::ProbeError error)
{
    switch (error) {
    case HonorForgePrinterAgent::ProbeError::None: return "none";
    case HonorForgePrinterAgent::ProbeError::AuthenticationRequired: return "authentication_required";
    case HonorForgePrinterAgent::ProbeError::ProtocolMismatch: return "protocol_mismatch";
    case HonorForgePrinterAgent::ProbeError::NetworkFailure: return "network_failure";
    case HonorForgePrinterAgent::ProbeError::Timeout: return "timeout";
    case HonorForgePrinterAgent::ProbeError::Cancelled: return "cancelled";
    }
    return "unknown";
}

std::string discovery_device_ref(const std::string& value)
{
    std::ostringstream output;
    output << std::hex << std::hash<std::string>{}(value);
    return output.str();
}

uint64_t elapsed_ms(std::chrono::steady_clock::time_point started)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
}

int endpoint_preference(uint16_t port)
{
    if (port == 7125) return 2;
    if (port == 80) return 1;
    return 0;
}

bool tcp_port_reachable(const std::string& host, uint16_t port, const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled())
        return false;
    boost::system::error_code address_error;
    const auto address = boost::asio::ip::make_address(host, address_error);
    if (address_error)
        return false;

    boost::asio::io_context context;
    boost::asio::ip::tcp::socket socket(context);
    boost::asio::steady_timer timer(context);
    bool connected = false;
    bool finished = false;
    socket.async_connect({address, port}, [&](const boost::system::error_code& error) {
        connected = !error;
        finished = true;
        boost::system::error_code ignored;
        timer.cancel(ignored);
    });
    timer.expires_after(std::chrono::milliseconds(350));
    timer.async_wait([&](const boost::system::error_code& error) {
        if (!error && !finished) {
            boost::system::error_code ignored;
            socket.cancel(ignored);
        }
    });
    context.run();
    return connected && !(cancelled && cancelled());
}

} // namespace

HonorForgePrinterAgent::HonorForgePrinterAgent(std::string log_dir)
    : HonorForgePrinterAgent(std::move(log_dir),
                             {make_local_virtual_printer_discovery_provider(), make_mdns_printer_discovery_provider(),
                              make_subnet_printer_discovery_provider()},
                             enumerate_printer_network_interfaces)
{
}

HonorForgePrinterAgent::HonorForgePrinterAgent(
    std::string log_dir,
    std::vector<std::shared_ptr<IPrinterDiscoveryProvider>> providers,
    InterfaceEnumerator interface_enumerator,
    std::chrono::milliseconds discovery_budget)
    : MoonrakerPrinterAgent(std::move(log_dir))
    , m_providers(std::move(providers))
    , m_interface_enumerator(std::move(interface_enumerator))
    , m_discovery_budget(discovery_budget)
    , m_discovery_thread(&HonorForgePrinterAgent::worker_loop, this)
{
}

HonorForgePrinterAgent::~HonorForgePrinterAgent()
{
    {
        std::lock_guard<std::mutex> lock(m_discovery_mutex);
        m_shutdown = true;
        m_request_pending = false;
        ++m_generation;
    }
    m_discovery_changed.notify_all();
    if (m_discovery_thread.joinable())
        m_discovery_thread.join();
}

AgentInfo HonorForgePrinterAgent::get_agent_info_static()
{
    PrinterAgentTraits traits;
    traits.protocol_family = PrinterAgentTraits::ProtocolFamily::Moonraker;
    traits.accepts_empty_access_code = true;
    traits.uses_printhost_api_key = true;
    traits.supports_active_discovery = true;
    traits.preserves_foreign_device_ownership = true;
    traits.supports_monitoring_telemetry = true;
    traits.supports_upload_only = true;
    traits.supports_upload_and_print = true;
    traits.requires_task_identity_for_control = false;
    traits.job_payload = PrinterAgentTraits::JobPayload::RawGcode;
    traits.storage_model = PrinterAgentTraits::StorageModel::VirtualGcodeRoot;
    traits.requires_transfer_preflight = false;
    return AgentInfo{HONOR_FORGE_PRINTER_AGENT_ID, "Honor Forge", "1.0.0",
                     "Honor Forge discovery with Moonraker transport", traits};
}

bool HonorForgePrinterAgent::start_discovery(bool start, bool sending)
{
    (void) sending;
    const uint64_t generation = ++m_generation;
    {
        std::lock_guard<std::mutex> lock(m_discovery_mutex);
        if (m_shutdown)
            return false;
        m_request_pending = start;
        m_discovery_status = {};
        m_discovery_status.request_id = generation;
        m_discovery_status.phase = start ? PrinterDiscoveryBackendPhase::Running : PrinterDiscoveryBackendPhase::Cancelled;
    }
    m_discovery_changed.notify_all();
    if (!start) {
        BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] request_cancelled request=" << generation
                                << " agent=" << HONOR_FORGE_PRINTER_AGENT_ID << " reason=caller_stop";
    }
    return true;
}

PrinterDiscoveryBackendStatus HonorForgePrinterAgent::discovery_status() const
{
    std::lock_guard<std::mutex> lock(m_discovery_mutex);
    return m_discovery_status;
}

void HonorForgePrinterAgent::worker_loop()
{
    while (true) {
        uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(m_discovery_mutex);
            m_discovery_changed.wait(lock, [&] { return m_shutdown || m_request_pending; });
            if (m_shutdown)
                return;
            generation = m_generation.load();
            m_request_pending = false;
        }
        run_discovery(generation);
    }
}

void HonorForgePrinterAgent::set_status(
    uint64_t generation, const std::function<void(PrinterDiscoveryBackendStatus&)>& update)
{
    std::lock_guard<std::mutex> lock(m_discovery_mutex);
    if (!m_shutdown && m_discovery_status.request_id == generation)
        update(m_discovery_status);
}

HonorForgePrinterAgent::ProbeResult HonorForgePrinterAgent::probe_candidate(
    const PrinterDiscoveryCandidate& candidate, const std::function<bool()>& cancelled) const
{
    ProbeResult result;
    result.candidate = candidate;
    if (cancelled && cancelled()) {
        result.error = ProbeError::Cancelled;
        result.error_code = "cancelled";
        return result;
    }

    if (!tcp_port_reachable(candidate.host, candidate.port, cancelled)) {
        result.error = cancelled && cancelled() ? ProbeError::Cancelled : ProbeError::NetworkFailure;
        result.error_code = result.error == ProbeError::Cancelled ? "cancelled" : "tcp_unreachable";
        return result;
    }

    const std::string port = candidate.port == 0 ? std::string() : std::to_string(candidate.port);
    const std::string base_url = normalize_base_url(candidate.use_ssl, candidate.host, port);
    MoonrakerDeviceInfo info;
    std::string error;
    if (!fetch_device_info_with_timeout(base_url, {}, info, error, cancelled, 1, 2, true)) {
        result.error_code = error;
        if (cancelled && cancelled())
            result.error = ProbeError::Cancelled;
        else if (boost::icontains(error, "HTTP 401") || boost::icontains(error, "HTTP 403") ||
                 boost::icontains(error, "HTTP error: 401") || boost::icontains(error, "HTTP error: 403"))
            result.error = ProbeError::AuthenticationRequired;
        else if (boost::icontains(error, "Protocol mismatch") || boost::icontains(error, "Invalid JSON"))
            result.error = ProbeError::ProtocolMismatch;
        else if (boost::icontains(error, "timeout") || boost::icontains(error, "timed out"))
            result.error = ProbeError::Timeout;
        else
            result.error = ProbeError::NetworkFailure;
        return result;
    }

    result.dev_name = info.dev_name;
    if (candidate.source == PrinterDiscoverySource::LocalVirtualPrinter &&
        (candidate.instance_uuid.empty() || candidate.instance_uuid != info.virtual_printer_uuid)) {
        result.error = ProbeError::ProtocolMismatch;
        result.error_code = "local_virtual_printer_identity_mismatch";
        return result;
    }
    result.virtual_printer_uuid = info.virtual_printer_uuid;
    result.name_source = info.name_source;
    if (result.dev_name.empty() && candidate.source == PrinterDiscoverySource::Mdns && !candidate.service_name.empty()) {
        result.dev_name = candidate.service_name;
        result.name_source = "mdns.service_name";
    }
    if (result.dev_name.empty() && candidate.source == PrinterDiscoverySource::Mdns && !candidate.hostname.empty()) {
        result.dev_name = candidate.hostname;
        if (boost::iends_with(result.dev_name, ".local"))
            result.dev_name.resize(result.dev_name.size() - 6);
        result.name_source = "mdns.hostname";
    }
    if (result.dev_name.empty()) {
        result.dev_name = "Honor Forge \xC2\xB7 " + candidate.host;
        result.name_source = "product_ip_fallback";
    }
    result.version = info.version;
    result.klippy_state = info.klippy_state;
    return result;
}

HonorForgePrinterAgent::ProbeBatchResult HonorForgePrinterAgent::probe_candidates(
    const PrinterDiscoveryRequest& request,
    const std::vector<PrinterDiscoveryCandidate>& candidates,
    const std::function<bool()>& cancelled) const
{
    ProbeBatchResult batch;
    if (candidates.empty())
        return batch;

    BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] probe_batch_started request=" << request.request_id
                            << " agent=" << request.agent_id
                            << " source=" << printer_discovery_source_name(candidates.front().source)
                            << " candidate_count=" << candidates.size()
                            << " parallelism=" << std::min(request.max_parallel_probes, candidates.size());

    std::atomic<size_t> next {0};
    std::mutex results_mutex;
    size_t failure_samples = 0;
    const size_t thread_count = std::max<size_t>(1, std::min(request.max_parallel_probes, candidates.size()));
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (size_t worker = 0; worker < thread_count; ++worker) {
        workers.emplace_back([&] {
            while (!(cancelled && cancelled())) {
                const size_t index = next.fetch_add(1);
                if (index >= candidates.size())
                    return;
                ProbeResult result = probe_candidate(candidates[index], cancelled);
                std::lock_guard<std::mutex> lock(results_mutex);
                if (result.error == ProbeError::None) {
                    BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] probe_succeeded request=" << request.request_id
                                            << " agent=" << request.agent_id
                                            << " source=" << printer_discovery_source_name(result.candidate.source)
                                            << " host=" << result.candidate.host << " port=" << result.candidate.port;
                    batch.validated.push_back(std::move(result));
                    continue;
                }
                switch (result.error) {
                case ProbeError::AuthenticationRequired: ++batch.authentication_failures; break;
                case ProbeError::ProtocolMismatch: ++batch.protocol_failures; break;
                case ProbeError::Timeout: ++batch.timeout_failures; break;
                case ProbeError::NetworkFailure: ++batch.network_failures; break;
                default: break;
                }
                if (failure_samples < 8 && result.error != ProbeError::Cancelled) {
                    ++failure_samples;
                    BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] probe_failed_sample request=" << request.request_id
                                            << " agent=" << request.agent_id
                                            << " source=" << printer_discovery_source_name(result.candidate.source)
                                            << " host=" << result.candidate.host << " port=" << result.candidate.port
                                            << " error_stage=server_info error_code=" << probe_error_name(result.error);
                }
            }
        });
    }
    for (auto& worker : workers)
        worker.join();

    BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] probe_batch_finished request=" << request.request_id
                            << " agent=" << request.agent_id
                            << " validated_count=" << batch.validated.size()
                            << " auth_failures=" << batch.authentication_failures
                            << " protocol_failures=" << batch.protocol_failures
                            << " network_failures=" << batch.network_failures
                            << " timeout_failures=" << batch.timeout_failures;
    return batch;
}

void HonorForgePrinterAgent::run_discovery(uint64_t generation)
{
    const auto started = std::chrono::steady_clock::now();
    const auto superseded = [this, generation] {
        return m_generation.load() != generation;
    };

    PrinterDiscoveryRequest request;
    request.request_id = generation;
    request.agent_id = HONOR_FORGE_PRINTER_AGENT_ID;
    request.mdns_services = {"moonraker", "wondermaker", "klipper", "octoprint", "http"};
    request.probe_ports = {7125, 80};
    request.deadline = started + m_discovery_budget;
    const auto cancelled = [&] {
        return superseded() || std::chrono::steady_clock::now() >= request.deadline;
    };

    BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] backend_start request=" << generation
                            << " agent=" << request.agent_id
                            << " budget_ms=" << m_discovery_budget.count();

    std::vector<PrinterNetworkInterface> interfaces;
    try {
        if (m_interface_enumerator)
            interfaces = m_interface_enumerator();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[PrinterDiscovery] interface_enumeration_failed request=" << generation
                                 << " agent=" << request.agent_id << " error=" << e.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "[PrinterDiscovery] interface_enumeration_failed request=" << generation
                                 << " agent=" << request.agent_id << " error=unknown";
    }
    for (const auto& network_interface : interfaces) {
        BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] interface_selected request=" << generation
                                << " agent=" << request.agent_id << " interface=" << network_interface.name
                                << " local_ip=" << network_interface.address << " prefix=" << network_interface.prefix_length;
    }

    size_t total_candidates = 0;
    size_t total_validated = 0;
    size_t total_emitted = 0;
    size_t auth_failures = 0;
    size_t protocol_failures = 0;
    size_t network_failures = 0;
    size_t timeout_failures = 0;
    bool provider_error = false;
    std::set<std::string> emitted_identities;

    for (const auto& provider : m_providers) {
        if (!provider || cancelled())
            break;
        const auto provider_result = provider->discover(request, interfaces, cancelled);
        if (provider_result.error == PrinterDiscoveryProviderError::InternalError)
            provider_error = true;
        total_candidates += provider_result.candidates.size();
        set_status(generation, [&](PrinterDiscoveryBackendStatus& status) {
            status.candidate_count = total_candidates;
            status.elapsed_ms = elapsed_ms(started);
        });

        ProbeBatchResult batch = probe_candidates(request, provider_result.candidates, cancelled);
        auth_failures += batch.authentication_failures;
        protocol_failures += batch.protocol_failures;
        network_failures += batch.network_failures;
        timeout_failures += batch.timeout_failures;
        total_validated += batch.validated.size();

        std::map<std::string, ProbeResult> physical_devices;
        std::map<std::string, size_t> endpoint_counts;
        for (const auto& device : batch.validated) {
            const std::string key = device.virtual_printer_uuid.empty() ? device.candidate.host :
                "virtual:" + device.virtual_printer_uuid;
            ++endpoint_counts[key];
            auto current = physical_devices.find(key);
            if (current == physical_devices.end() ||
                endpoint_preference(device.candidate.port) > endpoint_preference(current->second.candidate.port))
                physical_devices.insert_or_assign(key, device);
        }

        for (const auto& [host, device] : physical_devices) {
            if (cancelled())
                break;
            // Local provider runs first. A shared simulator seen again through mDNS keeps its loopback endpoint.
            if (emitted_identities.count(host) != 0)
                continue;
            const size_t endpoint_count = endpoint_counts[host];
            if (endpoint_count > 1) {
                BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] physical_device_grouped request=" << generation
                                        << " agent=" << request.agent_id
                                        << " host=" << host
                                        << " endpoint_count=" << endpoint_count
                                        << " preferred_port=" << device.candidate.port;
            }
            MoonrakerDeviceInfo info;
            info.dev_id = device.candidate.host + ":" + std::to_string(device.candidate.port);
            info.dev_ip = info.dev_id;
            info.base_url = normalize_base_url(device.candidate.use_ssl, device.candidate.host,
                                               std::to_string(device.candidate.port));
            info.dev_name = device.dev_name;
            info.name_source = device.name_source;
            info.model_name = "Honor Forge";
            info.model_id = "Honor Forge";
            info.version = device.version;
            info.klippy_state = device.klippy_state;
            if (emit_discovered_device(info, printer_discovery_source_name(device.candidate.source))) {
                emitted_identities.insert(host);
                ++total_emitted;
                BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] device_emitted request=" << generation
                                        << " agent=" << request.agent_id
                                        << " source=" << printer_discovery_source_name(device.candidate.source)
                                        << " name_source=" << (info.name_source.empty() ? "unavailable" : info.name_source)
                                        << " device_ref=" << discovery_device_ref(info.dev_id)
                                        << " host=" << device.candidate.host << " port=" << device.candidate.port;
            }
        }
        set_status(generation, [&](PrinterDiscoveryBackendStatus& status) {
            status.validated_count = total_validated;
            status.emitted_count = total_emitted;
            status.elapsed_ms = elapsed_ms(started);
        });

        // A validated mDNS result is authoritative; subnet probing is only the fallback.
        if (total_validated > 0 && provider->source() != PrinterDiscoverySource::LocalVirtualPrinter)
            break;
    }

    if (superseded()) {
        BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] request_cancelled request=" << generation
                                << " agent=" << request.agent_id << " reason=superseded";
        return;
    }

    PrinterDiscoveryBackendResult final_result = PrinterDiscoveryBackendResult::Empty;
    PrinterDiscoveryBackendPhase final_phase = PrinterDiscoveryBackendPhase::Completed;
    if (total_emitted > 0)
        final_result = PrinterDiscoveryBackendResult::DevicesFound;
    else if (interfaces.empty()) {
        final_result = PrinterDiscoveryBackendResult::NoEligibleInterface;
        final_phase = PrinterDiscoveryBackendPhase::Failed;
    } else if (std::chrono::steady_clock::now() >= request.deadline || timeout_failures > 0)
        final_result = PrinterDiscoveryBackendResult::Timeout;
    else if (auth_failures > 0)
        final_result = PrinterDiscoveryBackendResult::AuthenticationRequired;
    else if (protocol_failures > 0)
        final_result = PrinterDiscoveryBackendResult::ProtocolMismatch;
    else if (network_failures > 0)
        final_result = PrinterDiscoveryBackendResult::NetworkFailure;
    else if (provider_error) {
        final_result = PrinterDiscoveryBackendResult::InternalError;
        final_phase = PrinterDiscoveryBackendPhase::Failed;
    }

    set_status(generation, [&](PrinterDiscoveryBackendStatus& status) {
        status.phase = final_phase;
        status.result = final_result;
        status.candidate_count = total_candidates;
        status.validated_count = total_validated;
        status.emitted_count = total_emitted;
        status.elapsed_ms = elapsed_ms(started);
    });
    if (total_emitted > 0) {
        BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] backend_finished request=" << generation
                                << " agent=" << request.agent_id
                                << " result=" << static_cast<int>(final_result)
                                << " candidate_count=" << total_candidates
                                << " validated_count=" << total_validated
                                << " emitted_count=" << total_emitted
                                << " elapsed_ms=" << elapsed_ms(started);
    } else {
        BOOST_LOG_TRIVIAL(warning) << "[PrinterDiscovery] backend_finished request=" << generation
                                   << " agent=" << request.agent_id
                                   << " result=" << static_cast<int>(final_result)
                                   << " candidate_count=" << total_candidates
                                   << " validated_count=" << total_validated
                                   << " emitted_count=" << total_emitted
                                   << " elapsed_ms=" << elapsed_ms(started);
    }
}

} // namespace Slic3r
