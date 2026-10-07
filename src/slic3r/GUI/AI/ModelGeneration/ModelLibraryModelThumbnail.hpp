#pragma once

#include "../Orca/SoftwareViewportExport.hpp"
#include <boost/filesystem/fstream.hpp>
#include <boost/uuid/detail/sha1.hpp>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>
#include <wx/image.h>
#include <wx/imagpng.h>
#include <wx/mstream.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Slic3r::GUI {

// Derived display cache only. Original model/history/image records are never
// rewritten. Full content SHA is captured on the loader worker; listing checks
// the path and high-resolution file stamp rather than hashing every mesh.
struct LibraryModelThumbnailSource {
    std::string sha256;
    uintmax_t bytes {0};
    int64_t modified {0};
};
inline bool library_model_thumbnail_sha(const std::string& sha)
{
    return sha.size() == 64 && std::all_of(sha.begin(), sha.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
inline bool library_model_thumbnail_matches(const boost::filesystem::path& model,
                                            const LibraryModelThumbnailSource& source)
{
    std::error_code error;
    const std::filesystem::path path(model.native());
    const auto bytes = std::filesystem::file_size(path, error);
    if (error || bytes != source.bytes) return false;
    const auto modified = std::filesystem::last_write_time(path, error);
    return !error && modified.time_since_epoch().count() == source.modified &&
           library_model_thumbnail_sha(source.sha256);
}
inline boost::filesystem::path library_model_thumbnail_manifest(
    const boost::filesystem::path& root, const boost::filesystem::path& model)
{
    if (root.empty() || model.empty()) return {};
    std::error_code error;
    const auto relative = std::filesystem::relative(std::filesystem::path(model.native()),
                                                    std::filesystem::path(root.native()), error);
    if (error || relative.empty() || relative.is_absolute()) return {};
    for (const auto& part : relative) if (part == ".." || part == ".") return {};
    auto path = root / "model-thumbnails" / "by-path" / boost::filesystem::path(relative.native());
    path += ".json";
    return path;
}
// Asset path matters even for equal model bytes: saved display metadata can
// differ. SHA1 here is only a compact path key; model identity remains SHA256.
inline boost::filesystem::path library_model_thumbnail_png(const boost::filesystem::path& root,
    const boost::filesystem::path& model, const std::string& sha)
{
    const auto manifest = library_model_thumbnail_manifest(root, model);
    if (manifest.empty() || !library_model_thumbnail_sha(sha)) return {};
    const std::string key = manifest.lexically_relative(root / "model-thumbnails" / "by-path").generic_string();
    boost::uuids::detail::sha1 checksum;
    checksum.process_bytes(key.data(), key.size());
    boost::uuids::detail::sha1::digest_type digest;
    checksum.get_digest(digest);
    std::ostringstream encoded; encoded << std::hex << std::setfill('0');
    for (const auto part : digest) encoded << std::setw(8) << part;
    return root / "model-thumbnails" / "png" / (encoded.str() + "-" + sha + ".png");
}
inline boost::filesystem::path library_model_thumbnail_image(
    const boost::filesystem::path& root, const boost::filesystem::path& model)
{
    const auto manifest = library_model_thumbnail_manifest(root, model);
    if (manifest.empty()) return {};
    boost::system::error_code error;
    const auto bytes = boost::filesystem::file_size(manifest, error);
    if (error || bytes == 0 || bytes > 4096) return {};
    boost::filesystem::ifstream input(manifest, std::ios::binary);
    const auto record = nlohmann::json::parse(input, nullptr, false);
    if (!record.is_object() || !record.contains("source_sha256") || !record["source_sha256"].is_string() ||
        !record.contains("source_bytes") || !record["source_bytes"].is_number_unsigned() ||
        !record.contains("source_modified") || !record["source_modified"].is_number_integer()) return {};
    LibraryModelThumbnailSource source {record["source_sha256"].get<std::string>(),
        record["source_bytes"].get<uintmax_t>(), record["source_modified"].get<int64_t>()};
    if (!library_model_thumbnail_matches(model, source)) return {};
    auto image = library_model_thumbnail_png(root, model, source.sha256);
    if (!record.contains("image_bytes") || !record["image_bytes"].is_number_unsigned() ||
        !record.contains("image_modified") || !record["image_modified"].is_number_integer()) return {};
    // A derived image can be removed, truncated or replaced independently of
    // its model. Check its high-resolution stamp without decoding every card.
    const std::filesystem::path image_path(image.native());
    std::error_code image_error;
    const auto image_bytes = std::filesystem::file_size(image_path, image_error);
    if (image_error || image_bytes != record["image_bytes"].get<uintmax_t>()) return {};
    const auto image_modified = std::filesystem::last_write_time(image_path, image_error);
    return !image_error && image_modified.time_since_epoch().count() == record["image_modified"].get<int64_t>()
        ? image : boost::filesystem::path();
}
inline void publish_library_model_thumbnail(const boost::filesystem::path& root,
    const boost::filesystem::path& model, const LibraryModelThumbnailSource& source,
    int width, int height, const std::vector<unsigned char>& bottom_up_rgb)
{
    const auto manifest = library_model_thumbnail_manifest(root, model);
    if (manifest.empty() || !library_model_thumbnail_matches(model, source) ||
        width <= 0 || height <= 0 || width > 1024 || height > 1024 ||
        bottom_up_rgb.size() != size_t(width) * size_t(height) * 3)
        throw std::runtime_error("Invalid model thumbnail source or pixels.");
    wxImage image(width, height);
    if (!image.IsOk()) throw std::runtime_error("Unable to allocate model thumbnail.");
    const size_t row = size_t(width) * 3;
    for (int y = 0; y < height; ++y)
        std::copy_n(bottom_up_rgb.data() + size_t(height - 1 - y) * row, row,
                    image.GetData() + size_t(y) * row);
    wxMemoryOutputStream output;
    wxPNGHandler handler;
    if (!handler.SaveFile(&image, output, false)) throw std::runtime_error("Unable to encode model thumbnail.");
    std::vector<unsigned char> png(output.GetSize());
    output.CopyTo(png.data(), png.size());
    const auto destination = library_model_thumbnail_png(root, model, source.sha256);
    boost::filesystem::create_directories(destination.parent_path());
    boost::filesystem::create_directories(manifest.parent_path());
    const auto image_staging = destination.parent_path() / boost::filesystem::unique_path(".model-image-%%%%-%%%%.png");
    const auto staging = manifest.parent_path() / boost::filesystem::unique_path(".model-thumb-%%%%-%%%%.tmp");
    try {
        // Only this private derived PNG may be refreshed. The existing export
        // writer still refuses to overwrite user screenshots or model assets.
        export_software_viewport_png(image_staging, png);
        if (!library_model_thumbnail_matches(model, source))
            throw std::runtime_error("Model changed while publishing its thumbnail.");
        boost::filesystem::rename(image_staging, destination);
        std::error_code image_error;
        const auto image_modified = std::filesystem::last_write_time(std::filesystem::path(destination.native()), image_error);
        if (image_error) throw std::runtime_error("Unable to read model thumbnail identity.");
        boost::filesystem::ofstream record(staging, std::ios::binary);
        record << nlohmann::json {{"source_sha256", source.sha256}, {"source_bytes", source.bytes},
            {"source_modified", source.modified}, {"image_bytes", uintmax_t(png.size())},
            {"image_modified", static_cast<int64_t>(image_modified.time_since_epoch().count())}}.dump();
        record.close();
        if (!record) throw std::runtime_error("Unable to write model thumbnail identity.");
        if (!library_model_thumbnail_matches(model, source))
            throw std::runtime_error("Model changed while publishing its thumbnail.");
        boost::filesystem::rename(staging, manifest);
    } catch (...) {
        boost::system::error_code ignored;
        boost::filesystem::remove(image_staging, ignored);
        boost::filesystem::remove(staging, ignored);
        throw;
    }
}
} // namespace Slic3r::GUI
