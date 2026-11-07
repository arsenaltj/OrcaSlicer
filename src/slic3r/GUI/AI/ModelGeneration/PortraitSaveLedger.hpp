#pragma once
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <functional>
#include <exception>

namespace Slic3r::GUI::PortraitPrint {
// Publish complete bytes without replacing an existing version. A failed copy
// can only leave a private temporary file, which is removed before returning.
inline void publish_color_version(const boost::filesystem::path& source,
    const boost::filesystem::path& destination,
    const std::function<void(const boost::filesystem::path&, const boost::filesystem::path&)>& copy =
        [](const auto& from, const auto& to) { boost::filesystem::copy_file(from, to); })
{
    const auto temporary = destination.parent_path() /
        boost::filesystem::unique_path("portrait-version-%%%%-%%%%-%%%%.tmp");
    try {
        copy(source, temporary);
        boost::filesystem::create_hard_link(temporary, destination);
    } catch (...) {
        boost::system::error_code ignored;
        boost::filesystem::remove(temporary, ignored);
        throw;
    }
    boost::system::error_code ignored;
    boost::filesystem::remove(temporary, ignored);
}

// Keep byte-exact backups until both durable flow records have been replaced.
// The writer is injected so each commit boundary can be tested independently.
inline void commit_save_ledger(const std::array<boost::filesystem::path, 2>& paths,
    const nlohmann::json& document,
    const std::function<void(const boost::filesystem::path&, const nlohmann::json&)>& write)
{
    std::array<boost::filesystem::path, 2> backups;
    std::array<bool, 2> existed {}, prepared {};
    try {
        for (size_t i = 0; i < paths.size(); ++i) {
            existed[i] = boost::filesystem::exists(paths[i]);
            if (existed[i]) {
                backups[i] = paths[i].parent_path() / boost::filesystem::unique_path("portrait-ledger-%%%%-%%%%.rollback");
                boost::filesystem::copy_file(paths[i], backups[i]);
            }
            prepared[i] = true;
        }
        for (const auto& path : paths) write(path, document);
    } catch (...) {
        const auto failure = std::current_exception();
        for (size_t i = 0; i < paths.size(); ++i) if (prepared[i]) {
            if (existed[i]) {
                boost::filesystem::copy_file(backups[i], paths[i], boost::filesystem::copy_option::overwrite_if_exists);
            } else {
                boost::filesystem::remove(paths[i]);
            }
        }
        for (const auto& backup : backups) if (!backup.empty()) {
            boost::system::error_code ignored; boost::filesystem::remove(backup, ignored);
        }
        std::rethrow_exception(failure);
    }
    for (const auto& backup : backups) if (!backup.empty()) {
        boost::system::error_code ignored; boost::filesystem::remove(backup, ignored);
    }
}
}
