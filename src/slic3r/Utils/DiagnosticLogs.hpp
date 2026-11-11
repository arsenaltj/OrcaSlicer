#pragma once
#include <boost/filesystem/path.hpp>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r::Diagnostics {
struct LogExportResult {
    bool success {false},cancelled {false},empty {false};
    std::vector<std::string> files,omitted;
    std::string error;
};
std::string redact_log_text(const std::string&);
// Reads only the current data directory's log subtree, never models, profiles or accounts.
LogExportResult export_logs(const boost::filesystem::path& data,const boost::filesystem::path& target,
    const std::string& version,const std::atomic_bool& cancel,
    std::function<void(const std::string&)> progress={});
}
