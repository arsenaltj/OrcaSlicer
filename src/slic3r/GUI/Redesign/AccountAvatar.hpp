#pragma once

#include "../../Utils/HonorAccount/HonorOAuth.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/mstream.h>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

namespace Slic3r::GUI::AccountAvatar {

constexpr size_t max_bytes = 10 * 1024 * 1024;
constexpr int max_edge = 4096;
constexpr int stored_edge = 256;

// No raw account identifiers or original image paths in filenames/settings.
inline wxString account_path(const wxString& data, const HonorAccount::Config& config, const std::string& subject)
{
    if (data.empty() || subject.empty()) return {};
    const auto key = HonorAccount::digest(std::to_string(config.environment.size()) + ":" + config.environment +
        std::to_string(config.client_id.size()) + ":" + config.client_id + subject);
    return data + "/honor-avatars/" + wxString::FromUTF8(key) + ".png";
}

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

inline wxImage read(const wxString& path)
{
    wxLogNull quiet; // Decoder messages can contain private source paths.
    wxFile input(path, wxFile::read);
    if (!input.IsOpened()) return {};
    const auto length = input.Length();
    if (length <= 0 || length > wxFileOffset(max_bytes)) return {};
    std::string bytes(size_t(length), '\0');
    if (input.Read(bytes.data(), bytes.size()) != wxFileOffset(bytes.size())) return {};
    wxBitmapType type = wxBITMAP_TYPE_INVALID;
    const auto size = encoded_size(bytes, type);
    if (size.x <= 0 || size.y <= 0) return {};
    wxMemoryInputStream stream(bytes.data(), bytes.size());
    wxImage image(stream, type);
    if (!image.IsOk() || image.GetSize() != size) return {};
    return image;
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

// Use a sibling temporary and a single replace. No truncate-in-place fallback:
// failing to save must leave the previous avatar intact.
inline bool replace(const wxString& from, const wxString& to)
{
#ifdef __WXMSW__
    return ::MoveFileExW(from.wc_str(), to.wc_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return ::rename(from.utf8_str(), to.utf8_str()) == 0;
#endif
}

inline bool save(const wxString& path, const wxImage& image)
{
    if (path.empty() || !image.IsOk() || image.GetSize() != wxSize(stored_edge, stored_edge)) return false;
    wxLogNull quiet;
    const auto parent = wxFileName(path).GetPath();
    if (!wxFileName::DirExists(parent) && !wxFileName::Mkdir(parent, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) return false;
    wxFile temporary;
    const auto name = wxFileName::CreateTempFileName(path + ".", &temporary);
    if (name.empty()) return false;
    wxMemoryOutputStream encoded;
    bool ok = image.SaveFile(encoded, wxBITMAP_TYPE_PNG);
    if (ok) {
        std::string bytes(encoded.GetSize(), '\0'); encoded.CopyTo(bytes.data(), bytes.size());
        ok = temporary.Write(bytes.data(), bytes.size()) == bytes.size() && temporary.Flush();
    }
    const bool closed = temporary.Close();
    ok = ok && closed && replace(name, path);
    if (!ok) wxRemoveFile(name);
    return ok;
}

inline bool reset(const wxString& path)
{
    if (path.empty()) return false;
    wxLogNull quiet;
    return !wxFileExists(path) || wxRemoveFile(path);
}

} // namespace Slic3r::GUI::AccountAvatar
