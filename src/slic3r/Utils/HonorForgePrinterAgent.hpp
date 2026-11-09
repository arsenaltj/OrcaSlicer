#ifndef __HONOR_FORGE_PRINTER_AGENT_HPP__
#define __HONOR_FORGE_PRINTER_AGENT_HPP__

#include "HonorForgeDiscovery.hpp"
#include "MoonrakerPrinterAgent.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {

class HonorForgePrinterAgent : public MoonrakerPrinterAgent
{
public:
    using InterfaceEnumerator = std::function<std::vector<PrinterNetworkInterface>()>;

    explicit HonorForgePrinterAgent(std::string log_dir);
    HonorForgePrinterAgent(std::string log_dir,
                           std::vector<std::shared_ptr<IPrinterDiscoveryProvider>> providers,
                           InterfaceEnumerator interface_enumerator,
                           std::chrono::milliseconds discovery_budget = std::chrono::seconds(9));
    ~HonorForgePrinterAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo get_agent_info() override { return get_agent_info_static(); }
    PrinterAgentTraits get_traits() const override { return get_agent_info_static().traits; }

    bool start_discovery(bool start, bool sending) override;
    PrinterDiscoveryBackendStatus discovery_status() const override;

    enum class ProbeError {
        None = 0,
        AuthenticationRequired,
        ProtocolMismatch,
        NetworkFailure,
        Timeout,
        Cancelled
    };

    struct ProbeResult {
        PrinterDiscoveryCandidate candidate;
        ProbeError error {ProbeError::None};
        std::string error_code;
        std::string dev_name;
        std::string name_source;
        std::string version;
        std::string klippy_state;
    };

protected:
    virtual ProbeResult probe_candidate(const PrinterDiscoveryCandidate& candidate,
                                        const std::function<bool()>& cancelled) const;

private:
    struct ProbeBatchResult {
        std::vector<ProbeResult> validated;
        size_t authentication_failures {0};
        size_t protocol_failures {0};
        size_t network_failures {0};
        size_t timeout_failures {0};
    };

    void worker_loop();
    void run_discovery(uint64_t generation);
    ProbeBatchResult probe_candidates(const PrinterDiscoveryRequest& request,
                                      const std::vector<PrinterDiscoveryCandidate>& candidates,
                                      const std::function<bool()>& cancelled) const;
    void set_status(uint64_t generation, const std::function<void(PrinterDiscoveryBackendStatus&)>& update);

    std::vector<std::shared_ptr<IPrinterDiscoveryProvider>> m_providers;
    InterfaceEnumerator m_interface_enumerator;
    std::chrono::milliseconds m_discovery_budget;

    mutable std::mutex m_discovery_mutex;
    std::condition_variable m_discovery_changed;
    PrinterDiscoveryBackendStatus m_discovery_status;
    bool m_request_pending {false};
    bool m_shutdown {false};
    std::atomic<uint64_t> m_generation {0};
    std::thread m_discovery_thread;
};

} // namespace Slic3r

#endif
