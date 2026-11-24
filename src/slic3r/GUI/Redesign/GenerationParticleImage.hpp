#pragma once
#include "GenerationParticleModel.hpp"
#include "../AI/ModelGeneration/ModelLibraryThumbnail.hpp"
#include <wx/log.h>
#include <wx/mstream.h>
#include <openssl/evp.h>
#include <chrono>
#include <nlohmann/json.hpp>

namespace Slic3r::GUI::GenerationParticles {
inline constexpr std::uint64_t MAX_REFERENCE_IMAGE_BYTES = 20 * 1024 * 1024;
inline constexpr std::uint64_t MAX_REFERENCE_IMAGE_PIXELS = 16 * 1024 * 1024;

namespace detail {
// This display consumer validates the frozen input shape, without changing the
// job or substituting the group's current images for the accepted reference.
inline bool valid_advanced_snapshot(const nlohmann::json& snapshot)
{
    const auto& views=snapshot.at("views");
    if (!views.is_object() || !views.contains("front")) return false;
    const std::array<const char*,4> directions {"front","left","back","right"};
    auto selected=nlohmann::json::array();
    for (const auto* direction : directions) if (views.contains(direction)) selected.push_back(direction);
    if (selected.size()!=views.size()) return false;
    const auto& schema=snapshot.at("schema");
    if (schema=="approved-multiview-input-v1") return views.size()==directions.size();
    if (schema!="approved-multiview-input-v2") return false;
    return snapshot.at("selected_views")==selected &&
        snapshot.at("input_mode")== (selected.size()==1 ? "image" : "multiview") &&
        !snapshot.at("input_epoch").get<std::string>().empty();
}

// Read only the bounded JPEG APP1/IFD0 orientation. Particle display must not
// depend on the retired single-image input workflow or follow arbitrary offsets.
inline int jpeg_orientation(const std::vector<unsigned char>& bytes)
{
    for (std::size_t pos = 2; pos + 4 <= bytes.size();) {
        if (bytes[pos++] != 0xff) break;
        while (pos < bytes.size() && bytes[pos] == 0xff) ++pos;
        if (pos >= bytes.size()) break;
        const auto marker = bytes[pos++];
        if (marker == 0xda || marker == 0xd9) break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (pos + 2 > bytes.size()) break;
        const std::size_t length = (std::size_t(bytes[pos]) << 8) | bytes[pos + 1];
        if (length < 2 || length > bytes.size() - pos) break;
        const std::size_t end = pos + length, base = pos + 8;
        if (marker == 0xe1 && length >= 16 &&
            std::equal(bytes.begin() + pos + 2, bytes.begin() + pos + 8, "Exif\0\0")) {
            const bool little = bytes[base] == 'I' && bytes[base + 1] == 'I';
            const bool big = bytes[base] == 'M' && bytes[base + 1] == 'M';
            if (!little && !big) return 1;
            const auto word = [&](std::size_t p) -> std::uint32_t {
                if (p > end || end - p < 2) return 0;
                return little ? std::uint32_t(bytes[p]) | (std::uint32_t(bytes[p + 1]) << 8)
                              : (std::uint32_t(bytes[p]) << 8) | bytes[p + 1];
            };
            const auto dword = [&](std::size_t p) -> std::uint32_t {
                if (p > end || end - p < 4) return 0;
                return little ? word(p) | (word(p + 2) << 16) : (word(p) << 16) | word(p + 2);
            };
            if (word(base + 2) != 42) return 1;
            const auto offset = dword(base + 4);
            if (offset < 8 || offset > end - base || end - base - offset < 2) return 1;
            const std::size_t table = base + offset;
            const std::size_t count = word(table);
            if (count > (end - table - 2) / 12) return 1;
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t entry = table + 2 + i * 12;
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

inline wxImage orient_image(wxImage image, int orientation)
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
} // namespace detail

inline std::shared_ptr<const Shape> load(const ModelGenerationVisualReference& source, bool logo,
    const std::function<bool()>& canceled, const ForegroundProvider& foreground = {},
    PreparationDetails* details = nullptr)
{
    PreparationDetails local;
    auto& report=details?*details:local;
    report={"decode","invalid_or_unavailable_image"};
    const auto stopped = [&] { return canceled && canceled(); };
    if (stopped() || source.path.empty()) return {};
    const auto path = boost::filesystem::path(wxString::FromUTF8(source.path).ToStdWstring());
    boost::system::error_code error;
    std::string expected = source.sha256;
    if (!source.verification_manifest.empty()) {
        report={"snapshot","invalid_multiview_snapshot"};
        const auto manifest = boost::filesystem::path(wxString::FromUTF8(source.verification_manifest).ToStdWstring());
        const auto length = boost::filesystem::file_size(manifest, error);
        if (error || !length || length > 2*1024*1024 ||
            path != manifest.parent_path() / "inputs" / "front.png") return {};
        std::string text(static_cast<std::size_t>(length), '\0');
        boost::filesystem::ifstream receipt(manifest, std::ios::binary);
        receipt.read(text.data(), std::streamsize(text.size()));
        if (std::size_t(receipt.gcount()) != text.size() || stopped()) return {};
        try {
            const auto job = nlohmann::json::parse(text);
            const auto& snapshot = job.at("approved_multiview_snapshot");
            const auto& front = snapshot.at("views").at("front");
            if (job.at("id") != source.owner_id || job.at("creation_kind") != "advanced_multiview_v1" ||
                !detail::valid_advanced_snapshot(snapshot) || snapshot.at("snapshot_id") != source.owner_id ||
                front.at("path") != "inputs/front.png" || front.at("version_id").get<std::string>().empty()) return {};
            expected = front.at("sha256").get<std::string>();
            if (expected.size() != 64) return {};
        } catch (const nlohmann::json::exception&) { return {}; }
    }
    report={"decode","invalid_or_unavailable_image"};
    const auto bytes_count = boost::filesystem::file_size(path, error);
    if (error || !bytes_count || bytes_count > MAX_REFERENCE_IMAGE_BYTES) return {};
    bool png = false;
    const auto dimensions = library_image_dimensions(path, png);
    if (dimensions.x < 8 || dimensions.y < 8 ||
        std::uint64_t(dimensions.x) * dimensions.y > MAX_REFERENCE_IMAGE_PIXELS) return {};
    std::vector<unsigned char> bytes(static_cast<std::size_t>(bytes_count));
    boost::filesystem::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    if (std::size_t(file.gcount()) != bytes.size() || stopped()) return {};
    if (!png && (bytes.size()<2 || bytes[bytes.size()-2]!=0xff || bytes.back()!=0xd9)) return {};
    if (!expected.empty()) {
        unsigned char digest[EVP_MAX_MD_SIZE] {}; unsigned int size = 0;
        if (EVP_Digest(bytes.data(), bytes.size(), digest, &size, EVP_sha256(), nullptr) != 1 || size != 32) return {};
        constexpr char hex[] = "0123456789abcdef";
        std::string actual;
        for (unsigned int i = 0; i < size; ++i) { actual += hex[digest[i] >> 4]; actual += hex[digest[i] & 15]; }
        if (actual != expected) return {};
    }
    wxImage decoded; wxMemoryInputStream stream(bytes.data(), bytes.size());
    wxPNGHandler png_handler; wxJPEGHandler jpeg_handler;
    wxLogNull quiet;
    if (!(png ? png_handler.LoadFile(&decoded, stream) : jpeg_handler.LoadFile(&decoded, stream)) ||
        !decoded.IsOk() || decoded.GetWidth() != dimensions.x || decoded.GetHeight() != dimensions.y || stopped()) return {};
    if (!png) decoded = detail::orient_image(std::move(decoded), detail::jpeg_orientation(bytes));
    const double scale = std::min(1.0, 384.0 / std::max(decoded.GetWidth(), decoded.GetHeight()));
    if (scale < 1) decoded.Rescale(std::max(8, int(decoded.GetWidth()*scale)),
        std::max(8, int(decoded.GetHeight()*scale)), wxIMAGE_QUALITY_HIGH);
    if (stopped()) return {};
    Image image; image.width = decoded.GetWidth(); image.height = decoded.GetHeight();
    image.rgba.resize(std::size_t(image.width) * image.height * 4);
    const auto* rgb = decoded.GetData(); const auto* alpha = decoded.HasAlpha() ? decoded.GetAlpha() : nullptr;
    for (std::size_t i = 0; i < image.rgba.size()/4; ++i) {
        for (int c = 0; c < 3; ++c) image.rgba[4*i+c] = rgb[3*i+c];
        image.rgba[4*i+3] = alpha ? alpha[i] : 255;
    }
    // Existing job subject masks do not carry a digest binding them to the
    // actual modelling image. Do not reuse them by path/name alone. Alpha is
    // embedded in the verified image bytes and has no stale-mask ambiguity.
    if (logo || has_subject_alpha(image)) {
        report.method=logo?"logo":"image_alpha";
        const auto shape=prepare(image,logo,stopped);
        report.reason=shape?"ready":"invalid_mask";
        return shape;
    }
    if (foreground) {
        auto result=foreground(image,stopped);
        report={"local_foreground",result.reason};
        if (stopped()) { report.reason="canceled"; return {}; }
        if (!result.mask.alpha.empty()) {
            const auto shape=prepare(image,false,stopped,8000,&result.mask);
            report.reason=shape?"ready":"invalid_mask";
            // A semantic mask that fails validation must not be replaced by
            // a less reliable colour guess on this same image.
            return shape;
        }
    }
    if (stopped()) { report.reason="canceled"; return {}; }
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
    const auto shape=prepare(image,false,[&] { return stopped() || std::chrono::steady_clock::now()>deadline; });
    if (shape) report={"plain_background","ready"};
    else report.reason+=";plain_background_unreliable";
    return shape;
}
} // namespace Slic3r::GUI::GenerationParticles
