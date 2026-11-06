#include "HonorForgeDiscovery.hpp"

#include "Bonjour.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace Slic3r {
namespace {

using CandidateKey = std::tuple<std::string, uint16_t, bool>;

bool cancelled_or_expired(const PrinterDiscoveryRequest& request,
                          const IPrinterDiscoveryProvider::Cancelled& cancelled)
{
    return (cancelled && cancelled()) || std::chrono::steady_clock::now() >= request.deadline;
}

void append_unique_interface(std::vector<PrinterNetworkInterface>& result, PrinterNetworkInterface value)
{
    const auto duplicate = std::find_if(result.begin(), result.end(), [&](const PrinterNetworkInterface& existing) {
        return existing.address == value.address;
    });
    if (duplicate == result.end())
        result.push_back(std::move(value));
}

void append_resolved_interfaces(std::vector<PrinterNetworkInterface>& result)
{
    try {
        boost::asio::io_context context;
        boost::asio::ip::udp::resolver resolver(context);
        boost::system::error_code error;
        const auto endpoints = resolver.resolve(boost::asio::ip::udp::v4(), boost::asio::ip::host_name(), "", error);
        if (error)
            return;
        for (const auto& endpoint : endpoints) {
            const auto address = endpoint.endpoint().address();
            if (!address.is_v4() || address.is_loopback() || address.is_unspecified())
                continue;
            append_unique_interface(result, {"hostname-resolver", address.to_string(), 24});
        }
    } catch (...) {
    }
}

class MdnsPrinterDiscoveryProvider final : public IPrinterDiscoveryProvider
{
public:
    PrinterDiscoverySource source() const override { return PrinterDiscoverySource::Mdns; }

    PrinterDiscoveryProviderResult discover(const PrinterDiscoveryRequest& request,
                                             const std::vector<PrinterNetworkInterface>&,
                                             const Cancelled& cancelled) override
    {
        struct SharedState {
            std::mutex mutex;
            std::condition_variable changed;
            std::vector<PrinterDiscoveryCandidate> candidates;
            size_t remaining {0};
            bool accepting {true};
        };

        PrinterDiscoveryProviderResult result;
        if (request.mdns_services.empty())
            return result;

        auto state = std::make_shared<SharedState>();
        state->remaining = request.mdns_services.size();
        std::vector<Bonjour::Ptr> lookups;
        lookups.reserve(request.mdns_services.size());

        try {
            for (const std::string& service : request.mdns_services) {
                if (cancelled_or_expired(request, cancelled))
                    break;
                BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] mdns_query_sent request=" << request.request_id
                                        << " agent=" << request.agent_id << " service_type=_" << service << "._tcp";
                auto lookup = Bonjour(service)
                    .set_protocol("tcp")
                    .set_timeout(1)
                    .set_retries(1)
                    .on_reply([state, request_id = request.request_id, agent = request.agent_id, service](BonjourReply&& reply) {
                        if (!reply.ip.is_v4() || reply.ip.is_loopback() || reply.port == 0)
                            return;
                        PrinterDiscoveryCandidate candidate;
                        candidate.source = PrinterDiscoverySource::Mdns;
                        candidate.host = reply.ip.to_string();
                        candidate.port = reply.port;
                        candidate.service_type = "_" + service + "._tcp";
                        candidate.service_name = reply.service_name;
                        candidate.hostname = reply.hostname;
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            if (!state->accepting)
                                return;
                            state->candidates.push_back(candidate);
                        }
                        BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] mdns_reply_received request=" << request_id
                                                << " agent=" << agent << " service_type=" << candidate.service_type
                                                << " host=" << candidate.host << " port=" << candidate.port;
                        state->changed.notify_all();
                    })
                    .on_complete([state] {
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            if (state->remaining > 0)
                                --state->remaining;
                        }
                        state->changed.notify_all();
                    })
                    .lookup();
                lookups.push_back(std::move(lookup));
            }

            std::unique_lock<std::mutex> lock(state->mutex);
            while (state->remaining > 0 && !cancelled_or_expired(request, cancelled))
                state->changed.wait_for(lock, std::chrono::milliseconds(50));
            state->accepting = false;
            result.candidates = std::move(state->candidates);
            if (cancelled && cancelled())
                result.error = PrinterDiscoveryProviderError::Cancelled;
            else if (std::chrono::steady_clock::now() >= request.deadline && state->remaining > 0)
                result.error = PrinterDiscoveryProviderError::Timeout;
        } catch (const std::exception& e) {
            result.error = PrinterDiscoveryProviderError::InternalError;
            BOOST_LOG_TRIVIAL(error) << "[PrinterDiscovery] mdns_failed request=" << request.request_id
                                     << " agent=" << request.agent_id << " error=" << e.what();
        } catch (...) {
            result.error = PrinterDiscoveryProviderError::InternalError;
            BOOST_LOG_TRIVIAL(error) << "[PrinterDiscovery] mdns_failed request=" << request.request_id
                                     << " agent=" << request.agent_id << " error=unknown";
        }

        std::set<CandidateKey> seen;
        result.candidates.erase(std::remove_if(result.candidates.begin(), result.candidates.end(), [&](const auto& candidate) {
            return !seen.emplace(candidate.host, candidate.port, candidate.use_ssl).second;
        }), result.candidates.end());
        return result;
    }
};

class SubnetPrinterDiscoveryProvider final : public IPrinterDiscoveryProvider
{
public:
    PrinterDiscoverySource source() const override { return PrinterDiscoverySource::SubnetProbe; }

