#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <boost/filesystem.hpp>

namespace Slic3r::GUI::LocalSemanticCachePublication {
enum class Status { Stored, Cancelled, InvalidOwnership, Failed };
struct Result {
    Status status {Status::Failed};
    int native_error {0}; // Last encountered error, including one recovered by retry.
    unsigned attempts {0};
};

// Caller holds its cache lock and supplies the full cache ownership check.
// Revalidate on every attempt and never replace an unexpected destination.
// No inference, permission changes, fallback copy or cross-directory move.
inline Result publish(const boost::filesystem::path& pending, const boost::filesystem::path& destination,
    const std::atomic<bool>& cancelled, const std::function<bool()>& owned)
{
    namespace fs = boost::filesystem;
    Result result;
    for(unsigned attempt=0;attempt<20;++attempt) {
        if(cancelled.load()) {result.status=Status::Cancelled;return result;}
        try {
            if(!pending.is_absolute() || !destination.is_absolute() || pending.parent_path()!=destination.parent_path() ||
                pending==destination || !owned || !owned() || fs::exists(destination)) {
                result.status=Status::InvalidOwnership;return result;
            }
            ++result.attempts;
            boost::system::error_code error;
            fs::rename(pending,destination,error);
            if(!error) {result.status=Status::Stored;return result;}
            result.native_error=error.value();
#ifdef _WIN32
            // ERROR_ACCESS_DENIED / ERROR_SHARING_VIOLATION / ERROR_LOCK_VIOLATION.
            // These numbers are Win32 ABI values; no Windows header is needed.
            const bool retry=error.value()==5 || error.value()==32 || error.value()==33;
#else
            const bool retry=false;
#endif
            if(!retry || attempt==19) return result;
        } catch(const boost::system::system_error& error) {
            result.native_error=error.code().value();return result;
        } catch(...) {return result;}
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return result;
}
} // namespace Slic3r::GUI::LocalSemanticCachePublication
