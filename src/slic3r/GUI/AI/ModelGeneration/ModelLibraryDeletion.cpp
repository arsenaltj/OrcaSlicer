#include "ModelLibraryDeletion.hpp"
#include "ModelLibraryRemovalState.hpp"
#include "ModelLibraryMetadata.hpp"
#include "ModelLibraryObservations.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <filesystem>
#include <stdexcept>
#include <cwchar>

namespace Slic3r::GUI {
namespace {
using Path = boost::filesystem::path;
using Json = nlohmann::json;
struct DeletionError : std::runtime_error { using std::runtime_error::runtime_error; };

bool valid_id(const std::string& id)
{
    return !id.empty() && id != "." && id != ".." && id.find('\0') == std::string::npos &&
        id.find_first_of("/\\:") == std::string::npos;
}
Path local_path(const Path& root, const Path& value)
{
    if (value.empty()) throw DeletionError("历史资源路径为空，未删除文件。");
    const auto absolute = value.is_absolute() ? value.lexically_normal() : (root / value).lexically_normal();
    auto base = root.begin(), part = absolute.begin();
    for (; base != root.end(); ++base, ++part) {
#ifdef _WIN32
        if (part == absolute.end() || ::_wcsicmp(base->native().c_str(), part->native().c_str()) != 0)
#else
        if (part == absolute.end() || *base != *part)
#endif
            throw DeletionError("历史资源路径不在模型文件夹内，未删除文件。");
    }
    Path relative;
    for (; part != absolute.end(); ++part) relative /= *part;
    if (relative.empty() || relative == "." || relative.has_root_path() ||
        std::any_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; }))
        throw DeletionError("历史资源路径不在模型文件夹内，未删除文件。");
    auto candidate = root;
    for (const auto& part : relative) {
        candidate /= part;
        boost::system::error_code error;
        const auto status = boost::filesystem::symlink_status(candidate, error);
        if (boost::filesystem::is_symlink(status) ||
            (error && error != boost::system::errc::no_such_file_or_directory))
            throw DeletionError("无法安全确认历史资源路径，未删除文件。");
    }
    return root / relative;
}
Path record_path(const Path& root, const std::string& id)
{
    if (!valid_id(id)) throw DeletionError("历史记录身份无效，未删除文件。");
    return local_path(root, root / "downloads" / ("orcaslicer-ai-" + id + ".json"));
}
Path receipt_path(Path record) { record.replace_extension(".removed"); return record; }
std::string text(const Path& path) { return std::filesystem::path(path.native()).generic_u8string(); }
struct Observation {
    bool exists {false};
    uintmax_t size {0};
    std::filesystem::file_time_type modified {};
    bool operator==(const Observation& other) const {
        return exists == other.exists && size == other.size && modified == other.modified;
    }
};
Observation observe(const Path& path)
{
    boost::system::error_code error;
    const auto status = boost::filesystem::status(path, error);
    // A missing optional metadata file is a valid legacy entry, not an I/O
    // failure. Boost can report ENOENT as well as file_not_found here.
    if (status.type() == boost::filesystem::file_not_found || error == boost::system::errc::no_such_file_or_directory)
        return {};
    if (error || !boost::filesystem::is_regular_file(status))
        throw DeletionError("无法读取模型或历史记录，文件已保留。");
    Observation result {true};
    result.size = boost::filesystem::file_size(path, error);
    if (error) throw DeletionError("无法读取历史资源大小，文件已保留。");
    std::error_code stamp_error;
    result.modified = std::filesystem::last_write_time(std::filesystem::path(path.native()), stamp_error);
    if (stamp_error) throw DeletionError("无法读取历史资源状态，文件已保留。");
    return result;
}
bool same_file(const Path& left, const Path& right)
{
    if (left.empty() || right.empty()) return false;
    if (left == right) return true;
    boost::system::error_code error;
    return boost::filesystem::equivalent(left, right, error) && !error;
}
Json owned_metadata(const Path& record)
{
    boost::filesystem::ifstream input(record, std::ios::binary);
    if (!input) return {};
    return Json::parse(input, [](int depth, Json::parse_event_t event, Json& value) {
        if (depth != 1 || event != Json::parse_event_t::key) return true;
        const auto& key = value.get_ref<const std::string&>();
        return key == "original_appearance" || key == "model_sha256" || key == "workbench_check_certificate";
    }, false);
}
}

