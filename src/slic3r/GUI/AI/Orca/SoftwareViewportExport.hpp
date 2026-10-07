#pragma once
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <array>
#include <algorithm>
#include <vector>
#include <stdexcept>

namespace Slic3r::GUI {
// Publish a complete new screenshot, rejecting even a racing destination.
// A failed write leaves the preview in memory and removes only our staging file.
inline void export_software_viewport_png(const boost::filesystem::path& destination,
                                         const std::vector<unsigned char>& png)
{
    constexpr std::array<unsigned char, 8> signature {{137, 80, 78, 71, 13, 10, 26, 10}};
    if (png.size() < signature.size() || !std::equal(signature.begin(), signature.end(), png.begin()))
        throw std::runtime_error("截图编码失败。预览保留，请重新截图后重试。");
    if (boost::filesystem::exists(destination) || boost::filesystem::is_symlink(destination))
        throw std::runtime_error("文件已存在，未覆盖。请选择其他名称；当前截图保留。");
    const auto staging = destination.parent_path() / boost::filesystem::unique_path(".orca-viewport-%%%%-%%%%.tmp");
    try {
        boost::filesystem::ofstream output(staging, std::ios::binary);
        output.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
        output.close();
        if (!output) throw std::runtime_error("截图写入失败。请检查目录权限和磁盘空间，再选择位置重试；当前截图保留。");
        boost::filesystem::create_hard_link(staging, destination);
    } catch (...) {
        boost::system::error_code ignored;
        boost::filesystem::remove(staging, ignored);
        throw;
    }
    boost::system::error_code ignored;
    boost::filesystem::remove(staging, ignored);
}
}
