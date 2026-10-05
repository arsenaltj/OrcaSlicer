#pragma once

#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {
struct ModelLibraryExportRequest {
    boost::filesystem::path model, design, reference;
    std::string id, title, task_id, conversion_task_id;
};

// Only accepted library snapshots enter this function. The new directory is
// exclusively reserved before writing; rollback never touches a preexisting path.
inline bool export_model_library_copy(const ModelLibraryExportRequest& request,
    const boost::filesystem::path& parent, boost::filesystem::path& destination, std::string& error)
{
    namespace fs = boost::filesystem;
    destination.clear();
    error.clear();
    fs::path owned;
    try {
        if (!fs::is_directory(parent)) throw std::runtime_error("请选择已存在的导出文件夹。");
        if (request.model.empty() && request.design.empty()) throw std::runtime_error("没有可导出的模型或设计图。");
        for (const auto& path : {request.model, request.design, request.reference})
            if (!path.empty() && (!fs::is_regular_file(path) || fs::file_size(path) == 0))
                throw std::runtime_error("源文件缺失或为空，导出未完成：" + path.filename().string());
        std::string name = request.id.substr(0, 80);
        for (char& c : name)
            if (!std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 128)
                if (c != '-' && c != '_') c = '_';
        if (name.empty()) name = "asset";
        name = "Orca-" + name;
        for (unsigned i = 0; i < 1000; ++i) {
            const fs::path candidate = parent / (name + (i ? "-" + std::to_string(i + 1) : ""));
            boost::system::error_code ec;
            if (fs::create_directory(candidate, ec)) { owned = candidate; break; }
            if (ec && !fs::exists(candidate))
                throw std::runtime_error("无法创建导出文件夹。请尝试较短路径或更换可写位置，并检查磁盘空间。");
        }
        if (owned.empty()) throw std::runtime_error("同名副本过多，请选择其他文件夹。");
        if (!request.model.empty()) {
            std::string reason;
            if (!AI::archive_local_model(request.model, owned / "model.glb", reason)) {
                BOOST_LOG_TRIVIAL(warning) << "Library model export failed: " << reason;
                throw std::runtime_error("无法导出模型副本。请检查 GLB/OBJ 是否完整、关联贴图是否齐全，或更换可写导出位置后重试。");
            }
        }
        std::vector<std::string> images;
        for (const auto& item : std::vector<std::pair<fs::path, std::string>> {
                {request.design, "design"}, {request.reference, "reference"}}) {
            if (item.first.empty()) continue;
            const auto name = item.second + item.first.extension().string();
            fs::copy_file(item.first, owned / name);
            images.push_back(name);
        }
        fs::ofstream info(owned / "README.txt", std::ios::binary);
        info << "OrcaSlicer 历史资产副本\n名称：" << request.title << "\n本地 ID：" << request.id;
        if (!request.task_id.empty()) info << "\nTask ID：" << request.task_id;
        if (!request.conversion_task_id.empty()) info << "\n转换任务 ID：" << request.conversion_task_id;
        info << "\n\n";
        if (!request.model.empty()) info << "model.glb：自包含模型，材质/贴图随模型保存，可通过导入模型打开。\n";
        for (const auto& name : images) info << name << "\n";
        info << "设计图为关联历史图片，不代表美颜后模型外观。\n"
                "这是已保存资产副本，不包含未保存编辑、工程打印预设或完整任务恢复状态。\n";
        info.close();
        if (!info) throw std::runtime_error("无法完整写入导出说明，请检查磁盘空间。");
        destination = owned;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        if (!owned.empty()) {
            boost::system::error_code ec;
            fs::remove_all(owned, ec);
            if (ec) error += "\n未完成副本保留在：" + owned.string();
        }
        return false;
    }
}
} // namespace Slic3r::GUI
