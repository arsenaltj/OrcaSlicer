#pragma once

#include <atomic>
#include <string>
#include <boost/filesystem/path.hpp>
#include "slic3r/GUI/AI/Model/LocalSemanticEvidence.hpp"

namespace Slic3r::GUI::LocalSemanticWorker {

struct Configuration {
    bool enabled = false;
    boost::filesystem::path python_executable;
    boost::filesystem::path weights_directory;
    unsigned cpu_threads = 4;
    unsigned timeout_seconds = 120;
    unsigned long long cache_bytes = 1024ULL * 1024 * 1024;
};

// Paths come only from the application's explicit data-directory configuration,
// never a model-adjacent document. Failure leaves the destination unchanged.
bool read_configuration(const boost::filesystem::path& file, Configuration& destination, std::string& reason);
// Explicit user configuration wins, including disabled/invalid configuration.
// Otherwise use only the fixed runtime shipped beside application resources.
bool read_runtime_configuration(const boost::filesystem::path& file,
    const boost::filesystem::path& installed_runtime, Configuration& destination, std::string& reason);

// Remove only a caller-owned immediate child of the disposable request parent.
// Call after the owned worker has stopped. Already removed is successful.
bool cleanup_request(const boost::filesystem::path& owned_request,
                     const boost::filesystem::path& request_parent, std::string& reason);

enum class Status { Ready, Disabled, Unavailable, Cancelled, TimedOut };
struct Result {
    Status status = Status::Unavailable;
    std::string reason;
    std::string response_json;
    int exit_code = -1;
    boost::filesystem::path request_directory;
};

// Synchronous and cancellable, for a worker thread. It never calls wx or retains
// an owner pointer; the GUI must check its lifetime and request generation before
// consuming the result. Only this owned process tree is terminated. No provider keys or
// Python/HTTP proxy environment is inherited. The caller chooses installed_script.
Result probe(const Configuration& config, const boost::filesystem::path& installed_script,
             const boost::filesystem::path& request_root, const std::atomic<bool>& cancelled);

// Local diagnostics contain a fixed operation name and native error number,
// never filesystem paths, exception text, model contents or credentials.
struct CacheWriteReport {
    bool stored = false;
    std::string operation = "not_attempted";
    int native_error = 0; // Last observed error; may be nonzero after a recovered retry.
    unsigned publish_attempts = 0;
};

struct MeshResult {
    Result process;
    LocalSemanticEvidence::Evidence evidence;
    bool cache_hit = false;
    CacheWriteReport cache_write;
};

// Local analysis, independent of paid generation jobs. The native mesh
// must be the actual source import, not an echoed/reconstructed worker mesh.
// Ready is returned only after ordered native/render proof and evidence decode.
// An explicit application-owned cache root enables bounded successful-result
// reuse; empty root or cache_bytes=0 disables it. Cache hits repeat all proofs
// against the current native mesh and retain the original successful request ID.
MeshResult analyze(const Configuration& config, const boost::filesystem::path& installed_directory,
                   const boost::filesystem::path& request_root, const boost::filesystem::path& source,
                   const indexed_triangle_set& native_mesh, const std::atomic<bool>& cancelled,
                   const boost::filesystem::path& cache_root = {});

} // namespace Slic3r::GUI::LocalSemanticWorker
