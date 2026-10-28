#include "slic3r/GUI/Redesign/AccountAvatar.hpp"
#include <catch2/catch_test_macros.hpp>
#include <wx/dir.h>
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

TEST_CASE("Account avatars remain separate across accounts applications and environments", "[AccountAvatar]")
{
    HA::Config config;
    const auto first = Avatar::account_path("/user-data",config,"subject-one");
    REQUIRE(first.Contains("/honor-avatars/"));
    REQUIRE_FALSE(first.Contains("subject-one"));
    REQUIRE(Avatar::account_path("/user-data",config,"").empty());
    REQUIRE(Avatar::account_path("",config,"subject-one").empty());
    REQUIRE(first != Avatar::account_path("/user-data",config,"subject-two"));
    config.client_id += "2";
    REQUIRE(first != Avatar::account_path("/user-data",config,"subject-one"));
    config = HA::Config{};config.environment = "production";
    REQUIRE(first != Avatar::account_path("/user-data",config,"subject-one"));
}

TEST_CASE("Avatar previews crop the center and preserve transparent image appearance", "[AccountAvatar]")
{
    auto wide = solid(600,300,255,0,0);
    wide.SetRGB(wxRect(150,0,300,300),0,80,160);
    const auto cropped = Avatar::square(wide);
    REQUIRE(cropped.GetSize() == wxSize(256,256));
    REQUIRE(cropped.GetRed(0,128) == 0);
    REQUIRE(cropped.GetBlue(255,128) == 160);
    auto transparent = solid(30,60,255,0,0);transparent.InitAlpha();
    std::memset(transparent.GetAlpha(),0,30*60);
    const auto white = Avatar::square(transparent);
    REQUIRE_FALSE(white.HasAlpha());
    REQUIRE(white.GetGreen(128,128) == 255);
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

TEST_CASE("Saved avatars survive source removal and reset without affecting another account", "[AccountAvatar]")
{
    Images files;HA::Config config;
    const auto path=Avatar::account_path(files.directory,config,"first");
    const auto other=Avatar::account_path(files.directory,config,"second");
    REQUIRE(solid(100,200).SaveFile(files.path("source.png"),wxBITMAP_TYPE_PNG));
    REQUIRE(Avatar::save(path,Avatar::square(Avatar::read(files.path("source.png")))));
    REQUIRE(Avatar::save(other,Avatar::square(solid(60,30,200,0,0))));
    REQUIRE(wxRemoveFile(files.path("source.png")));
    REQUIRE(Avatar::read(path).GetSize() == wxSize(256,256));
    REQUIRE(Avatar::read(path).GetBlue(128,128) == 160);
    REQUIRE(Avatar::save(path,Avatar::square(solid(200,100,0,200,0))));
    REQUIRE(Avatar::read(path).GetGreen(128,128) == 200);
    REQUIRE_FALSE(Avatar::save(path,wxImage{}));
    REQUIRE(Avatar::read(path).GetGreen(128,128) == 200);
    REQUIRE(Avatar::reset(path));
    REQUIRE_FALSE(wxFileExists(path));
    REQUIRE(Avatar::read(other).GetRed(128,128) == 200);
    REQUIRE(Avatar::reset(path));
}

TEST_CASE("Avatar write failures preserve the previous image and leave no temporary files", "[AccountAvatar]")
{
    Images files;const auto path=files.path("saved.png");
    REQUIRE(Avatar::save(path,Avatar::square(solid(60,60))));
#ifdef __WXMSW__
    HANDLE lock=::CreateFileW(path.wc_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    const bool saved=Avatar::save(path,Avatar::square(solid(60,60,200,0,0)));
    const bool reset=Avatar::reset(path);
    ::CloseHandle(lock);
    REQUIRE_FALSE(saved);
    REQUIRE_FALSE(reset);
    REQUIRE(Avatar::read(path).GetBlue(128,128) == 160);
#endif
    write(files.path("blocked"),"regular file");
    REQUIRE_FALSE(Avatar::save(files.path("blocked/image.png"),Avatar::square(solid(20,20))));
    wxArrayString leftovers;
    wxDir::GetAllFiles(files.directory,&leftovers,"saved.png.*",wxDIR_FILES);
    REQUIRE(leftovers.empty());
}
