#pragma once

#include "ModelLibraryEntry.hpp"
#include "ModelGenerationHost.hpp"
#include <boost/filesystem.hpp>

namespace Slic3r::GUI {
inline std::string workbench_history_key(const ModelLibraryEntry& entry)
{
    // A task can have multiple saved versions. Neither title nor RGB identifies one.
    return std::to_string(entry.job_id.size()) + ":" + entry.job_id + entry.model_path.generic_string();
}

inline std::string workbench_thumbnail_source(const ModelLibraryEntry& entry, int edge)
{
    const auto stamp = [](const boost::filesystem::path& path) {
        boost::system::error_code ec;
        const auto bytes = boost::filesystem::file_size(path, ec);
        const auto size = ec ? std::string("missing") : std::to_string(bytes);
        const auto time = boost::filesystem::last_write_time(path, ec);
        const auto name = path.generic_string();
        return std::to_string(name.size()) + ":" + name + ":" + size + ":" +
            (ec ? "missing" : std::to_string(time));
    };
    auto display = entry.ai_image_path;
    if (!display.empty()) display += ".display.png";
    return stamp(entry.ai_image_path) + "|" + stamp(display) + "|" +
        stamp(entry.reference_image_path) + "|" + std::to_string(edge);
}

inline bool retain_workbench_thumbnail(ModelGenerationUIHistoryEntry& target,
                                       const ModelGenerationUIHistoryEntry& previous)
{
    if (target.asset_key != previous.asset_key || target.thumbnail_source != previous.thumbnail_source ||
        !previous.thumbnail_ready) return false;
    target.thumbnail_width = previous.thumbnail_width;
    target.thumbnail_height = previous.thumbnail_height;
    target.thumbnail_rgb = previous.thumbnail_rgb;
    target.thumbnail_alpha = previous.thumbnail_alpha;
    target.thumbnail_ready = true;
    target.thumbnail_revision = previous.thumbnail_revision;
    return true;
}
} // namespace Slic3r::GUI
