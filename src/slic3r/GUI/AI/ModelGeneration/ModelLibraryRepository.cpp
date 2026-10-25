#include "ModelLibraryRepository.hpp"
#include "ModelLibraryMetadata.hpp"
#include "ModelLibraryThumbnail.hpp"
#include "ModelGenerationPresentation.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <wx/datetime.h>
#include <algorithm>
#include <map>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;
std::vector<ModelLibraryEntry> ModelLibraryRepository::read(
    const boost::filesystem::path& root, const std::atomic<bool>& cancelled)
{
    // Resolve the root before dispatch; no app configuration is read here.
    // The summary reader may maintain its cache beneath the supplied root.
    const boost::filesystem::path downloads = root / "downloads";
    const auto temp_path = [&downloads](const std::string& id, const std::string& extension) {
        return downloads / ("orcaslicer-ai-" + id + "." + extension);
    };
    const auto library_metadata_path = [&temp_path](const std::string& id) { return temp_path(id, "json"); };
    const auto is_supported_image = is_library_image_file;
    const auto library_image_path = [&is_supported_image](const nlohmann::json& data, const char* key,
                                                        const boost::filesystem::path& base) {
        const auto value = data.find(key);
        if (value == data.end() || !value->is_string() || value->get_ref<const std::string&>().empty())
            return boost::filesystem::path();
        const auto path = base / value->get<std::string>();
        return path_is_inside(base, path) && is_supported_image(path) ? path : boost::filesystem::path();
    };
    thread_local ModelLibraryMetadata summaries;
    const auto read_json = [](const boost::filesystem::path& path) {
        return summaries.read(path);
    };
    boost::system::error_code ec;
    if (cancelled || !boost::filesystem::is_directory(root, ec)) return {};

    std::map<std::string, boost::filesystem::path> models;
    for (boost::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (cancelled) return {};
        boost::system::error_code entry_ec;
        if (!boost::filesystem::is_directory(it->path(), entry_ec))
            continue;
        const std::string job_id = it->path().filename().string();
        if (job_id == "downloads" || job_id.rfind("attempt-", 0) == 0)
            continue;
        boost::filesystem::path model_path = it->path() / "model.glb";
        if (!is_nonempty_model(model_path)) model_path = it->path() / "model-vertex-color.obj";
        if (boost::filesystem::is_regular_file(model_path, entry_ec) &&
            boost::filesystem::file_size(model_path, entry_ec) > 0 && !entry_ec)
            models.emplace(job_id, model_path);
    }
    ec.clear();
    if (boost::filesystem::is_directory(downloads, ec)) {
        for (boost::filesystem::directory_iterator it(downloads, ec), end; !ec && it != end; it.increment(ec)) {
            if (cancelled) return {};
            const boost::filesystem::path path = it->path();
            boost::system::error_code entry_ec;
            if (!boost::filesystem::is_regular_file(path, entry_ec) || !AI::is_model_artifact(path))
                continue;
            const std::string job_id = download_job_id(path);
            if (job_id.rfind("finish-", 0) == 0 && !read_json(library_metadata_path(job_id)).is_object())
                continue; // Unaccepted finishing previews are never history entries.
            if (!job_id.empty() && boost::filesystem::file_size(path, entry_ec) > 0 && !entry_ec)
                models.emplace(job_id, path);
        }
    }

    // A finishing OBJ stays beside its source so relative materials remain
    // valid. Accepted metadata, not its temporary filename, registers it.
    const auto records = library_metadata_path("placeholder").parent_path();
    ec.clear();
    if (boost::filesystem::is_directory(records, ec)) {
        for (boost::filesystem::directory_iterator it(records, ec), end; !ec && it != end; it.increment(ec)) {
            if (cancelled) return {};
            if (it->path().extension() != ".json") continue;
            // The summary index lives beside history metadata. It is an
            // implementation detail, never a model-library record; skipping
            // it also avoids recursively parsing our own cache on every scan.
            if (it->path().filename() == "library-summary-cache-v1.json") continue;
            const auto data = read_json(it->path());
            if (!data.is_object() || data.value("source", std::string()) != "local_finishing") continue;
            const auto id = data.value("job_id", std::string());
            const auto path = root / data.value("model_path", std::string());
            if (id.rfind("finish-", 0) == 0 && path_is_inside(root, path) && is_nonempty_model(path))
                models[id] = path;
        }
    }
    std::vector<ModelLibraryEntry> entries;
    entries.reserve(models.size());
    ec.clear();
    for (boost::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (cancelled) return {};
        const auto job_id = it->path().filename().string();
        if (models.count(job_id)) continue;
        const auto design = read_design_history_entry(root, job_id, false);
        if (!design) continue;
        ModelLibraryEntry entry;
        entry.design_only = true;
        entry.job_id = job_id;
        entry.generated_at = design->generated_at;
        entry.reference_image_path = design->input_path;
        entry.preview_path = design->preview_path;
        entry.ai_image_path = design->raw_preview_path;
        entry.title = wxString::FromUTF8(design->prompt);
        if (entry.title.empty()) entry.title = _L("AI 设计图 ") + wxString::FromUTF8(job_id.substr(0, 8));
        if (entry.title.length() > 32) entry.title = entry.title.Left(32) + _L("…");
        const wxDateTime generated(entry.generated_at);
        entry.details = generated.IsValid() ? generated.FormatISODate() + " " + generated.FormatISOTime() : _L("未知时间");
        entry.details += design->state == "awaiting_confirmation" ? _L(" · 设计待确认")
            : design->state == "stopped" ? _L(" · 已停止，设计图已保留") : _L(" · 未完成，设计图已保留");
        entry.details += _L("\n素材：");
        entry.details += entry.reference_image_path.empty() ? _L("无参考图") : _L("原图 ✓");
        entry.details += _L(" · AI 图 ✓ · 尚无 3D 模型");
        entry.search_text = entry.title + "\n" + entry.details;
        entries.emplace_back(std::move(entry));
    }
    for (const auto& [job_id, model_path] : models) {
        if (cancelled) return {};
        boost::system::error_code entry_ec;
        ModelLibraryEntry entry;
        entry.job_id = job_id;
        entry.model_path = model_path;
        entry.generated_at = boost::filesystem::last_write_time(model_path, entry_ec);
        if (entry_ec)
            entry.generated_at = 0;

        const nlohmann::json metadata = read_json(library_metadata_path(job_id));
        if (metadata.is_object()) {
            entry.accepted_finishing = job_id.rfind("finish-", 0) == 0 &&
                metadata.value("source", std::string()) == "local_finishing";
            entry.generated_at = metadata.value("generated_at", entry.generated_at);
            entry.imported_at = metadata.value("imported_at", std::time_t {0});
            entry.triangle_count = metadata.value("triangle_count", size_t {0});
            entry.load_seconds = metadata.value("load_seconds", 0.0);
            entry.print_feedback = metadata.value("print_feedback", std::string());
            entry.use_printable_colors = metadata.value("use_printable_colors", false);
            const std::string provider = metadata.value("provider", std::string());
            const std::string provider_task_id = metadata.value("provider_task_id", std::string());
            const std::string provider_conversion_task_id =
                metadata.value("provider_conversion_task_id", std::string());
            if ((provider == "tripo" || provider == "yoyoclaw") && valid_provider_task_id(provider_task_id)) {
                entry.provider_name = provider;
                entry.provider_task_id = provider_task_id;
                if (valid_provider_task_id(provider_conversion_task_id))
                    entry.provider_conversion_task_id = provider_conversion_task_id;
            }
            std::string prompt = metadata.value("prompt", std::string());
            if (prompt == INTERNAL_DEFAULT_IMAGE_INSTRUCTION)
                prompt.clear();
            if (!prompt.empty()) {
                entry.title = wxString::FromUTF8(prompt);
                if (entry.title.length() > 32)
                    entry.title = entry.title.Left(32) + _L("…");
            }
            const auto palette = metadata.find("palette");
            if (palette != metadata.end() && palette->is_array()) {
                for (const auto& color : *palette) {
                    if (color.is_string())
                        entry.palette.push_back(color.get<std::string>());
                }
            }
            const auto roles = metadata.find("palette_roles");
            if (roles != metadata.end() && roles->is_object()) {
                for (const char* role : PALETTE_ROLE_IDS) {
                    const auto color = roles->find(role);
                    if (color != roles->end() && color->is_string())
                        entry.palette_roles.emplace(role, color->get<std::string>());
                }
            }
            entry.reference_image_path = library_image_path(metadata, "reference_image_path", root);
            entry.ai_image_path = library_image_path(metadata, "ai_image_path", root);
            const auto color_intent_path = metadata.find("color_intent_path");
            if (color_intent_path != metadata.end() && color_intent_path->is_string()) {
                const boost::filesystem::path candidate = root / color_intent_path->get<std::string>();
                if (path_is_inside(root, candidate) && boost::filesystem::is_regular_file(candidate, entry_ec))
                    entry.color_intent_path = candidate;
            }
            const auto color_intent_schema = metadata.find("color_intent_schema");
            const auto color_intent_sha256 = metadata.find("color_intent_sha256");
            if (color_intent_schema != metadata.end() && color_intent_schema->is_string())
                entry.color_intent_schema = color_intent_schema->get<std::string>();
            if (color_intent_sha256 != metadata.end() && color_intent_sha256->is_string())
                entry.color_intent_sha256 = color_intent_sha256->get<std::string>();
        }

        const boost::filesystem::path job_preview = root / job_id / "preview.png";
        const boost::filesystem::path download_preview = temp_path(job_id, "png");
        entry_ec.clear();
        if (boost::filesystem::is_regular_file(job_preview, entry_ec))
            entry.preview_path = job_preview;
        else {
            entry_ec.clear();
            if (boost::filesystem::is_regular_file(download_preview, entry_ec))
                entry.preview_path = download_preview;
        }
        if (entry.reference_image_path.empty()) {
            const boost::filesystem::path legacy_input = temp_path(job_id + "-input", "png");
            if (is_supported_image(legacy_input) && path_is_inside(root, legacy_input))
                entry.reference_image_path = legacy_input;
        }
        if (entry.ai_image_path.empty()) {
            const boost::filesystem::path legacy_raw = temp_path(job_id + "-raw", "png");
            if (is_supported_image(legacy_raw) && path_is_inside(root, legacy_raw))
                entry.ai_image_path = legacy_raw;
            else if (is_supported_image(entry.preview_path) && path_is_inside(root, entry.preview_path))
                entry.ai_image_path = entry.preview_path;
        }

        if (entry.palette.empty()) {
            const nlohmann::json preview_colors = read_json(root / job_id / "preview-colors.json");
            if (preview_colors.is_object() && preview_colors.value("palette_constrained", true)) {
                const auto pixels = preview_colors.find("palette_pixels");
                if (pixels != preview_colors.end() && pixels->is_object()) {
                    for (auto color = pixels->begin(); color != pixels->end(); ++color)
                        entry.palette.push_back(color.key());
                }
            }
            entry.use_printable_colors = !entry.palette.empty();
        }
        for (auto role = entry.palette_roles.begin(); role != entry.palette_roles.end();) {
            const bool matches_palette = std::any_of(
                entry.palette.begin(), entry.palette.end(), [&role](const std::string& color) {
                    return same_palette_color(color, role->second);
                });
            if (!matches_palette)
                role = entry.palette_roles.erase(role);
            else
                ++role;
        }
        if (entry.palette_roles.empty())
            entry.palette_roles = automatic_palette_roles(entry.palette);

        if (entry.title.empty())
            entry.title = _L("AI 模型 ") + wxString::FromUTF8(job_id.substr(0, std::min<size_t>(8, job_id.size())));
        wxDateTime generated(entry.generated_at);
        const wxString date = generated.IsValid()
            ? generated.FormatISODate() + " " + generated.FormatISOTime()
            : _L("未知时间");
        entry_ec.clear();
        const auto model_size = boost::filesystem::file_size(model_path, entry_ec);
        const double megabytes = entry_ec ? 0.0 : double(model_size) / (1024.0 * 1024.0);
        entry.details = date + wxString::Format(_L(" · %.1f MB · "), megabytes) +
            (entry.use_printable_colors
                ? wxString::Format(_L("%llu 种可打印颜色"), static_cast<unsigned long long>(entry.palette.size()))
                : _L("自然颜色"));
        if (entry.triangle_count > 0)
            entry.details += wxString::Format(
                _L(" · %.1f 万面"), static_cast<double>(entry.triangle_count) / 10000.0);
        if (entry.load_seconds > 0.0)
            entry.details += wxString::Format(_L(" · 上次加载 %.2f 秒"), entry.load_seconds);
        entry.details += _L("\n素材：");
        entry.details += entry.reference_image_path.empty() ? _L("原图未保存") : _L("原图 ✓");
        entry.details += _L(" · ");
        entry.details += entry.ai_image_path.empty() ? _L("AI 图未保存") : _L("AI 图 ✓");
        if (!entry.color_intent_path.empty() || !entry.color_intent_schema.empty() ||
            !entry.color_intent_sha256.empty())
            entry.details += AI::is_valid_color_intent_manifest_ref(
                {entry.color_intent_path.string(), entry.color_intent_schema, entry.color_intent_sha256})
                ? _L(" · 颜色意图 ✓") : _L(" · 颜色意图无效");
        if (entry.imported_at > 0) {
            entry.details += _L("\n已导入准备页");
            if (entry.print_feedback == "success")
                entry.details += _L(" · 打印反馈：成功");
            else if (entry.print_feedback == "issue")
                entry.details += _L(" · 打印反馈：有问题");
            else
                entry.details += _L(" · 尚未记录打印结果");
        }

        entry.search_text = entry.title + "\n" + entry.details;
        entries.emplace_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const ModelLibraryEntry& lhs, const ModelLibraryEntry& rhs) {
        if (lhs.generated_at != rhs.generated_at)
            return lhs.generated_at > rhs.generated_at;
        return lhs.job_id < rhs.job_id;
    });
    return entries;
}

} // namespace Slic3r::GUI
