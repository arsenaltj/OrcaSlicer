#pragma once
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <boost/filesystem.hpp>
#include <memory>
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <vector>
#ifdef _WIN32
#include <wx/msw/wrapwin.h>
#endif

namespace Slic3r::GUI {
// Worker-owned hash verification; Windows GUI commits compare leased identities.
// Windows read leases also prevent writes/replacement between validation and
// installation. Platforms without an equivalent lease retain strict digest
// revalidation at handoff, never treating size/time as content evidence.
class VerifiedModelFiles {
public:
    ~VerifiedModelFiles() {
#ifdef _WIN32
        for (const auto& file : m_files)
            if (file.handle != INVALID_HANDLE_VALUE) ::CloseHandle(file.handle);
#endif
    }
    VerifiedModelFiles() = default;
    VerifiedModelFiles(const VerifiedModelFiles&) = delete;
    VerifiedModelFiles& operator=(const VerifiedModelFiles&) = delete;
    bool verify(const boost::filesystem::path& path, const std::string& expected, bool optional = false) {
        if (m_failed) return false;
        // Any failed member invalidates the whole proof permanently.
        m_failed = true;
        File file; file.path = path;
        boost::system::error_code ec;
        if (optional && expected.empty() && absent(path)) {
            file.missing = true; m_files.push_back(std::move(file)); m_failed = false; return true;
        }
#ifdef _WIN32
        file.handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.handle == INVALID_HANDLE_VALUE) return false;
#endif
        // Retain the lease even on failure; the owning result releases it.
        m_files.push_back(std::move(file));
        auto& saved = m_files.back();
        saved.sha256 = expected;
        saved.size = boost::filesystem::file_size(path, ec); if (ec) return false;
        saved.modified = boost::filesystem::last_write_time(path, ec); if (ec) return false;
        if (expected.empty() || AI::model_artifact_sha256(path) != expected) return false;
        m_failed = !unchanged(saved);
        return !m_failed;
    }
    bool current() const {
        if (m_failed || m_files.empty()) return false;
        return std::all_of(m_files.begin(), m_files.end(), [](const File& file) {
            if (!unchanged(file)) return false;
#ifndef _WIN32
            if (!file.missing) {
                try { return AI::model_artifact_sha256(file.path) == file.sha256 && unchanged(file); }
                catch (...) { return false; }
            }
#endif
            return true;
        });
    }
private:
    struct File {
        boost::filesystem::path path;
        std::string sha256;
        std::uintmax_t size {0}; std::time_t modified {0}; bool missing {false};
#ifdef _WIN32
        HANDLE handle {INVALID_HANDLE_VALUE};
#endif
    };
    static bool unchanged(const File& file) {
        boost::system::error_code ec;
        if (file.missing) return absent(file.path);
        const auto size = boost::filesystem::file_size(file.path, ec); if (ec || size != file.size) return false;
        return boost::filesystem::last_write_time(file.path, ec) == file.modified && !ec;
    }
    static bool absent(const boost::filesystem::path& path) {
        boost::system::error_code ec;
        const auto status = boost::filesystem::symlink_status(path, ec);
        // Boost/Windows may retain ENOENT for a file_not_found status. Other
        // errors and dangling symlinks must never count as a missing companion.
        return status.type() == boost::filesystem::file_not_found &&
            (!ec || ec == boost::system::errc::no_such_file_or_directory);
    }
    std::vector<File> m_files;
    bool m_failed {false};
};
} // namespace Slic3r::GUI
