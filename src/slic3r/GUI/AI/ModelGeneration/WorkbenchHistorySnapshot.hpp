#pragma once

#include "ModelLibraryEntry.hpp"
#include "ModelLibraryModelThumbnail.hpp"
#include "ModelGenerationHost.hpp"
#include <boost/filesystem.hpp>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>

namespace Slic3r::GUI {
inline std::string workbench_history_key(const ModelLibraryEntry& entry)
{
    // A task can have multiple saved versions. Neither title nor RGB identifies one.
    return std::to_string(entry.job_id.size()) + ":" + entry.job_id + entry.model_path.generic_string();
}

inline std::optional<std::string> workbench_history_fallback_key(
    const std::vector<std::string>& ordered_keys, const std::string& removed_key)
{
    const auto found = std::find(ordered_keys.begin(), ordered_keys.end(), removed_key);
    if (found == ordered_keys.end()) return std::nullopt;
    const auto next = std::next(found);
    if (next != ordered_keys.end()) return *next;
    if (found != ordered_keys.begin()) return *std::prev(found);
    return std::nullopt;
}

inline std::vector<std::string> workbench_history_fallback_keys(
    const std::vector<std::string>& ordered_keys, const std::string& removed_key)
{
    const auto found = std::find(ordered_keys.begin(), ordered_keys.end(), removed_key);
    if (found == ordered_keys.end()) return {};
    std::vector<std::string> result(std::next(found), ordered_keys.end());
    for (auto before = found; before != ordered_keys.begin();) result.push_back(*--before);
    return result;
}

inline std::vector<const ModelLibraryEntry*> filtered_workbench_history_entries(
    const std::vector<ModelLibraryEntry>& library, const wxString& query, int filter)
{
    std::vector<const ModelLibraryEntry*> entries;
    for (const auto& entry : library) {
        if (entry.design_only || entry.model_path.empty() ||
            (!query.empty() && !entry.title.Lower().Contains(query))) continue;
        if ((filter == 1 && entry.palette.size() != 1) ||
            (filter == 2 && entry.palette.size() < 2) || (filter == 3 && !entry.palette.empty())) continue;
        entries.push_back(&entry);
    }
    return entries;
}

inline std::string workbench_thumbnail_source(const ModelLibraryEntry& entry,
                                              const boost::filesystem::path& root, int edge)
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
    const auto manifest = library_model_thumbnail_manifest(root, entry.model_path);
    const auto model_thumbnail = library_model_thumbnail_image(root, entry.model_path);
    return stamp(entry.model_path) + "|" + stamp(manifest) + "|" +
        stamp(legacy_library_model_thumbnail_manifest(root, entry.model_path)) + "|" + stamp(model_thumbnail) + "|" +
        stamp(entry.ai_image_path) + "|" + stamp(display) + "|" +
        stamp(entry.reference_image_path) + "|" + std::to_string(edge);
}

inline wxSize fit_workbench_thumbnail_size(int source_width, int source_height,
                                           int target_width, int target_height)
{
    if (source_width <= 0 || source_height <= 0 || target_width <= 0 || target_height <= 0)
        return {};
    const double scale = std::min(double(target_width) / source_width,
                                  double(target_height) / source_height);
    return {std::max(1, int(std::lround(source_width * scale))),
            std::max(1, int(std::lround(source_height * scale)))};
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