bool ModelLibraryDeletion::appearance_companion(const Path& path)
{
    const auto name = text(path.filename());
    const std::string suffix = ".source-appearance.obj";
    return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool ModelLibraryDeletion::removed(const Path& library, const std::string& id, const Path& asset, bool design_only)
{
    try {
        const auto root = boost::filesystem::canonical(library);
        const auto receipt = local_path(root, receipt_path(record_path(root, id)));
        boost::system::error_code error;
        const auto size = boost::filesystem::file_size(receipt, error);
        if (error || size > 16 * 1024) return false;
        boost::filesystem::ifstream input(receipt, std::ios::binary);
        const auto data = Json::parse(input, nullptr, false);
        if (!data.is_object() || data.value("schema", "") != "orca.model-library-removal/v1" ||
            data.value("job_id", "") != id || data.value("design_only", false) != design_only) return false;
        ModelLibraryEntry selected, recorded;
        selected.job_id = recorded.job_id = id; selected.design_only = recorded.design_only = design_only;
        const auto stored = local_path(root, Path(std::filesystem::u8path(data.value("asset", "")).native()));
        if (design_only) { selected.preview_path = local_path(root, asset); recorded.preview_path = stored; }
        else { selected.model_path = local_path(root, asset); recorded.model_path = stored; }
        return ModelLibraryRemovalState(root).same(selected, recorded);
    } catch (...) { return false; }
}

ModelLibraryDeletion::Result ModelLibraryDeletion::remove(const Path& library, const ModelLibraryEntry& entry,
    const std::vector<ModelLibraryEntry>& entries, const std::function<bool()>& canceled,
    const std::function<void()>& committed)
{
    Result result;
    Path temporary;
    try {
        const auto root = boost::filesystem::canonical(library);
        const auto record = record_path(root, entry.job_id);
        const auto asset = local_path(root, entry.design_only ? entry.preview_path : entry.model_path);
        if (removed(root, entry.job_id, asset, entry.design_only)) {
            result.committed = true; if (committed) committed(); return result;
        }
        const auto before_asset = observe(asset), before_record = observe(record);
        const auto metadata = owned_metadata(record);
        const auto appearance = ModelLibraryMetadata::owned_original_appearance_companion(metadata, root);
        const auto observations = ModelLibraryObservations::owned_observations_companion(record, metadata);
        const auto check = ModelLibraryMetadata::preflight_deletion(root, record, asset,
            appearance.value_or(Path{}), canceled);
        if (canceled && canceled()) { result.canceled = true; return result; }
        if (!(observe(asset) == before_asset) || !(observe(record) == before_record)) {
            result.identity_changed = true;
            throw DeletionError("模型或历史记录已变化，删除已取消，请刷新后重试。");
        }

        // Commit the list removal before deleting any bytes. Referenced or
        // uncertain resources are retained unchanged; a bad unrelated history
        // record must not make a visible entry impossible to remove.
        const auto receipt = local_path(root, receipt_path(record));
        boost::filesystem::create_directories(receipt.parent_path());
        temporary = receipt.parent_path() / boost::filesystem::unique_path("history-removal-%%%%-%%%%.tmp");
        {
            boost::filesystem::ofstream output(temporary, std::ios::binary);
            output << Json {{"schema", "orca.model-library-removal/v1"}, {"job_id", entry.job_id},
                {"asset", text(asset.lexically_relative(root))}, {"design_only", entry.design_only}}.dump();
            output.close();
            if (!output) throw DeletionError("无法保存历史删除记录，原文件保持不变。");
        }
        if (canceled && canceled()) { result.canceled = true; boost::filesystem::remove(temporary); return result; }
        boost::filesystem::rename(temporary, receipt);
        temporary.clear();
        result.committed = true;
        // The durable list decision precedes best-effort byte cleanup. The
        // data callback only posts a GUI handoff and must never access controls.
        if (committed) committed();

        const auto shared = [&](const Path& target) {
            if (ModelLibraryMetadata::deletion_protects(check, root, target)) return true;
            for (const auto& other : entries) {
                if (other.job_id == entry.job_id && other.model_path == entry.model_path && other.design_only == entry.design_only) continue;
                for (const auto& path : {other.model_path, other.preview_path, other.reference_image_path,
                        other.ai_image_path, other.color_intent_path}) if (same_file(target, path)) return true;
            }
            return false;
        };
        // The exact record is also retained when another result needs its
        // model. Provenance/certificates must remain restorable after restart.
        const bool retain_asset = shared(asset);
        result.retained_dependencies = retain_asset;
        std::vector<Path> targets;
        if (!retain_asset) {
            auto draft = record;
            draft += ".draft";
            targets = {asset, record, draft};
            if (observations) targets.push_back(*observations);
            if (appearance) targets.push_back(*appearance);
            if (!entry.color_intent_path.empty()) targets.push_back(entry.color_intent_path);
            const auto parent = root / entry.job_id;
            // A stopped/failed task may retain both a model and a design.
            // Removing its independent model must not register the old design
            // as a new history card on the next scan.
            if (entry.design_only || asset.parent_path() == parent) targets.push_back(parent / "job.json");
            for (const auto& path : {entry.preview_path, entry.reference_image_path, entry.ai_image_path}) {
                if (path.empty()) continue;
                const auto relative = path.lexically_relative(parent);
                if (!relative.empty() && !relative.has_root_path() && *relative.begin() != "..") targets.push_back(path);
            }
            for (const auto* suffix : {"", "-raw", "-strict", "-clean", "-heatmap", "-input"})
                targets.push_back(root / "downloads" / ("orcaslicer-ai-" + entry.job_id + suffix + ".png"));
        }
        // No recursive directory removal: unrelated versions, materials and
        // exact provenance can share the original directory.
        for (const auto& path : targets) {
            const auto target = local_path(root, path);
            if (shared(target)) { result.retained_dependencies = true; continue; }
            boost::system::error_code error;
            if (boost::filesystem::remove(target, error)) ++result.removed_files;
            if (error) result.error = "历史记录已移除，部分文件未能清理，请检查文件权限。";
        }
        // Remove only an empty owned directory. In particular, leaving an
        // empty website directory would prevent a later explicit download.
        const auto parent = asset.parent_path();
        if (!retain_asset && (parent == root / entry.job_id || parent == root / "website" / entry.job_id)) {
            const auto directory = local_path(root, parent);
            boost::system::error_code error;
            if (boost::filesystem::is_directory(directory, error) && !error &&
                boost::filesystem::is_empty(directory, error) && !error)
                boost::filesystem::remove(directory, error);
        }
    } catch (const boost::filesystem::filesystem_error&) {
        result.error = result.committed ? "历史记录已移除，部分文件未能清理，请检查文件权限。" :
            "无法读取或写入历史删除记录，原文件保持不变，请检查文件权限。";
        if (!temporary.empty()) { boost::system::error_code ignored; boost::filesystem::remove(temporary, ignored); }
    } catch (const DeletionError& error) {
        result.error = result.committed ? "历史记录已移除，部分文件未能安全清理，文件已保留。" : error.what();
        if (!temporary.empty()) { boost::system::error_code ignored; boost::filesystem::remove(temporary, ignored); }
    } catch (const std::exception&) {
        result.error = result.committed ? "历史记录已移除，部分文件未能清理，文件已保留。" :
            "无法完成历史删除，原文件已保留，请刷新后重试。";
        if (!temporary.empty()) { boost::system::error_code ignored; boost::filesystem::remove(temporary, ignored); }
    }
    return result;
}
}
