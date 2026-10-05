#pragma once

#include "ModelLibraryThumbnail.hpp"
#include <wx/mstream.h>
#include <wx/log.h>
#include <vector>

namespace Slic3r::GUI {

enum class ImageInputError { None, Unreadable, Unsupported, ExtensionMismatch, TooLarge, TooSmall, TooManyPixels, Corrupt, Cancelled };
inline constexpr uint64_t MAX_INPUT_IMAGE_BYTES = 20 * 1024 * 1024;
inline constexpr uint64_t MAX_INPUT_IMAGE_PIXELS = 16 * 1024 * 1024;

// Read only the bounded JPEG APP1/IFD0 orientation, never follow arbitrary EXIF offsets.
inline int input_jpeg_orientation(const std::vector<unsigned char>& bytes)
{
    for (size_t pos = 2; pos + 4 <= bytes.size();) {
        if (bytes[pos++] != 0xff) break;
        while (pos < bytes.size() && bytes[pos] == 0xff) ++pos;
        if (pos >= bytes.size()) break;
        const auto marker = bytes[pos++];
        if (marker == 0xda || marker == 0xd9) break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (pos + 2 > bytes.size()) break;
        const size_t length = (size_t(bytes[pos]) << 8) | bytes[pos + 1];
        if (length < 2 || length > bytes.size() - pos) break;
        const size_t end = pos + length, base = pos + 8;
        if (marker == 0xe1 && length >= 16 &&
            std::equal(bytes.begin() + pos + 2, bytes.begin() + pos + 8, "Exif\0\0")) {
            const bool little = bytes[base] == 'I' && bytes[base + 1] == 'I';
            const bool big = bytes[base] == 'M' && bytes[base + 1] == 'M';
            if (!little && !big) return 1;
            auto word = [&](size_t p) -> uint32_t {
                if (p > end || end - p < 2) return 0;
                return little ? uint32_t(bytes[p]) | (uint32_t(bytes[p + 1]) << 8)
                              : (uint32_t(bytes[p]) << 8) | bytes[p + 1];
            };
            auto dword = [&](size_t p) -> uint32_t {
                if (p > end || end - p < 4) return 0;
                return little ? word(p) | (word(p + 2) << 16) : (word(p) << 16) | word(p + 2);
            };
            if (word(base + 2) != 42) return 1;
            const auto offset = dword(base + 4);
            if (offset < 8 || offset > end - base || end - base - offset < 2) return 1;
            const size_t table = base + offset;
            const size_t count = word(table);
            if (count > (end - table - 2) / 12) return 1;
            for (size_t i = 0; i < count; ++i) {
                const size_t entry = table + 2 + i * 12;
                if (word(entry) == 0x112 && word(entry + 2) == 3 && dword(entry + 4) == 1) {
                    const int orientation = int(word(entry + 8));
                    return orientation >= 1 && orientation <= 8 ? orientation : 1;
                }
            }
        }
        pos = end;
    }
    return 1;
}

inline wxImage orient_input_image(wxImage image, int orientation)
{
    switch (orientation) {
    case 2: return image.Mirror(true);
    case 3: return image.Rotate90().Rotate90();
    case 4: return image.Mirror(false);
    case 5: return image.Mirror(true).Rotate90(false);
    case 6: return image.Rotate90(true);
    case 7: return image.Mirror(true).Rotate90(true);
    case 8: return image.Rotate90(false);
    default: return image;
    }
}

// Transactional output: failure leaves the caller's previous image untouched.
// Only the display is oriented; the provider receives the unmodified original.
inline ImageInputError load_model_input_image(const boost::filesystem::path& path, wxImage& output)
{
    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(path, ec) || ec) return ImageInputError::Unreadable;
    const auto size = boost::filesystem::file_size(path, ec);
    if (ec || size == 0) return ImageInputError::Unreadable;
    if (size > MAX_INPUT_IMAGE_BYTES) return ImageInputError::TooLarge;
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (extension != ".png" && extension != ".jpg" && extension != ".jpeg") return ImageInputError::Unsupported;
    bool png = false;
    const auto dimensions = library_image_dimensions(path, png);
    if (dimensions.x <= 0 || dimensions.y <= 0) return ImageInputError::Corrupt;
    if (png != (extension == ".png")) return ImageInputError::ExtensionMismatch;
    if (dimensions.x < 64 || dimensions.y < 64) return ImageInputError::TooSmall;
    if (uint64_t(dimensions.x) * uint64_t(dimensions.y) > MAX_INPUT_IMAGE_PIXELS) return ImageInputError::TooManyPixels;
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    boost::filesystem::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (file.gcount() != static_cast<std::streamsize>(size)) return ImageInputError::Unreadable;
    if (!png && (size < 2 || bytes[size - 2] != 0xff || bytes[size - 1] != 0xd9)) return ImageInputError::Corrupt;
    wxMemoryInputStream stream(bytes.data(), bytes.size());
    wxPNGHandler png_handler;
    wxJPEGHandler jpeg_handler;
    wxImageHandler& handler = png ? static_cast<wxImageHandler&>(png_handler) : static_cast<wxImageHandler&>(jpeg_handler);
    wxImage candidate;
    wxLogNull quiet;
    if (!handler.LoadFile(&candidate, stream, false) || !candidate.IsOk() || candidate.GetSize() != dimensions)
        return ImageInputError::Corrupt;
    output = png ? candidate : orient_input_image(candidate, input_jpeg_orientation(bytes));
    return ImageInputError::None;
}

// A rejected file must not ask the user to leave an edited model. Cancellation
// after validation also preserves the caller's previous preview.
template<class CanReplace>
inline ImageInputError prepare_model_input_replacement(
    const boost::filesystem::path& path, wxImage& output, CanReplace can_replace)
{
    wxImage candidate;
    const auto error = load_model_input_image(path, candidate);
    if (error != ImageInputError::None) return error;
    if (!can_replace()) return ImageInputError::Cancelled;
    output = candidate;
    return ImageInputError::None;
}

} // namespace Slic3r::GUI
