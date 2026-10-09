#pragma once

#include "PortraitResidualProposal.hpp"
#include "LocalSemanticWorkerClient.hpp"
#include <atomic>
#include <boost/filesystem/path.hpp>

namespace Slic3r::GUI::PortraitResidual {

enum class ClientStatus { Ready, Disabled, Unavailable, Cancelled, TimedOut };
struct ClientResult {
    ClientStatus status {ClientStatus::Unavailable};
    std::string reason;
    Document document;
    boost::filesystem::path request_directory;
    bool cache_hit {false};
};

// Runs only the installed offline proposal script.  It never inherits proxy
// or provider credentials and treats every malformed/stale result as R9.
ClientResult run(const LocalSemanticWorker::Configuration& config,
                 const boost::filesystem::path& installed_script,
                 const boost::filesystem::path& request_root,
                 const boost::filesystem::path& cache_root,
                 const nlohmann::json& request,
                 const std::atomic<bool>& cancelled);

} // namespace Slic3r::GUI::PortraitResidual
