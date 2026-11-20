#include "slic3r/GUI/Redesign/AccountAvatar.hpp"
#include <catch2/catch_test_macros.hpp>
#include <wx/filename.h>
#include <wx/imagjpeg.h>
#include <wx/imagpng.h>

namespace Avatar = Slic3r::GUI::AccountAvatar;
namespace HA = Slic3r::HonorAccount;
namespace {
struct Images {
    wxString directory = wxFileName::GetTempDir() + "/orca-avatar-" + wxString::FromUTF8(HA::random_value());
    Images()
    {
        if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG)) wxImage::AddHandler(new wxPNGHandler);
        if (!wxImage::FindHandler(wxBITMAP_TYPE_JPEG)) wxImage::AddHandler(new wxJPEGHandler);
        REQUIRE(wxFileName::Mkdir(directory));
    }
    ~Images() { wxFileName::Rmdir(directory, wxPATH_RMDIR_RECURSIVE); }
    wxString path(const char* name) const { return directory + "/" + name; }
};
wxImage solid(int w, int h, unsigned char r = 20, unsigned char g = 80, unsigned char b = 160)
{
    wxImage image(w, h); image.SetRGB(wxRect(0,0,w,h),r,g,b); return image;
}
void write(const wxString& path, const std::string& bytes)
{
    wxFile file(path, wxFile::write); REQUIRE(file.IsOpened()); REQUIRE(file.Write(bytes.data(),bytes.size()) == bytes.size());
}
}

TEST_CASE("Avatar previews crop the center and preserve transparent image appearance", "[AccountAvatar]")
{
    auto wide = solid(600,300,255,0,0);
    wide.SetRGB(wxRect(150,0,300,300),0,80,160);
    const auto cropped = Avatar::square(wide);
    REQUIRE(cropped.GetSize() == wxSize(96,96));
    REQUIRE(cropped.GetRed(0,48) == 0);
    REQUIRE(cropped.GetBlue(95,48) == 160);
    auto transparent = solid(30,60,255,0,0);transparent.InitAlpha();
    std::memset(transparent.GetAlpha(),0,30*60);
    const auto white = Avatar::square(transparent);
    REQUIRE_FALSE(white.HasAlpha());
    REQUIRE(white.GetGreen(48,48) == 255);
    const auto round = Avatar::circle(cropped,72);
    REQUIRE(round.GetAlpha(0,0) == 0);
    REQUIRE(round.GetAlpha(36,36) == 255);
}

TEST_CASE("Avatars accept PNG and JPEG content while rejecting corrupt and oversized images", "[AccountAvatar]")
{
    Images files;
    const auto image=solid(40,80);
    REQUIRE(image.SaveFile(files.path("valid.png"),wxBITMAP_TYPE_PNG));
    REQUIRE(image.SaveFile(files.path("valid.jpg"),wxBITMAP_TYPE_JPEG));
    REQUIRE(Avatar::read(files.path("valid.png")).GetSize() == image.GetSize());
    REQUIRE(Avatar::read(files.path("valid.jpg")).GetSize() == image.GetSize());
    write(files.path("invalid.png"),"not an image");
    REQUIRE_FALSE(Avatar::read(files.path("invalid.png")).IsOk());
    write(files.path("huge.jpg"),std::string(Avatar::max_bytes+1,'x'));
    REQUIRE_FALSE(Avatar::read(files.path("huge.jpg")).IsOk());
    REQUIRE(solid(4097,1).SaveFile(files.path("wide.png"),wxBITMAP_TYPE_PNG));
    REQUIRE_FALSE(Avatar::read(files.path("wide.png")).IsOk());
    REQUIRE_FALSE(Avatar::read(files.path("missing.png")).IsOk());
}

TEST_CASE("Untrusted image headers are bounded before raster allocation", "[AccountAvatar]")
{
    wxBitmapType type=wxBITMAP_TYPE_INVALID;
    std::string png("\x89PNG\r\n\x1a\n\0\0\0\rIHDR",16);
    png.append("\x7f\xff\xff\xff\0\0\0\x10",8);
    REQUIRE(Avatar::encoded_size(png,type).x <= 0);
    const std::string truncated_jpeg("\xff\xd8\xff\xc0\xff\xff",6);
    REQUIRE(Avatar::encoded_size(truncated_jpeg,type).x <= 0);
    REQUIRE(Avatar::encoded_size("GIF89a",type).x <= 0);
}

TEST_CASE("Account avatars decode bounded HTTPS and inline PNG without platform credentials", "[AccountAvatar]")
{
    Images files;
    const auto image = solid(12, 18);
    wxMemoryOutputStream stream;
    REQUIRE(image.SaveFile(stream, wxBITMAP_TYPE_PNG));
    std::string encoded(stream.GetSize(), '\0');
    stream.CopyTo(encoded.data(), encoded.size());
    unsigned requests = 0;
    const HA::Transport mock = [&](const HA::Request& request, HA::Cancellation) {
        ++requests;
        REQUIRE(request.bearer.empty());
        REQUIRE(request.headers.empty());
        REQUIRE(request.response_limit == Avatar::max_bytes);
        return HA::Response{200, encoded};
    };
    REQUIRE(Avatar::fetch("https://avatar.example.test/profile.jpg", {}, mock).GetSize() == image.GetSize());
    REQUIRE_FALSE(Avatar::fetch("http://avatar.example.test/profile.jpg", {}, mock).IsOk());
    REQUIRE_FALSE(Avatar::fetch("https://user@avatar.example.test/profile.jpg", {}, mock).IsOk());
    const auto inline_png = "data:image/png;base64," + wxBase64Encode(encoded.data(), encoded.size()).ToStdString();
    REQUIRE(Avatar::fetch(inline_png, {}, mock).GetSize() == image.GetSize());
    REQUIRE(requests == 1);
}

TEST_CASE("Repeated avatar previews leave cached source pixels and alpha unchanged", "[AccountAvatar]")
{
    auto source=solid(72,72);source.InitAlpha();
    std::memset(source.GetAlpha(),255,72*72);
    source.GetAlpha()[0]=128;
    const auto first=Avatar::circle(source,72);
    const auto second=Avatar::circle(source,72);
    REQUIRE(source.GetAlpha(0,0) == 128);
    REQUIRE(first.GetAlpha(0,0) == 0);
    REQUIRE(second.GetAlpha(0,0) == 0);
    REQUIRE(source.GetBlue(0,0) == 160);
}

TEST_CASE("Avatar uploads encode a bounded 96 pixel PNG", "[AccountAvatar]")
{
    Images files;
    const auto bytes = Avatar::png_data(Avatar::square(solid(200,100,0,200,0)));
    REQUIRE(bytes.rfind("data:image/png;base64,", 0) == 0);
    REQUIRE(bytes.size() <= 65536);
    const auto decoded = Avatar::fetch(bytes, {});
    REQUIRE(decoded.IsOk());
    REQUIRE(decoded.GetSize() == wxSize(96,96));
    REQUIRE(decoded.GetGreen(48,48) == 200);
    REQUIRE(Avatar::png_data(solid(95,96)).empty());
    REQUIRE(Avatar::png_data(wxImage{}).empty());
}
