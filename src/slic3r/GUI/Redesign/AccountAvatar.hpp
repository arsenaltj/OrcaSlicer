#pragma once

#include "../../Utils/HonorAccount/HonorClient.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <wx/base64.h>
#include <wx/file.h>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/mstream.h>

namespace Slic3r::GUI::AccountAvatar {

constexpr size_t max_bytes = 10 * 1024 * 1024;
constexpr int max_edge = 4096;
constexpr int stored_edge = 96;
constexpr size_t max_data_url = 65536;
constexpr std::string_view png_prefix = "data:image/png;base64,";

// Check dimensions before decoding, so a small compressed file cannot allocate
// an unbounded raster. Decode the same bounded bytes (not a reopened source).
inline wxSize encoded_size(const std::string& bytes, wxBitmapType& type)
{
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t n = bytes.size();
    if (n >= 24 && std::memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0 &&
        p[8] == 0 && p[9] == 0 && p[10] == 0 && p[11] == 13 && std::memcmp(p + 12, "IHDR", 4) == 0) {
        auto word = [p](size_t i) { return (uint32_t(p[i]) << 24) | (uint32_t(p[i+1]) << 16) | (uint32_t(p[i+2]) << 8) | p[i+3]; };
        const auto w = word(16), h = word(20);
        type = wxBITMAP_TYPE_PNG;
        if (w > 0 && h > 0 && w <= max_edge && h <= max_edge) return wxSize(int(w), int(h));
        return {};
    }
    if (n < 4 || p[0] != 0xff || p[1] != 0xd8) return {};
    type = wxBITMAP_TYPE_JPEG;
    size_t i = 2;
    while (i < n) {
        if (p[i++] != 0xff) return {};
        while (i < n && p[i] == 0xff) ++i;
        if (i == n) return {};
        const unsigned marker = p[i++];
        if (marker == 0xda || marker == 0xd9 || marker == 0) return {};
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (n - i < 2) return {};
        const size_t length = (unsigned(p[i]) << 8) | p[i+1];
        if (length < 2 || length > n - i) return {};
        const bool frame = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
        if (frame) {
            if (length < 8) return {};
            const int h = (unsigned(p[i+3]) << 8) | p[i+4], w = (unsigned(p[i+5]) << 8) | p[i+6];
            if (w > 0 && h > 0 && w <= max_edge && h <= max_edge) return wxSize(w, h);
            return {};
        }
        i += length;
    }
    return {};
}

inline wxImage decode(const std::string& bytes)
{
    wxLogNull quiet;
    if (bytes.empty() || bytes.size() > max_bytes) return {};
    wxBitmapType type = wxBITMAP_TYPE_INVALID;
    const auto size = encoded_size(bytes, type);
    if (size.x <= 0 || size.y <= 0) return {};
    wxMemoryInputStream stream(bytes.data(), bytes.size());
    wxImage image(stream, type);
    if (!image.IsOk() || image.GetSize() != size) return {};
    return image;
}

inline wxImage read(const wxString& path)
{
    if (path.empty()) return {};
    wxLogNull quiet;
    wxFile input(path, wxFile::read);
    if (!input.IsOpened()) return {};
    const auto length = input.Length();
    if (length <= 0 || length > wxFileOffset(max_bytes)) return {};
    std::string bytes(size_t(length), '\0');
    if (input.Read(bytes.data(), bytes.size()) != wxFileOffset(bytes.size())) return {};
    return decode(bytes);
}

inline wxImage fetch(const std::string& url, HonorAccount::Cancellation cancel,
                     const HonorAccount::Transport& transport = HonorAccount::https_request)
{
    if (cancel && cancel->load()) return {};
    if (url.compare(0, png_prefix.size(), png_prefix) == 0 && url.size() <= max_data_url) {
        const auto bytes = wxBase64Decode(url.data() + png_prefix.size(), url.size() - png_prefix.size());
        if (!bytes.GetDataLen()) return {};
        const std::string encoded(static_cast<const char*>(bytes.GetData()), bytes.GetDataLen());
        if (encoded.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0) return {};
        return decode(encoded);
    }
    if (!HonorAccount::valid_authorization_url(url)) return {};
    try {
        HonorAccount::Request request;
        request.url = url;
        request.accept = "image/png, image/jpeg";
        request.response_limit = max_bytes;
        const auto response = transport(request, cancel);
        if (response.status == 200 && (!cancel || !cancel->load())) return decode(response.body);
    } catch (const HonorAccount::Error&) {
    }
    return {};
}

inline wxImage square(const wxImage& source)
{
    if (!source.IsOk()) return {};
    const int side = std::min(source.GetWidth(), source.GetHeight());
    wxImage result = source.GetSubImage(wxRect((source.GetWidth()-side)/2, (source.GetHeight()-side)/2, side, side))
        .Scale(stored_edge, stored_edge, wxIMAGE_QUALITY_HIGH);
    // Flatten transparency against white for consistent previews and sidebars.
    if (result.HasAlpha() || result.HasMask()) {
        const auto* alpha = result.HasAlpha() ? result.GetAlpha() : nullptr;
        auto* rgb = result.GetData();
        for (int i = 0; i < stored_edge * stored_edge; ++i) {
            const unsigned a = alpha ? alpha[i] :
                (rgb[3*i] == result.GetMaskRed() && rgb[3*i+1] == result.GetMaskGreen() && rgb[3*i+2] == result.GetMaskBlue() ? 0 : 255);
            for (int c = 0; c < 3; ++c) rgb[3*i+c] = (rgb[3*i+c]*a + 255*(255-a) + 127)/255;
        }
        result.ClearAlpha(); result.SetMask(false);
    }
    return result;
}

inline wxImage circle(const wxImage& source, int edge)
{
    if (!source.IsOk() || edge <= 0) return {};
    // Scale may share the source buffer when dimensions already match.
    // Applying a display mask must never change a cached image or saved pixels.
    wxImage result = source.GetSize() == wxSize(edge, edge) ? source.Copy() :
        source.Scale(edge, edge, wxIMAGE_QUALITY_HIGH);
    if (!result.HasAlpha()) result.InitAlpha();
    auto* alpha = result.GetAlpha();
    const double radius = edge / 2.0;
    for (int y = 0; y < edge; ++y)
        for (int x = 0; x < edge; ++x) {
            const double coverage = std::clamp(radius - std::hypot(x + 0.5 - radius, y + 0.5 - radius) + 0.5, 0.0, 1.0);
            alpha[y*edge+x] = static_cast<unsigned char>(alpha[y*edge+x] * coverage);
        }
    return result;
}

inline std::string png_data(const wxImage& image)
{
    if (!image.IsOk() || image.GetSize() != wxSize(stored_edge, stored_edge)) return {};
    wxLogNull quiet;
    wxMemoryOutputStream encoded;
    if (!image.SaveFile(encoded, wxBITMAP_TYPE_PNG)) return {};
    std::string bytes(encoded.GetSize(), '\0');
    encoded.CopyTo(bytes.data(), bytes.size());
    const auto base64 = wxBase64Encode(bytes.data(), bytes.size());
    if (base64.empty() || base64.size() + png_prefix.size() > max_data_url) return {};
    return std::string(png_prefix) + std::string(base64.utf8_string());
}

} // namespace Slic3r::GUI::AccountAvatar