    PrinterDiscoveryProviderResult discover(const PrinterDiscoveryRequest& request,
                                             const std::vector<PrinterNetworkInterface>& interfaces,
                                             const Cancelled& cancelled) override
    {
        PrinterDiscoveryProviderResult result;
        if (interfaces.empty()) {
            result.error = PrinterDiscoveryProviderError::NoEligibleInterface;
            return result;
        }

        BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] subnet_fallback_started request=" << request.request_id
                                << " agent=" << request.agent_id << " interface_count=" << interfaces.size();
        std::set<CandidateKey> seen;
        for (const auto& network_interface : interfaces) {
            if (cancelled_or_expired(request, cancelled))
                break;
            boost::system::error_code error;
            const auto address = boost::asio::ip::make_address_v4(network_interface.address, error);
            if (error)
                continue;

            const unsigned prefix = std::max(24u, std::min(network_interface.prefix_length, 32u));
            if (prefix >= 31)
                continue;
            const uint32_t address_value = address.to_uint();
            const uint32_t mask = prefix == 0 ? 0 : (0xffffffffu << (32u - prefix));
            const uint32_t network = address_value & mask;
            const uint32_t broadcast = network | ~mask;
            for (const uint16_t port : request.probe_ports) {
                size_t emitted = 0;
                for (uint32_t value = network + 1; value < broadcast && emitted < request.max_hosts_per_interface; ++value) {
                    if (cancelled_or_expired(request, cancelled))
                        break;
                    if (value == address_value)
                        continue;
                    if (port == 0 || !seen.emplace(boost::asio::ip::address_v4(value).to_string(), port, false).second)
                        continue;
                    PrinterDiscoveryCandidate candidate;
                    candidate.source = PrinterDiscoverySource::SubnetProbe;
                    candidate.interface_name = network_interface.name;
                    candidate.local_address = network_interface.address;
                    candidate.host = boost::asio::ip::address_v4(value).to_string();
                    candidate.port = port;
                    result.candidates.push_back(std::move(candidate));
                    ++emitted;
                }
            }
        }
        if (cancelled && cancelled())
            result.error = PrinterDiscoveryProviderError::Cancelled;
        else if (std::chrono::steady_clock::now() >= request.deadline)
            result.error = PrinterDiscoveryProviderError::Timeout;

        BOOST_LOG_TRIVIAL(info) << "[PrinterDiscovery] subnet_candidates_ready request=" << request.request_id
                                << " agent=" << request.agent_id << " candidate_count=" << result.candidates.size();
        return result;
    }
};

} // namespace

const char* printer_discovery_source_name(PrinterDiscoverySource source)
{
    switch (source) {
    case PrinterDiscoverySource::Mdns: return "mdns";
    case PrinterDiscoverySource::SubnetProbe: return "subnet_probe";
    }
    return "unknown";
}

std::vector<PrinterNetworkInterface> enumerate_printer_network_interfaces()
{
    std::vector<PrinterNetworkInterface> result;
#ifdef _WIN32
    ULONG size = 0;
    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size) == ERROR_BUFFER_OVERFLOW && size > 0) {
        std::vector<unsigned char> storage(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) == NO_ERROR) {
            for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
                const bool physical = adapter->IfType == IF_TYPE_IEEE80211 ||
                                      adapter->IfType == IF_TYPE_ETHERNET_CSMACD ||
                                      adapter->IfType == IF_TYPE_FASTETHER ||
                                      adapter->IfType == IF_TYPE_FASTETHER_FX ||
                                      adapter->IfType == IF_TYPE_GIGABITETHERNET;
                if (!physical || adapter->OperStatus != IfOperStatusUp)
                    continue;
                for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                    if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET)
                        continue;
                    char buffer[INET_ADDRSTRLEN] = {};
                    const auto* socket_address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                    if (!inet_ntop(AF_INET, &socket_address->sin_addr, buffer, sizeof(buffer)))
                        continue;
                    boost::system::error_code error;
                    const auto address = boost::asio::ip::make_address_v4(buffer, error);
                    if (error || address.is_loopback() || address.is_unspecified())
                        continue;
                    append_unique_interface(result, {adapter->AdapterName ? adapter->AdapterName : "windows-adapter",
                                                     address.to_string(), unicast->OnLinkPrefixLength});
                }
            }
        }
    }
#else
    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) == 0) {
        for (const ifaddrs* item = addresses; item; item = item->ifa_next) {
            if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
                !(item->ifa_flags & IFF_UP) || (item->ifa_flags & IFF_LOOPBACK))
                continue;
            char address_buffer[INET_ADDRSTRLEN] = {};
            const auto* socket_address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
            if (!inet_ntop(AF_INET, &socket_address->sin_addr, address_buffer, sizeof(address_buffer)))
                continue;
            unsigned prefix = 24;
            if (item->ifa_netmask) {
                const auto* mask_address = reinterpret_cast<const sockaddr_in*>(item->ifa_netmask);
                prefix = static_cast<unsigned>(__builtin_popcount(ntohl(mask_address->sin_addr.s_addr)));
            }
            append_unique_interface(result, {item->ifa_name ? item->ifa_name : "network-interface", address_buffer, prefix});
        }
        freeifaddrs(addresses);
    }
#endif
    if (result.empty())
        append_resolved_interfaces(result);
    return result;
}

std::shared_ptr<IPrinterDiscoveryProvider> make_mdns_printer_discovery_provider()
{
    return std::make_shared<MdnsPrinterDiscoveryProvider>();
}

std::shared_ptr<IPrinterDiscoveryProvider> make_subnet_printer_discovery_provider()
{
    return std::make_shared<SubnetPrinterDiscoveryProvider>();
}

} // namespace Slic3r
