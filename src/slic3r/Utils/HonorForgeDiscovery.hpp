#ifndef slic3r_HonorForgeDiscovery_hpp_
#define slic3r_HonorForgeDiscovery_hpp_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

enum class PrinterDiscoverySource {
    Mdns = 0,
    SubnetProbe,
    LocalVirtualPrinter
};

const char* printer_discovery_source_name(PrinterDiscoverySource source);

struct PrinterNetworkInterface
{
    std::string name;
    std::string address;
    unsigned prefix_length {24};
};

struct PrinterDiscoveryCandidate
{
    PrinterDiscoverySource source {PrinterDiscoverySource::Mdns};
    std::string interface_name;
    std::string local_address;
    std::string host;
    uint16_t port {0};
    bool use_ssl {false};
    std::string service_type;
    std::string service_name;
    std::string hostname;
    // Present only for a user-local runtime declaration; verified against /server/info.
    std::string instance_uuid;
};

enum class PrinterDiscoveryProviderError {
    None = 0,
    NoEligibleInterface,
    Timeout,
    Cancelled,
    InternalError
};

struct PrinterDiscoveryProviderResult
{
    PrinterDiscoveryProviderError error {PrinterDiscoveryProviderError::None};
    std::vector<PrinterDiscoveryCandidate> candidates;
};

struct PrinterDiscoveryRequest
{
    uint64_t request_id {0};
    std::string agent_id;
    std::vector<std::string> mdns_services;
    std::vector<uint16_t> probe_ports;
    std::chrono::steady_clock::time_point deadline;
    size_t max_hosts_per_interface {254};
    size_t max_parallel_probes {32};
};

class IPrinterDiscoveryProvider
{
public:
    using Cancelled = std::function<bool()>;

    virtual ~IPrinterDiscoveryProvider() = default;
    virtual PrinterDiscoverySource source() const = 0;
    virtual PrinterDiscoveryProviderResult discover(const PrinterDiscoveryRequest& request,
                                                     const std::vector<PrinterNetworkInterface>& interfaces,
                                                     const Cancelled& cancelled) = 0;
};

std::vector<PrinterNetworkInterface> enumerate_printer_network_interfaces();
std::shared_ptr<IPrinterDiscoveryProvider> make_mdns_printer_discovery_provider();
std::shared_ptr<IPrinterDiscoveryProvider> make_subnet_printer_discovery_provider();
std::shared_ptr<IPrinterDiscoveryProvider> make_local_virtual_printer_discovery_provider(std::string declaration_path = {});

} // namespace Slic3r

#endif
