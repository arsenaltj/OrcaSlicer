#pragma once

#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/ModelFilePath.hpp"
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <vector>

namespace Slic3r::GUI {

class WorkbenchRepairStaging
{
public:
    explicit WorkbenchRepairStaging(boost::filesystem::path root = {})
    {
        namespace fs = boost::filesystem;
        m_published.reserve(2);
        if (root.empty()) {
#ifdef _WIN32
            root = boost::nowide::widen(Slic3r::temporary_dir());
#else
            root = Slic3r::temporary_dir();
#endif
        }
        if (root.empty())
            root = fs::temp_directory_path();
        fs::create_directories(AI::model_io_path(root));
        auto id = boost::uuids::to_string(boost::uuids::random_generator()());
        id.erase(std::remove(id.begin(), id.end(), '-'), id.end());
        m_directory = fs::absolute(root) / ("r-" + id);
        if (!fs::create_directory(AI::model_io_path(m_directory)))
            throw std::runtime_error("无法创建独立的修复临时目录，请重试。");
    }

    WorkbenchRepairStaging(const WorkbenchRepairStaging&) = delete;
    WorkbenchRepairStaging& operator=(const WorkbenchRepairStaging&) = delete;

    ~WorkbenchRepairStaging()
    {
        discard_published();
        boost::system::error_code ignored;
        boost::filesystem::remove_all(AI::model_io_path(m_directory), ignored);
    }

    boost::filesystem::path model() const { return m_directory / "m.obj"; }
    boost::filesystem::path appearance() const { return m_directory / "a.obj"; }

    void publish(const boost::filesystem::path& staged, const boost::filesystem::path& destination,
                 const std::string& sha256, const std::function<bool()>& canceled)
    {
        namespace fs = boost::filesystem;
        const auto check_cancel = [&] {
            if (canceled && canceled())
                throw std::runtime_error("已取消保存修复结果。");
        };
        check_cancel();
        if (sha256.empty() || AI::model_artifact_sha256(staged) != sha256)
            throw std::runtime_error("修复文件校验失败，请重新检查。");
        const auto source = AI::model_io_path(staged);
        const auto target = AI::model_io_path(destination);
        m_published.push_back(target);
        bool linked = false;
        try {
            boost::system::error_code error;
            fs::create_hard_link(source, target, error);
            if (error) {
                if (error != boost::system::errc::cross_device_link
#ifdef _WIN32
                    && !(error.category() == boost::system::system_category() && error.value() == 17)
#endif
                )
                    throw fs::filesystem_error("publish repair", source, target, error);
                // A private directory on the destination volume keeps the copy
                // out of scans and lets the final link commit without replacing.
                const auto transfer = destination.parent_path() / fs::unique_path(".r-%%%%%%%%");
                const auto io_transfer = AI::model_io_path(transfer);
                if (!fs::create_directory(io_transfer))
                    throw std::runtime_error("无法创建修复保存暂存目录，请重试。");
                try {
                    const auto copy = AI::model_io_path(transfer / "p");
                    fs::copy_file(source, copy);
                    if (AI::model_artifact_sha256(copy) != sha256)
                        throw std::runtime_error("修复文件复制校验失败，请重试。");
                    check_cancel();
                    fs::create_hard_link(copy, target);
                    linked = true;
                } catch (...) {
                    boost::system::error_code ignored;
                    fs::remove_all(io_transfer, ignored);
                    throw;
                }
                fs::remove_all(io_transfer, error);
            } else {
                linked = true;
            }
            check_cancel();
        } catch (...) {
            if (linked) {
                boost::system::error_code ignored;
                fs::remove(target, ignored);
            }
            m_published.pop_back();
            throw;
        }
    }

    void discard_published()
    {
        boost::system::error_code ignored;
        for (const auto& path : m_published)
            boost::filesystem::remove(path, ignored);
        keep_published();
    }

    void keep_published() { m_published.clear(); }

private:
    boost::filesystem::path m_directory;
    std::vector<boost::filesystem::path> m_published;
};

} // namespace Slic3r::GUI
