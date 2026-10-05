#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/AI/AIWindowAppearance.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "ModelLibraryThumbnail.hpp"
#include "WorkbenchStyle.hpp"
#include "ModelGenerationPresentation.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <map>
#include <limits>
#include <mutex>
#include <optional>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/datetime.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/notebook.h>
#include <wx/log.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/weakref.h>
#include <wx/utils.h>
#include <wx/wupdlock.h>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;

std::vector<ModelGenerationPanel::GeneratedModelEntry> ModelGenerationPanel::read_library_entries(
    const boost::filesystem::path& root, const std::atomic<bool>& cancelled)
{
    // This snapshot reader runs on the history worker. Resolve the output root
    // before dispatch and do not read app configuration or create directories.
    const boost::filesystem::path downloads = root / "downloads";
    const auto temp_path = [&downloads](const std::string& id, const std::string& extension) {
        return downloads / ("orcaslicer-ai-" + id + "." + extension);
    };
    const auto library_metadata_path = [&temp_path](const std::string& id) { return temp_path(id, "json"); };
    const auto library_history_index_path = [&library_metadata_path](const std::string& id) {
        auto path = library_metadata_path(id);
        path.replace_extension(".history.json");
        return path;
    };
    const auto is_supported_image = is_library_image_file;
    const auto library_image_path = [&is_supported_image](const nlohmann::json& data, const char* key,
                                                        const boost::filesystem::path& base) {
        const auto value = data.find(key);
        if (value == data.end() || !value->is_string() || value->get_ref<const std::string&>().empty())
            return boost::filesystem::path();
        const auto path = base / value->get<std::string>();
        return path_is_inside(base, path) && is_supported_image(path) ? path : boost::filesystem::path();
    };
    const auto read_json = [](const boost::filesystem::path& path) {
        boost::system::error_code error;
        const auto bytes = boost::filesystem::file_size(path, error);
        if (error || bytes > 1024 * 1024)
            return nlohmann::json();
        nlohmann::json value = ModelGenerationPresentation::read_json(path);
        if (!value.is_object())
            return value;

        // A single hand-edited or partially-written metadata file must not
        // abort the complete history scan.  The model path remains useful even
        // when optional metadata has the wrong JSON type; remove only fields
        // whose typed access below would otherwise throw.
        const auto erase_unless = [&value](const char* key, const auto& predicate) {
            const auto it = value.find(key);
            if (it != value.end() && !predicate(*it))
                value.erase(it);
        };
        erase_unless("generated_at", [](const nlohmann::json& item) {
            return item.is_number_integer() || item.is_number_unsigned();
        });
        erase_unless("imported_at", [](const nlohmann::json& item) {
            return item.is_number_integer() || item.is_number_unsigned();
        });
        erase_unless("triangle_count", [](const nlohmann::json& item) {
            return item.is_number_integer() || item.is_number_unsigned();
        });
        erase_unless("load_seconds", [](const nlohmann::json& item) { return item.is_number(); });
        for (const char* key : {"schema", "model_sha256", "print_feedback", "provider", "provider_task_id",
                                "provider_conversion_task_id", "prompt", "reference_image_path",
                                "ai_image_path", "color_intent_path", "color_intent_schema",
                                "color_intent_sha256", "job_id", "model_path", "source"}) {
            erase_unless(key, [](const nlohmann::json& item) { return item.is_string(); });
        }
        erase_unless("use_printable_colors", [](const nlohmann::json& item) { return item.is_boolean(); });
        erase_unless("palette_constrained", [](const nlohmann::json& item) { return item.is_boolean(); });
        return value;
    };
    const auto read_finishing_index = [&](const std::string& id) {
        const auto index = read_json(library_history_index_path(id));
        const std::string hash = index.is_object() ? index.value("model_sha256", std::string()) : std::string();
        if (!index.is_object() || index.value("schema", std::string()) != "orca.local-finishing-history/v1" ||
            index.value("source", std::string()) != "local_finishing" ||
            index.value("job_id", std::string()) != id ||
            hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return std::isxdigit(c); }))
            return nlohmann::json();
        const auto model_path = root / index.value("model_path", std::string());
        if (!path_is_inside(root, model_path) || !is_nonempty_model(model_path) ||
            AI::model_artifact_sha256(model_path) != hash)
            return nlohmann::json();
        return index;
    };
    const auto read_accepted_finishing_metadata = [&](const std::string& id) {
        auto metadata = read_finishing_index(id);
        if (metadata.is_object()) return metadata;
        metadata = read_json(library_metadata_path(id));
        if (!metadata.is_object() || metadata.contains("history_index_required") ||
            metadata.value("source", std::string()) != "local_finishing" ||
            metadata.value("job_id", std::string()) != id)
            return nlohmann::json();
        const auto model_path = root / metadata.value("model_path", std::string());
        return path_is_inside(root, model_path) && is_nonempty_model(model_path)
            ? metadata : nlohmann::json();
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
            if (job_id.rfind("finish-", 0) == 0 &&
                !read_accepted_finishing_metadata(job_id).is_object())
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
            const auto data = read_json(it->path());
            if (!data.is_object() || data.value("source", std::string()) != "local_finishing") continue;
            const auto id = data.value("job_id", std::string());
            if ((data.contains("schema") || data.contains("history_index_required")) &&
                !read_finishing_index(id).is_object()) continue;
            const auto path = root / data.value("model_path", std::string());
            if (id.rfind("finish-", 0) == 0 && path_is_inside(root, path) && is_nonempty_model(path))
                models[id] = path;
        }
    }
    std::vector<GeneratedModelEntry> entries;
    entries.reserve(models.size());
    ec.clear();
    for (boost::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (cancelled) return {};
        const auto job_id = it->path().filename().string();
        if (models.count(job_id)) continue;
        const auto design = read_design_history_entry(root, job_id, false);
        if (!design) continue;
        GeneratedModelEntry entry;
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
        entries.emplace_back(std::move(entry));
    }
    for (const auto& [job_id, model_path] : models) {
        if (cancelled) return {};
        boost::system::error_code entry_ec;
        GeneratedModelEntry entry;
        entry.job_id = job_id;
        entry.model_path = model_path;
        entry.generated_at = boost::filesystem::last_write_time(model_path, entry_ec);
        if (entry_ec)
            entry.generated_at = 0;

        nlohmann::json metadata = job_id.rfind("finish-", 0) == 0
            ? read_accepted_finishing_metadata(job_id) : read_json(library_metadata_path(job_id));
        if (metadata.is_object()) {
            entry.accepted_finishing = job_id.rfind("finish-", 0) == 0;
            entry.generated_at = metadata.value("generated_at", entry.generated_at);
            entry.imported_at = metadata.value("imported_at", std::time_t {0});
            entry.triangle_count = metadata.value("triangle_count", size_t {0});
            const auto colors = metadata.find("color_count");
            if (colors != metadata.end() && colors->is_number_integer() && *colors >= 0)
                entry.color_count = colors->get<size_t>();
            entry.load_seconds = metadata.value("load_seconds", 0.0);
            entry.print_feedback = metadata.value("print_feedback", std::string());
            entry.use_printable_colors = metadata.value("use_printable_colors", false);
            const std::string provider = metadata.value("provider", std::string());
            const std::string provider_task_id = metadata.value("provider_task_id", std::string());
            const std::string provider_conversion_task_id =
                metadata.value("provider_conversion_task_id", std::string());
            if ((provider == "tripo" || provider == "hunyuan") && valid_provider_task_id(provider_task_id)) {
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

        entries.emplace_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const GeneratedModelEntry& lhs, const GeneratedModelEntry& rhs) {
        if (lhs.generated_at != rhs.generated_at)
            return lhs.generated_at > rhs.generated_at;
        return lhs.job_id < rhs.job_id;
    });
    return entries;
}

namespace {

// Keep thumbnail painting in one fixed, clipped surface, independently of the
// native title/button controls and the adjacent notebook's OpenGL preview.
class LibraryThumbnail final : public wxPanel
{
public:
    explicit LibraryThumbnail(wxWindow* parent, bool workbench = false) : wxPanel(parent), m_workbench(workbench)
    {
        SetMinSize(FromDIP(wxSize(96, 96)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        if (m_workbench) SetWindowStyleFlag(GetWindowStyleFlag() | wxCLIP_CHILDREN);
        if (m_workbench) m_placeholder = create_scaled_bitmap("workbench_object", this, 36);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetBackgroundColour()));
            dc.Clear();
            const auto size = GetClientSize();
            if (m_bitmap.IsOk()) {
                dc.DrawBitmap(m_bitmap, (size.x - m_bitmap.GetLogicalWidth()) / 2,
                              (size.y - m_bitmap.GetLogicalHeight()) / 2, true);
            } else if (m_workbench) {
                if (m_placeholder.IsOk())
                    dc.DrawBitmap(m_placeholder, (size.x - m_placeholder.GetLogicalWidth()) / 2,
                                  (size.y - m_placeholder.GetLogicalHeight()) / 2, true);
            } else {
                dc.SetFont(GetFont());
                dc.SetTextForeground(GetForegroundColour());
                dc.DrawLabel(m_pending ? _L("加载缩略图…") : _L("无缩略图"), wxRect(size), wxALIGN_CENTER);
            }
            if (m_inspecting) {
                dc.SetFont(GetFont());
                const int title_height = dc.GetCharHeight();
                const wxFont detail_font = GetFont().Smaller();
                dc.SetFont(detail_font);
                const int detail_height = dc.GetCharHeight();
                const int footer_overlap = m_workbench ? FromDIP(24) : 0;
                const int top = std::max(0, size.y - footer_overlap - title_height - detail_height - FromDIP(4));
                dc.SetBrush(wxBrush(GetBackgroundColour()));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRectangle(0, top, size.x, size.y - top);
                dc.SetTextForeground(wxColour(238, 240, 244));
                dc.SetFont(GetFont());
                dc.DrawText(wxControl::Ellipsize(m_title, dc, wxELLIPSIZE_END, size.x), 0, top);
                dc.SetFont(detail_font);
                dc.SetTextForeground(wxColour(158, 164, 174));
                dc.DrawText(wxControl::Ellipsize(m_details, dc, wxELLIPSIZE_END, size.x), 0, top + title_height);
            }
        });
    }

    void set_image(const wxImage& image)
    {
        m_pending = false;
        wxImage fitted = image;
        if (m_workbench && image.IsOk()) {
            const auto available = GetClientSize();
            const auto reference = FromDIP(wxSize(75, 90));
            const wxSize bounds(std::max(1, std::min(reference.x, available.x)),
                                std::max(1, std::min(reference.y, available.y)));
            const double scale = std::min(double(bounds.x) * GetContentScaleFactor() / image.GetWidth(),
                                          double(bounds.y) * GetContentScaleFactor() / image.GetHeight());
            fitted = image.Scale(std::max(1, int(image.GetWidth() * scale)),
                                 std::max(1, int(image.GetHeight() * scale)), wxIMAGE_QUALITY_HIGH);
        }
        m_bitmap = fitted.IsOk() ? wxBitmap(fitted) : wxBitmap();
        if (m_bitmap.IsOk()) m_bitmap.SetScaleFactor(GetContentScaleFactor());
        else SetToolTip(_L("缩略图不可用（文件缺失、损坏或过大），可尝试双击打开资产。"));
        Refresh(false);
    }

    void finish_pending()
    {
        if (m_pending) set_image(wxImage());
    }

    void set_details(const wxString& title, const wxString& details)
    {
        m_title = title;
        m_details = details;
        SetName(title);
    }

    void set_inspecting(bool inspecting)
    {
        if (m_inspecting == inspecting) return;
        m_inspecting = inspecting;
        Refresh(false);
    }

private:
    wxBitmap m_bitmap;
    wxBitmap m_placeholder;
    bool m_pending {true};
    bool m_workbench {false};
    bool m_inspecting {false};
    wxString m_title, m_details;
};

} // namespace

struct ModelGenerationPanel::LibraryLoadState
{
    struct Request {
        uint64_t revision {0};
        bool scan {false};
        boost::filesystem::path root;
        std::vector<GeneratedModelEntry> entries;
        int edge {96};
        std::atomic<bool> cancelled {false};
    };
    struct Snapshot {
        uint64_t revision;
        std::vector<GeneratedModelEntry> entries;
        bool failed {false};
    };
    using Thumbnail = LibraryThumbnailPixels;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping {false};
    std::shared_ptr<Request> pending, active;
    std::optional<Snapshot> snapshot;
    std::vector<Thumbnail> thumbnails;
};

void ModelGenerationPanel::start_library_worker()
{
    if (m_library_load_state) return;
    m_library_load_state = std::make_shared<LibraryLoadState>();
    m_library_worker = std::thread([state = m_library_load_state] {
        ModelLibraryThumbnailCache cache;
        for (;;) {
            std::shared_ptr<LibraryLoadState::Request> request;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->wake.wait(lock, [&] { return state->stopping || state->pending; });
                if (state->stopping) return;
                request = std::move(state->pending);
                state->active = request;
            }
            if (request->scan) {
                LibraryLoadState::Snapshot snapshot {request->revision, {}};
                try { snapshot.entries = read_library_entries(request->root, request->cancelled); }
                catch (...) { snapshot.failed = true; }
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->stopping && !request->cancelled) state->snapshot = std::move(snapshot);
            } else {
                for (size_t index = 0; index < request->entries.size() && !request->cancelled; ++index) {
                    const auto& entry = request->entries[index];
                    wxImage image;
                    try { image = cache.load(entry.ai_image_path, entry.reference_image_path, request->edge, request->cancelled); }
                    catch (...) { /* An unavailable thumbnail never removes an asset. */ }
                    LibraryLoadState::Thumbnail thumbnail {request->revision, index};
                    if (image.IsOk() && !request->cancelled) {
                        thumbnail.width = image.GetWidth();
                        thumbnail.height = image.GetHeight();
                        const size_t pixels = size_t(thumbnail.width) * thumbnail.height;
                        thumbnail.rgb.assign(image.GetData(), image.GetData() + pixels * 3);
                        if (image.HasAlpha()) thumbnail.alpha.assign(image.GetAlpha(), image.GetAlpha() + pixels);
                    }
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->stopping || request->cancelled) break;
                    state->thumbnails.push_back(std::move(thumbnail));
                }
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->active == request) state->active.reset();
            }
        }
    });
}

void ModelGenerationPanel::cancel_library_loading()
{
    m_library_scan_pending = false;
    ++m_library_revision;
    m_library_refresh_pending = true;
    m_library_timer.Stop();
    if (!m_library_load_state) return;
    std::lock_guard<std::mutex> lock(m_library_load_state->mutex);
    if (m_library_load_state->active) m_library_load_state->active->cancelled = true;
    if (m_library_load_state->pending) m_library_load_state->pending->cancelled = true;
    m_library_load_state->pending.reset();
    m_library_load_state->snapshot.reset();
    m_library_load_state->thumbnails.clear();
}

void ModelGenerationPanel::stop_library_loading()
{
    cancel_library_loading();
    if (!m_library_load_state) return;
    {
        std::lock_guard<std::mutex> lock(m_library_load_state->mutex);
        m_library_load_state->stopping = true;
    }
    m_library_load_state->wake.notify_one();
    // Cancellation is checked between files. Each decode is bounded by file
    // bytes and pixel dimensions, and no worker callback owns a wxWindow.
    if (m_library_worker.joinable()) m_library_worker.join();
    m_library_load_state.reset();
}

void ModelGenerationPanel::load_library_entries()
{
    if (m_shutdown) return;
    const bool library_visible = m_library_scroller && m_library_scroller->IsShownOnScreen();
    const bool workbench_visible = m_workbench_shell && m_workbench_shell->IsShownOnScreen();
    if (!m_page_initialized || (!library_visible && !workbench_visible)) {
        cancel_library_loading();
        return;
    }
    cancel_library_loading();
    start_library_worker();
    auto request = std::make_shared<LibraryLoadState::Request>();
    request->revision = m_library_revision;
    request->scan = true;
    m_library_scan_pending = true;
    request->root = ModelGenerationPresentation::generated_models_root();
    {
        std::lock_guard<std::mutex> lock(m_library_load_state->mutex);
        m_library_load_state->pending = std::move(request);
    }
    m_library_empty->SetLabel(_L("正在读取历史资产…"));
    m_library_empty->Show();
    m_library_previous->Disable();
    m_library_next->Disable();
    m_library_scroller->GetParent()->Layout();
    m_library_load_state->wake.notify_one();
    m_library_timer.Start(50);
}

void ModelGenerationPanel::on_library_timer(wxTimerEvent&)
{
    if (m_shutdown || !m_library_load_state) return;
    const bool library_visible = m_library_scroller && m_library_scroller->IsShownOnScreen();
    const bool workbench_visible = m_workbench_shell && m_workbench_shell->IsShownOnScreen();
    if (!library_visible && !workbench_visible) {
        cancel_library_loading();
        return;
    }
    std::optional<LibraryLoadState::Snapshot> snapshot;
    std::vector<LibraryLoadState::Thumbnail> thumbnails;
    {
        std::lock_guard<std::mutex> lock(m_library_load_state->mutex);
        snapshot = std::move(m_library_load_state->snapshot);
        m_library_load_state->snapshot.reset();
        thumbnails.swap(m_library_load_state->thumbnails);
    }
    if (snapshot && snapshot->revision == m_library_revision) {
        m_library_scan_pending = false;
        if (snapshot->failed) {
            m_library_empty->SetLabel(_L("历史资产读取失败，已保留当前列表。请点击刷新重试。"));
            m_library_empty->Show();
            m_library_previous->Enable(m_library_page > 0);
            m_library_next->Enable((m_library_page + 1) * m_library_page_size < m_library_entries.size());
            for (auto* thumbnail : m_library_thumbnails)
                static_cast<LibraryThumbnail*>(thumbnail)->finish_pending();
        } else {
            const size_t anchor_index = m_library_page * m_library_page_size;
            const std::string anchor = anchor_index < m_library_entries.size() ? m_library_entries[anchor_index].job_id : std::string();
            const auto position = std::find_if(snapshot->entries.begin(), snapshot->entries.end(),
                [&](const GeneratedModelEntry& entry) { return entry.job_id == anchor; });
            if (!anchor.empty() && position != snapshot->entries.end())
                m_library_page = size_t(position - snapshot->entries.begin()) / m_library_page_size;
            m_library_entries = std::move(snapshot->entries);
            refresh_library();
        }
    }
    for (const auto& thumbnail : thumbnails) {
        const auto image = receive_library_thumbnail(thumbnail, m_library_revision, m_library_thumbnail_targets.size());
        if (image)
            static_cast<LibraryThumbnail*>(m_library_thumbnail_targets[thumbnail.index])->set_image(*image);
    }
    // DPI changes invalidate only the visible page; the worker cache keys
    // include pixel size, and themes repaint via the normal AI appearance path.
    if (library_visible && !m_library_refresh_pending &&
        (m_library_thumbnail_edge != int(FromDIP(96) * GetContentScaleFactor()) ||
         m_library_layout_width != m_library_scroller->GetClientSize().x)) refresh_library();
}

void ModelGenerationPanel::request_library_thumbnails()
{
    cancel_library_loading();
    start_library_worker();
    m_library_refresh_pending = false;
    auto request = std::make_shared<LibraryLoadState::Request>();
    request->revision = m_library_revision;
    request->edge = m_library_thumbnail_edge;
    const auto begin = std::min(m_library_page * m_library_page_size, m_library_entries.size());
    const auto end = std::min(begin + m_library_page_size, m_library_entries.size());
    m_library_thumbnail_targets.clear();
    if (m_library_scroller->IsShownOnScreen()) {
        request->entries.assign(m_library_entries.begin() + begin, m_library_entries.begin() + end);
        m_library_thumbnail_targets = m_library_thumbnails;
    } else {
        request->edge = int(FromDIP(112) * GetContentScaleFactor());
        for (size_t index : m_workbench_thumbnail_entries) request->entries.push_back(m_library_entries[index]);
        m_library_thumbnail_targets = m_workbench_thumbnails;
    }
    {
        std::lock_guard<std::mutex> lock(m_library_load_state->mutex);
        m_library_load_state->pending = std::move(request);
    }
    m_library_load_state->wake.notify_one();
    m_library_timer.Start(50);
}

void ModelGenerationPanel::persist_generation_options()
{
    if (m_shutdown || m_busy || m_restoring_input || !m_awaiting_confirmation || m_job_id.empty() ||
        !job_inputs_match() || use_printable_colors() != m_job_use_printable_colors ||
        (use_printable_colors() && current_palette() != m_job_palette)) {
        refresh_controls();
        return;
    }
    if (!generation_options_valid()) {
        refresh_controls();
        show_input_hint(_L("当前设置尚未保存：200 万面需要选择精细几何。"));
        return;
    }
    const auto previous = m_job_generation_options;
    const auto requested = current_generation_options();
    const uint64_t sequence = m_sequence;
    const std::string job_id = m_job_id;
    m_saving_generation_options = true;
    m_busy = true;
    refresh_controls();
    m_status->SetLabel(_L("正在保存 3D 生成设置..."));
    wxWeakRef<ModelGenerationPanel> weak(this);
    const auto finish = [weak, sequence, job_id](AIModelGenerationClient::GenerationOptions options, wxString error) {
        if (!weak) return;
        wxGetApp().CallAfter([weak, sequence, job_id, options = std::move(options), error = std::move(error)] {
            if (!weak || weak->m_shutdown || sequence != weak->m_sequence || weak->m_job_id != job_id) return;
            weak->m_job_generation_options = options;
            weak->m_job_face_limit = options.face_limit;
            weak->m_job_generation_profile = options.face_limit <= 300000 ? "performance" : "quality";
            weak->m_provider->SetSelection(options.provider == "hunyuan" ? 1 : 0);
            weak->refresh_provider_options();
            weak->m_quality->SetSelection(options.face_limit <= 300000 ? 0 : options.face_limit == 2000000 && weak->m_quality->GetCount() == 3 ? 2 : 1);
            weak->m_geometry_quality->SetSelection(weak->m_geometry_quality->GetCount() > 1 && options.geometry_quality == "detailed" ? 1 : 0);
            weak->m_texture_quality->SetSelection(weak->m_texture_quality->GetCount() == 1 ? 0 : options.texture_quality == "extreme" ? 2 : options.texture_quality == "detailed" ? 1 : 0);
            weak->m_output_format->SetSelection(options.output_format == "obj" ? 1 : 0);
            weak->m_saving_generation_options = false;
            weak->m_busy = false;
            weak->refresh_controls();
            weak->show_input_hint(error.empty() ? _L("3D 生成设置已保存。")
                : _L("保存未确认，界面已恢复先前设置；请重新打开设计记录核对：") + error);
        });
    };
    m_client.update_generation_options(job_id, requested,
        [finish, previous, job_id](AIModelGenerationClient::JobStatus status) {
            if (status.id != job_id || status.state != "awaiting_confirmation") {
                finish(previous, _L("任务状态已变化，请重新打开设计记录。"));
                return;
            }
            finish(std::move(status.generation_options), wxString());
        },
        [finish, previous](std::string error) {
            finish(previous, wxString::FromUTF8(error));
        });
}

void ModelGenerationPanel::load_design_library_entry(const std::string& job_id)
{
    if (m_busy || m_design_history_loading || m_finishing_running || m_shutdown) return;
    if (!m_service_available || !ModelGenerationPresentation::read_design_history_entry(
            ModelGenerationPresentation::generated_models_root(), job_id)) {
        m_status->SetLabel(_L("设计记录或本地 AI 服务不可用，当前内容已保留。"));
        return;
    }
    const uint64_t sequence = m_sequence;
    const uint64_t history_sequence = ++m_design_history_sequence;
    m_design_history_loading = true;
    m_busy = true;
    refresh_controls();
    m_status->SetLabel(_L("正在恢复历史设计图..."));
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.get_status(job_id,
        [weak, sequence, history_sequence, job_id](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, history_sequence, job_id, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    history_sequence != weak->m_design_history_sequence) return;
                weak->m_design_history_loading = false;
                weak->m_busy = false;
                if (status.id != job_id ||
                    (status.state != "awaiting_confirmation" && status.state != "stopped" && status.state != "failed") ||
                    (!status.preview_ready && !status.raw_preview_ready && !status.model_reference_ready) ||
                    (status.state == "awaiting_confirmation" && status.source == "image" && !status.input_ready)) {
                    weak->refresh_controls();
                    weak->m_status->SetLabel(_L("设计记录不完整或状态已变化，当前内容已保留。"));
                    return;
                }
                // Reopening the current pending design after reconnect is a
                // read refresh: keep local input and unsubmitted quality choices.
                if (status.id == weak->m_job_id && weak->m_awaiting_confirmation &&
                    status.state == "awaiting_confirmation" && !weak->m_model_preview_ready && weak->m_style_preview_ready) {
                    weak->handle_status(std::move(status), sequence);
                    if (weak->m_preview_book) weak->m_preview_book->SetSelection(0);
                    return;
                }
                // Commit navigation only after the persisted job was read successfully.
                // GET and restore keep stopped/failed states and never submit generation.
                if (!weak->m_finishing_candidate.empty()) {
                    boost::system::error_code ignored;
                    boost::filesystem::remove(weak->m_finishing_candidate, ignored);
                    weak->m_finishing_candidate.clear();
                }
                weak->m_finishing_options.selected_faces.clear();
                weak->m_finishing_before = false;
                weak->m_finishing_undo_path.clear();
                weak->m_finishing_redo_path.clear();
                weak->m_selected_image_path.clear();
                weak->reset(false);
                ++weak->m_style_recommendation_sequence;
                weak->m_style_recommendation_loading = false;
                weak->m_style_recommendation_available = false;
                weak->m_style_recommendation = {};
                weak->m_history_display_image = wxImage();
                weak->m_history_display_source.clear();
                weak->set_finishing_workbench(false);
                weak->restore_job(std::move(status), weak->m_sequence);
                if (weak->m_preview_book) weak->m_preview_book->SetSelection(0);
                weak->refresh_controls();
            });
        },
        [weak, sequence, history_sequence](std::string error) {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, history_sequence, error = std::move(error)] {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    history_sequence != weak->m_design_history_sequence) return;
                weak->m_design_history_loading = false;
                weak->m_busy = false;
                if (ModelGenerationPresentation::is_transient_sidecar_poll_error(error)) {
                    // A restarted sidecar has a new nonce. Discovery performs a
                    // fresh authenticated challenge; never replay a paid POST.
                    weak->set_service_availability(false, error);
                    weak->m_status->SetLabel(_L("服务连接已失效，当前内容已保留。\n正在重新检测，就绪后请重新打开设计。"));
                    if (weak->m_service_retry_handler) weak->m_service_retry_handler();
                    return;
                }
                weak->refresh_controls();
                weak->m_status->SetLabel(_L("历史设计加载失败，当前模型与输入已保留。"));
            });
        });
}

void ModelGenerationPanel::refresh_library()
{
    if (m_shutdown) return;
    if (m_library_sizer == nullptr || m_library_scroller == nullptr || !m_library_scroller->IsShownOnScreen()) {
        m_library_refresh_pending = true;
        refresh_workbench_history();
        return;
    }
    const int scroll_y = m_library_scroller->GetViewStart().y;
    m_library_thumbnail_edge = int(FromDIP(96) * GetContentScaleFactor());
    m_library_layout_width = m_library_scroller->GetClientSize().x;
    m_library_thumbnails.clear();
    m_library_sizer->Clear(true);
    m_library_page = std::min(m_library_page, m_library_entries.empty() ? size_t(0) : (m_library_entries.size() - 1) / m_library_page_size);
    m_library_empty->SetLabel(_L("还没有保存的设计图或模型。"));
    m_library_empty->Show(m_library_entries.empty());
    const size_t begin = m_library_page * m_library_page_size;
    const size_t end = std::min(begin + m_library_page_size, m_library_entries.size());
    for (size_t index = begin; index < end; ++index) {
        auto* card = create_library_card(m_library_entries[index]);
        refresh_ai_appearance(card);
        m_library_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }
    m_library_page_label->SetLabel(wxString::Format(_L("第 %llu / %llu 页 · 共 %llu 条"),
        static_cast<unsigned long long>(m_library_page + 1),
        static_cast<unsigned long long>(std::max(size_t(1), (m_library_entries.size() + m_library_page_size - 1) / m_library_page_size)),
        static_cast<unsigned long long>(m_library_entries.size())));
    m_library_previous->Enable(m_library_page > 0);
    m_library_next->Enable(end < m_library_entries.size());
    m_library_scroller->Layout();
    m_library_scroller->FitInside();
    m_library_scroller->Scroll(0, scroll_y);
    m_library_scroller->GetParent()->Layout();
    m_library_scroller->Refresh();
    refresh_workbench_history();
    request_library_thumbnails();
}

wxWindow* ModelGenerationPanel::create_library_card(const GeneratedModelEntry& entry)
{
        auto* card = new wxPanel(m_library_scroller, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        wxWindow* format = new LibraryThumbnail(card);
        m_library_thumbnails.push_back(format);
        row->Add(format, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(12));
        auto* text = new wxBoxSizer(wxVERTICAL);
        const int text_width = std::max(FromDIP(140), m_library_layout_width - FromDIP(264));
        auto* title = new wxStaticText(card, wxID_ANY, entry.title);
        wxFont title_font = title->GetFont();
        title_font.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(title_font);
        title->Wrap(text_width);
        text->Add(title, 0, wxBOTTOM, FromDIP(3));
        auto* details = new wxStaticText(card, wxID_ANY, entry.details);
        details->SetForegroundColour(wxColour(91, 104, 107));
        details->Wrap(text_width);
        text->Add(details, 0);
        if (!entry.provider_task_id.empty()) {
            // Use a normal button: the collapsible pane's custom header can
            // lose its caption when repainted inside the Windows model list.
            auto* diagnostics = new wxPanel(card);
            auto* diagnostics_sizer = new wxBoxSizer(wxVERTICAL);
            const bool expanded = m_library_expanded_details.count(entry.job_id) > 0;
            auto* toggle_details = new wxButton(diagnostics, wxID_ANY, expanded ? _L("收起任务详情") : _L("展开任务详情"));
            diagnostics_sizer->Add(toggle_details, 0, wxALIGN_LEFT);
            auto* task_parent = new wxPanel(diagnostics);
            auto* task_row = new wxBoxSizer(wxVERTICAL);
            auto* task_id = new wxStaticText(
                task_parent, wxID_ANY, _L("任务编号：") + wxString::FromUTF8(entry.provider_task_id));
            task_id->Wrap(text_width);
            task_id->SetForegroundColour(wxColour(31, 122, 116));
            task_id->SetToolTip(wxString::FromUTF8(entry.provider_task_id));
            auto* copy_task_id = new wxButton(
                task_parent, wxID_ANY, _L("复制"), wxDefaultPosition, wxSize(FromDIP(58), FromDIP(26)));
            copy_task_id->SetToolTip(_L("复制完整的 3D 生成任务编号"));
            copy_task_id->Bind(wxEVT_BUTTON, [this, provider_task_id = entry.provider_task_id](wxCommandEvent&) {
                bool copied = false;
                if (wxTheClipboard->Open()) {
                    copied = wxTheClipboard->SetData(
                        new wxTextDataObject(wxString::FromUTF8(provider_task_id)));
                    wxTheClipboard->Close();
                }
                m_status->SetLabel(copied ? _L("3D Task ID 已复制到剪贴板。")
                                          : _L("无法访问剪贴板，请稍后重试。"));
            });
            task_row->Add(task_id, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
            task_row->Add(copy_task_id, 0, wxALIGN_LEFT);
            task_parent->SetSizer(task_row);
            diagnostics_sizer->Add(task_parent, 0, wxEXPAND | wxTOP, FromDIP(8));
            task_parent->Show(expanded);
            diagnostics->SetSizer(diagnostics_sizer);
            text->Add(diagnostics, 0, wxEXPAND | wxTOP, FromDIP(4));
            toggle_details->Bind(wxEVT_BUTTON, [this, card, diagnostics, task_parent, toggle_details, job_id = entry.job_id](wxCommandEvent&) {
                const bool expanded = !task_parent->IsShown();
                if (expanded) m_library_expanded_details.insert(job_id);
                else m_library_expanded_details.erase(job_id);
                task_parent->Show(expanded);
                toggle_details->SetLabel(expanded ? _L("收起任务详情") : _L("展开任务详情"));
                diagnostics->InvalidateBestSize(); diagnostics->Layout();
                card->InvalidateBestSize();
                card->Layout(); m_library_scroller->Layout(); m_library_scroller->FitInside();
            });
        }
        row->Add(text, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(8));
        auto* actions = new wxBoxSizer(wxVERTICAL);
        auto* load = new wxButton(card, wxID_ANY, entry.design_only ? _L("打开设计") : _L("加载"),
                                  wxDefaultPosition, wxSize(FromDIP(104), -1));
        load->Bind(wxEVT_BUTTON,
            [this, model_path = entry.model_path, palette = entry.palette,
              palette_roles = entry.palette_roles, use_printable_colors = entry.use_printable_colors,
              reference_image_path = entry.reference_image_path, ai_image_path = entry.ai_image_path,
              color_intent_path = entry.color_intent_path, color_intent_schema = entry.color_intent_schema,
              color_intent_sha256 = entry.color_intent_sha256,
              job_id = entry.job_id, title_text = entry.title](wxCommandEvent&) {
                load_library_entry(model_path, reference_image_path, ai_image_path, palette, palette_roles,
                                   use_printable_colors, color_intent_path, color_intent_schema,
                                   color_intent_sha256, job_id, title_text);
            });
        actions->Add(load, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        auto* reuse_geometry = new wxButton(
            card, wxID_ANY, _L("复用造型"), wxDefaultPosition, wxSize(FromDIP(104), -1));
        reuse_geometry->SetToolTip(
            _L("保留这个历史模型的网格与脸部造型，使用当前确认图片重新生成颜色"));
        reuse_geometry->Enable(
            m_service_available && !m_busy && !m_job_id.empty() && m_job_preview_expected &&
            (m_ready || m_awaiting_confirmation) && entry.job_id != m_job_id);
        reuse_geometry->Bind(wxEVT_BUTTON, [this, job_id = entry.job_id, title_text = entry.title](wxCommandEvent&) {
            on_retexture_from_library(job_id, title_text);
        });
        actions->Add(reuse_geometry, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        reuse_geometry->Show(!entry.design_only);
        auto* remove = new wxButton(card, wxID_ANY, _L("删除本地"), wxDefaultPosition, wxSize(FromDIP(104), -1));
        remove->Bind(wxEVT_BUTTON, [this, entry](wxCommandEvent&) {
            delete_library_entry(entry);
        });
        actions->Add(remove, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        if (entry.imported_at > 0) {
            const wxString feedback_label = entry.print_feedback == "success"
                ? _L("打印成功 ✓")
                : entry.print_feedback == "issue" ? _L("打印有问题") : _L("记录打印结果");
            auto* feedback = new wxButton(
                card, wxID_ANY, feedback_label, wxDefaultPosition, wxSize(FromDIP(104), -1));
            feedback->SetToolTip(_L("由测试人员记录实际打印结果；不会从打印机自动推断"));
            feedback->Bind(wxEVT_BUTTON, [this, job_id = entry.job_id](wxCommandEvent&) {
                MessageDialog dialog(
                    this,
                    _L("请根据已经完成的真实打印记录结果。\n\n“打印成功”表示成品达到本次测试预期；“有问题”表示需要后续复盘。"),
                    _L("记录实际打印结果"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
                dialog.SetButtonLabel(wxID_YES, _L("打印成功"));
                dialog.SetButtonLabel(wxID_NO, _L("有问题"));
                const int result = dialog.ShowModal();
                if (result == wxID_YES)
                    record_library_print_feedback(job_id, "success");
                else if (result == wxID_NO)
                    record_library_print_feedback(job_id, "issue");
            });
            actions->Add(feedback, 0, wxEXPAND);
        }
        row->Add(actions, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(8));
        card->SetSizer(row);
        const auto bind_load = [this, model_path = entry.model_path, palette = entry.palette,
                                 palette_roles = entry.palette_roles,
                                 use_printable_colors = entry.use_printable_colors,
                                 reference_image_path = entry.reference_image_path,
                                 ai_image_path = entry.ai_image_path,
                                 color_intent_path = entry.color_intent_path,
                                 color_intent_schema = entry.color_intent_schema,
                                 color_intent_sha256 = entry.color_intent_sha256,
                                 job_id = entry.job_id,
                                title_text = entry.title](wxWindow* window) {
            window->SetCursor(wxCursor(wxCURSOR_HAND));
            window->SetToolTip(model_path.empty() ? _L("双击打开设计图") : _L("也可双击加载到 3D 模型预览"));
            window->Bind(wxEVT_LEFT_DCLICK, [this, model_path, reference_image_path, ai_image_path,
                                             palette, palette_roles, use_printable_colors, job_id,
                                             color_intent_path, color_intent_schema, color_intent_sha256,
                                             title_text](wxMouseEvent&) {
                load_library_entry(model_path, reference_image_path, ai_image_path, palette, palette_roles,
                                   use_printable_colors, color_intent_path, color_intent_schema,
                                   color_intent_sha256, job_id, title_text);
            });
        };
        bind_load(card);
        bind_load(format);
        format->SetToolTip(entry.design_only ? _L("双击恢复设计图及其输入，不会自动生成 3D。")
                                            : _L("关联设计图缩略图；双击加载实际 3D 模型。"));
        bind_load(title);
        bind_load(details);
        return card;
}

wxWindow* ModelGenerationPanel::build_workbench_history(wxWindow* parent)
{
    auto* panel = new WorkbenchPanel(parent, true);
    panel->SetMinSize(wxSize(FromDIP(280), FromDIP(200)));
    panel->SetBackgroundColour(wxColour(32, 32, 35));
    m_workbench_history_panel = panel;

    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(panel, wxID_ANY, _L("选择模型"));
    wxFont title_font = title->GetFont();
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(title_font);
    title->SetForegroundColour(wxColour(238, 240, 244));
    root->Add(title, 0, wxLEFT | wxRIGHT, FromDIP(12));
    root->PrependSpacer(FromDIP(24));
    root->AddSpacer(FromDIP(26));
    auto* search = new TextInput(panel, wxEmptyString, wxEmptyString, "workbench_search");
    auto search_icon = ScalableBitmap(search, "workbench_search", 12).bmp().ConvertToImage();
    search_icon.Replace(0, 0, 0, 235, 235, 235);
    search->SetIcon(wxBitmap(search_icon));
    search->SetName("ai_content_color");
    search->SetCornerRadius(FromDIP(13));
    search->SetBackgroundColor(StateColor(wxColour(49, 49, 54)));
    search->SetBorderColor(StateColor(
        std::pair<wxColour, int>(wxColour(255, 194, 39), StateColor::Focused),
        std::pair<wxColour, int>(wxColour(49, 49, 54), StateColor::Normal)));
    search->SetTextColor(StateColor(wxColour(235, 235, 235)));
    search->SetMinSize(FromDIP(wxSize(180, 26)));
    search->SetMaxSize(FromDIP(wxSize(-1, 26)));
    m_workbench_history_search = search->GetTextCtrl();
    m_workbench_history_search->SetBackgroundColour(wxColour(49, 49, 54));
    m_workbench_history_search->SetFont(panel->GetFont());
    m_workbench_history_search->SetHint(_L("搜索模型"));
    m_workbench_history_search->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        m_workbench_history_page = 0; refresh_workbench_history();
    });
    auto* search_row = new wxBoxSizer(wxHORIZONTAL);
    search_row->Add(search, 1, wxALIGN_CENTER_VERTICAL);
    auto* upload = workbench_button(panel, _L("上传"));
    m_workbench_history_upload = upload;
    upload->SetCornerRadius(FromDIP(9));
    upload->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(wxColour(48, 48, 52), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(94, 94, 96), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(77, 77, 79), StateColor::Normal)));
    upload->set_scaled_icon("workbench_upload", 15);
    upload->SetPaddingSize(FromDIP(wxSize(8, 4)));
    upload->SetMinSize(FromDIP(wxSize(63, 27)));
    upload->SetToolTip(_L("加载本地 GLB / OBJ 模型"));
    upload->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!post_generation_ui_state().can_switch_version) return;
        wxFileDialog dialog(this, _L("选择 3D 模型"), wxEmptyString, wxEmptyString,
            _L("3D 模型 (*.glb;*.obj)|*.glb;*.obj|GLB 模型 (*.glb)|*.glb|OBJ 模型 (*.obj)|*.obj"),
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        const boost::filesystem::path path(dialog.GetPath().ToStdWstring());
        load_library_entry(path, {}, {}, {}, {}, false, {}, {}, {}, {}, wxString(path.filename().wstring()));
    });
    search_row->Add(upload, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    root->Add(search_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    root->AddSpacer(FromDIP(18));
    m_workbench_history_status = new wxStaticText(panel, wxID_ANY, wxEmptyString);
    m_workbench_history_status->SetForegroundColour(wxColour(158, 164, 174));
    m_workbench_history_status->Wrap(FromDIP(204));
    auto* history_tools = new wxBoxSizer(wxHORIZONTAL);
    const std::array<wxString, 4> categories {_L("全部模型"), _L("原始模型"), _L("单色模型"), _L("多色模型")};
    const std::array<const char*, 4> category_assets {"workbench_history_all", "workbench_history_original",
        "workbench_history_monochrome", "workbench_history_multicolor"};
    for (size_t index = 0; index < categories.size(); ++index) {
        auto* button = workbench_button(panel, wxEmptyString);
        m_workbench_history_filters[index] = button;
        button->SetName(categories[index]);
        button->SetToolTip(categories[index]);
        button->set_scaled_icon(category_assets[index], 18);
        button->SetPaddingSize(FromDIP(wxSize(4, 4)));
        button->SetMinSize(FromDIP(wxSize(34, 34)));
        button->SetMaxSize(FromDIP(wxSize(34, 34)));
        button->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            m_workbench_history_filter = int(index);
            m_workbench_history_page = 0;
            refresh_workbench_history();
        });
        history_tools->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(7));
    }
    history_tools->AddStretchSpacer();
    auto* open_history = workbench_button(panel, wxEmptyString);
    open_history->SetName(_L("打开完整历史资产"));
    open_history->set_scaled_icon("workbench_history_manage", 18);
    open_history->SetMinSize(FromDIP(wxSize(32, 32)));
    open_history->SetToolTip(_L("打开完整历史资产"));
    open_history->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        set_finishing_workbench(false);
        if (m_preview_book) m_preview_book->SetSelection(1);
    });
    history_tools->Add(open_history, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(history_tools, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    root->Add(m_workbench_history_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_workbench_history_scroller = new WorkbenchScrolledWindow(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxVSCROLL | wxBORDER_NONE);
    m_workbench_history_scroller->SetBackgroundColour(wxColour(32, 32, 35));
    m_workbench_history_scroller->SetScrollRate(0, FromDIP(8));
    m_workbench_history_sizer = new wxBoxSizer(wxVERTICAL);
    m_workbench_history_scroller->SetSizer(m_workbench_history_sizer);
    root->AddSpacer(FromDIP(18));
    root->Add(m_workbench_history_scroller, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    auto* pages = new wxBoxSizer(wxHORIZONTAL);
    const auto page_button = [panel, this](const wxString& name, const char* asset, bool previous) {
        auto* button = workbench_button(panel, wxEmptyString);
        button->SetName(name);
        button->SetToolTip(name);
        button->set_scaled_icon(asset, 12, previous, true);
        button->SetPaddingSize(FromDIP(wxSize(4, 4)));
        button->SetMinSize(FromDIP(wxSize(28, 28)));
        button->SetBackgroundColor(StateColor(
            std::pair<wxColour, int>(wxColour(48, 48, 52), StateColor::Hovered),
            std::pair<wxColour, int>(wxColour(32, 32, 35), StateColor::Normal)));
        return button;
    };
    m_workbench_history_first = page_button(_L("第一页"), "workbench_page_edge", true);
    m_workbench_history_previous = page_button(_L("上一页"), "workbench_page_step", true);
    auto* counter = new wxPanel(panel, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(62, 28)), wxBORDER_NONE);
    counter->SetBackgroundColour(panel->GetBackgroundColour());
    counter->SetBackgroundStyle(wxBG_STYLE_PAINT);
    counter->Bind(wxEVT_PAINT, [counter](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(counter);
        dc.SetBackground(wxBrush(counter->GetBackgroundColour()));
        dc.Clear();
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(wxColour(153, 153, 156), counter->FromDIP(1)));
        const auto size = counter->GetClientSize();
        const int inset = counter->FromDIP(1);
        dc.DrawRoundedRectangle(inset, inset, size.x - 2 * inset, size.y - 2 * inset, counter->FromDIP(8));
    });
    counter->SetMinSize(FromDIP(wxSize(62, 28)));
    auto* counter_sizer = new wxBoxSizer(wxVERTICAL);
    counter_sizer->AddStretchSpacer();
    m_workbench_history_page_label = new wxStaticText(counter, wxID_ANY, "1 / 1", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_workbench_history_page_label->SetForegroundColour(wxColour(220, 220, 222));
    counter_sizer->Add(m_workbench_history_page_label, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(4));
    counter_sizer->AddStretchSpacer();
    counter->SetSizer(counter_sizer);
    m_workbench_history_next = page_button(_L("下一页"), "workbench_page_step", false);
    m_workbench_history_last = page_button(_L("最后一页"), "workbench_page_edge", false);
    pages->Add(m_workbench_history_first);
    pages->Add(m_workbench_history_previous);
    pages->AddStretchSpacer();
    pages->Add(counter, 0, wxALIGN_CENTER_VERTICAL);
    pages->AddStretchSpacer();
    pages->Add(m_workbench_history_next);
    pages->Add(m_workbench_history_last);
    root->Add(pages, 0, wxEXPAND | wxALL, FromDIP(8));
    m_workbench_history_previous->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_workbench_history_page) --m_workbench_history_page; refresh_workbench_history();
    });
    m_workbench_history_next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        ++m_workbench_history_page; refresh_workbench_history();
    });
    m_workbench_history_first->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_workbench_history_page = 0; refresh_workbench_history();
    });
    m_workbench_history_last->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_workbench_history_page = std::numeric_limits<size_t>::max();
        refresh_workbench_history();
    });

    panel->SetSizer(root);
    panel->Hide();
    return panel;
}

void ModelGenerationPanel::refresh_workbench_history()
{
    if (!m_workbench_history_sizer || !m_workbench_history_scroller) return;
    if (m_library_scan_pending) return;
    // Present the rebuilt cards only after their colors and geometry are ready.
    wxWindowUpdateLocker history_updates(m_workbench_history_panel);
    cancel_library_loading();
    m_workbench_thumbnails.clear();
    m_workbench_thumbnail_entries.clear();
    m_workbench_history_sizer->Clear(true);
    std::vector<WorkbenchHistoryRecord> records;
    for (const auto& entry : m_library_entries)
        records.push_back({entry.title, entry.model_path, entry.generated_at, entry.design_only,
                           is_nonempty_model(entry.model_path), entry.accepted_finishing, entry.color_count});
    const auto visible = workbench_history_indices(records, m_workbench_history_search->GetValue(),
        static_cast<WorkbenchHistoryFilter>(m_workbench_history_filter));
    for (size_t index = 0; index < m_workbench_history_filters.size(); ++index) {
        auto* button = m_workbench_history_filters[index];
        button->SetBackgroundColor(StateColor(
            std::pair<wxColour, int>(wxColour(64, 64, 68), StateColor::Hovered),
            std::pair<wxColour, int>(int(index) == m_workbench_history_filter
                ? wxColour(72, 72, 76) : wxColour(22, 22, 25), StateColor::Normal)));
        button->Refresh(false);
    }
    constexpr size_t page_size = 10;
    const size_t model_count = visible.size();
    const size_t pages = std::max(size_t(1), (model_count + page_size - 1) / page_size);
    m_workbench_history_page = std::min(m_workbench_history_page, pages - 1);
    const size_t begin = m_workbench_history_page * page_size;
    const size_t end = std::min(begin + page_size, model_count);
    auto* grid = new wxGridSizer(0, 2, FromDIP(9), FromDIP(9));
    const PostGenerationUiState ui_state = post_generation_ui_state();
    const auto card_label = [](wxWindow* parent, const wxString& text) {
        auto* label = new wxStaticText(parent, wxID_ANY, text);
        label->SetBackgroundColour(parent->GetBackgroundColour());
        label->SetBackgroundStyle(wxBG_STYLE_PAINT);
        // Native disabled static text adds a light embossed shadow on Windows.
        label->Bind(wxEVT_PAINT, [label](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(label);
            dc.SetBackground(wxBrush(label->GetBackgroundColour()));
            dc.Clear();
            dc.SetFont(label->GetFont());
            dc.SetTextForeground(label->GetForegroundColour());
            const auto size = label->GetClientSize();
            const auto caption = wxControl::Ellipsize(label->GetLabel(), dc, wxELLIPSIZE_END,
                std::max(0, size.x));
            dc.DrawText(caption, 0, std::max(0, (size.y - dc.GetTextExtent(caption).y) / 2));
        });
        return label;
    };
    for (size_t item = begin; item < end; ++item) {
        const size_t index = visible[item];
        const auto& entry = m_library_entries[index];
        const auto version = workbench_version_status(entry.model_path, m_displayed_model_path,
            entry.accepted_finishing ? entry.model_path : m_finishing_accepted_path,
            m_finishing_candidate.empty() ? boost::filesystem::path() : m_finishing_source);
        const bool current = version != WorkbenchVersionStatus::Saved;
        auto* card = new WorkbenchPanel(m_workbench_history_scroller);
        card->set_selected(current);
        card->SetMinSize(FromDIP(wxSize(123, 116)));
        card->SetBackgroundColour(wxColour(22, 22, 25));
        auto* thumbnail = new LibraryThumbnail(card, true);
        thumbnail->SetMinSize(FromDIP(wxSize(100, 90)));
        thumbnail->SetBackgroundColour(card->GetBackgroundColour());
        thumbnail->SetForegroundColour(wxColour(158, 164, 174));
        m_workbench_thumbnails.push_back(thumbnail);
        m_workbench_thumbnail_entries.push_back(index);
        const wxString face_count = entry.triangle_count > 0
            ? wxString::Format(_L("%llu 面"), static_cast<unsigned long long>(entry.triangle_count))
            : _L("面数待加载");
        thumbnail->set_details(entry.title, face_count);
        auto* status = card_label(thumbnail, wxEmptyString);
        status->SetFont(status->GetFont().Smaller());
        if (current || entry.accepted_finishing) {
            const wxString badge = version == WorkbenchVersionStatus::Candidate ? _L("候选编辑中") :
                version == WorkbenchVersionStatus::Accepted ? _L("已接受 · 当前") :
                current ? _L("当前模型") : _L("已接受");
            status->SetLabel(badge);
            status->SetForegroundColour(current ? wxColour(255, 194, 39) : wxColour(158, 164, 174));
        }
        auto* footer = new wxPanel(thumbnail, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        footer->SetBackgroundColour(card->GetBackgroundColour());
        const wxDateTime generated(entry.generated_at);
        auto* date = card_label(footer,
            generated.IsValid() ? generated.Format("%y/%m/%d") : _L("未知时间"));
        date->SetForegroundColour(wxColour(158, 164, 174));
        date->SetFont(date->GetFont().Smaller());
        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        actions->Add(date, 1, wxALIGN_CENTER_VERTICAL);
        footer->SetSizer(actions);
        const wxWeakRef<ModelGenerationPanel> weak_panel(this);
        const auto load =
            [weak_panel, model_path = entry.model_path, palette = entry.palette,
             palette_roles = entry.palette_roles, use_printable_colors = entry.use_printable_colors,
             reference_image_path = entry.reference_image_path, ai_image_path = entry.ai_image_path,
             color_intent_path = entry.color_intent_path, color_intent_schema = entry.color_intent_schema,
             color_intent_sha256 = entry.color_intent_sha256,
             job_id = entry.job_id, title_text = entry.title]() {
                if (!weak_panel || !weak_panel->post_generation_ui_state().can_switch_version) return;
                // Loading rebuilds these cards. Let the originating input event finish first.
                wxGetApp().CallAfter([=] {
                    if (!weak_panel || weak_panel->m_shutdown ||
                        !weak_panel->post_generation_ui_state().can_switch_version) return;
                    weak_panel->load_library_entry(model_path, reference_image_path, ai_image_path,
                        palette, palette_roles, use_printable_colors, color_intent_path,
                        color_intent_schema, color_intent_sha256, job_id, title_text);
                });
            };
        for (wxWindow* target : {static_cast<wxWindow*>(card), static_cast<wxWindow*>(thumbnail),
                                  static_cast<wxWindow*>(footer),
                                  static_cast<wxWindow*>(date), static_cast<wxWindow*>(status)}) {
            if (!target) continue;
            target->Bind(wxEVT_LEFT_UP, [load](wxMouseEvent&) { load(); });
            target->SetToolTip(entry.title + "\n" + entry.details + "\n" +
                (generated.IsValid() ? generated.Format("%Y/%m/%d %H:%M") : _L("未知时间")));
        }
        const auto action_button = [this, footer, actions](const char* icon, const wxString& tooltip) {
            auto* button = workbench_button(footer, wxEmptyString);
            button->set_scaled_icon(icon, 12);
            button->SetPaddingSize(FromDIP(wxSize(1, 2)));
            button->SetMinSize(FromDIP(wxSize(18, 24)));
            button->SetMaxSize(FromDIP(wxSize(18, 24)));
            button->SetToolTip(tooltip);
            actions->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxRESERVE_SPACE_EVEN_IF_HIDDEN);
            return button;
        };
        auto* open = action_button("workbench_history_load", _L("加载到 3D 工作台"));
        open->Bind(wxEVT_BUTTON, [load](wxCommandEvent&) { load(); });
        auto* download = action_button("workbench_history_download", _L("另存模型文件"));
        download->Bind(wxEVT_BUTTON, [this, source = entry.model_path](wxCommandEvent&) {
            if (!post_generation_ui_state().can_switch_version) return;
            boost::system::error_code ec;
            if (!AI::is_model_artifact(source) || !boost::filesystem::is_regular_file(source, ec) || ec) {
                show_error(this, _L("模型文件不可用，无法导出。"));
                return;
            }
            const bool glb = AI::model_artifact_format(source) == "glb";
            wxFileDialog dialog(this, _L("另存模型文件"), wxEmptyString, wxString(source.filename().wstring()),
                glb ? _L("GLB 模型 (*.glb)|*.glb") : _L("OBJ 模型 (*.obj)|*.obj"),
                wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (dialog.ShowModal() != wxID_OK) return;
            const boost::filesystem::path destination(dialog.GetPath().ToStdWstring());
            // Export must not overwrite the loaded source or any managed history asset.
            const auto root = generated_models_root();
            if (path_is_inside(root, destination.parent_path()) ||
                boost::filesystem::equivalent(root, destination.parent_path(), ec) ||
                path_is_inside(root, destination) ||
                (boost::filesystem::exists(destination, ec) && boost::filesystem::equivalent(source, destination, ec))) {
                show_error(this, _L("请选择历史资产目录之外的新位置。"));
                return;
            }
            if (AI::model_artifact_format(destination) != AI::model_artifact_format(source)) {
                show_error(this, _L("请保留模型原有的文件扩展名。"));
                return;
            }
            ec.clear();
            boost::filesystem::copy_file(source, destination, boost::filesystem::copy_options::overwrite_existing, ec);
            if (ec) show_error(this, _L("无法导出模型文件：") + wxString::FromUTF8(ec.message()));
        });
        auto* remove = action_button("workbench_history_delete", _L("删除本地资产"));
        remove->Bind(wxEVT_BUTTON, [weak_panel, entry](wxCommandEvent&) {
            wxGetApp().CallAfter([weak_panel, entry] {
                if (!weak_panel || weak_panel->m_shutdown ||
                    !weak_panel->post_generation_ui_state().can_switch_version) return;
                weak_panel->delete_library_entry(entry);
            });
        });
        const std::vector<wxWeakRef<wxWindow>> action_controls {open, download, remove};
        const wxWeakRef<wxWindow> weak_card(card);
        const wxWeakRef<LibraryThumbnail> weak_thumbnail(thumbnail);
        const wxWeakRef<wxWindow> weak_footer(footer);
        const auto update_actions = [weak_card, weak_thumbnail, weak_footer, action_controls, current]() {
            if (!weak_card) return;
            auto* focus = wxWindow::FindFocus();
            const bool inspecting = weak_card->GetScreenRect().Contains(wxGetMousePosition()) ||
                (focus && (focus == weak_card.get() || weak_card->IsDescendant(focus)));
            const bool visible = current || inspecting;
            if (weak_thumbnail) weak_thumbnail->set_inspecting(inspecting);
            if (weak_footer) { weak_footer->Show(visible); weak_footer->Raise(); }
            for (const auto& control : action_controls) if (control) {
                control->Show(visible);
                control->Refresh(false);
            }
            weak_card->Layout();
        };
        // Defer leave checks so moving between a card and its children keeps actions visible.
        const auto bind_hover = [&update_actions](auto&& self, wxWindow* window) -> void {
            for (const auto event : {wxEVT_ENTER_WINDOW, wxEVT_LEAVE_WINDOW})
                window->Bind(event, [update_actions](wxMouseEvent& event) {
                    wxGetApp().CallAfter(update_actions); event.Skip();
                });
            window->Bind(wxEVT_SET_FOCUS, [update_actions](wxFocusEvent& event) {
                wxGetApp().CallAfter(update_actions); event.Skip();
            });
            window->Bind(wxEVT_KILL_FOCUS, [update_actions](wxFocusEvent& event) {
                wxGetApp().CallAfter(update_actions); event.Skip();
            });
            for (wxWindow* child : window->GetChildren()) self(self, child);
        };
        bind_hover(bind_hover, card);
        for (const auto& control : action_controls) if (control) control->Show(current);
        // Fixed reference geometry keeps metadata overlays from resizing the card.
        const auto layout_card = [card, thumbnail, footer, status] {
            const auto size = card->GetClientSize();
            const int inset = card->FromDIP(6);
            // Center the reference image in the card; the status remains an overlay.
            const int thumbnail_top = card->FromDIP(1);
            const int thumbnail_height = std::max(0, size.y - 2 * thumbnail_top);
            thumbnail->SetSize(inset, thumbnail_top, std::max(0, size.x - 2 * inset), thumbnail_height);
            status->SetSize(0, card->FromDIP(1), std::max(0, size.x - 2 * inset), card->FromDIP(14));
            status->Raise();
            footer->SetSize(0, std::max(0, thumbnail_height - card->FromDIP(24)),
                std::max(0, size.x - 2 * inset), card->FromDIP(24));
            footer->Raise();
            footer->Layout();
        };
        card->Bind(wxEVT_SIZE, [layout_card](wxSizeEvent& event) { layout_card(); event.Skip(); });
        card->Enable(ui_state.can_switch_version);
        grid->Add(card, 0, wxEXPAND);
        wxGetApp().CallAfter(update_actions);
    }
    m_workbench_history_sizer->Add(grid, 0, wxEXPAND);
    if (model_count == 0)
        m_workbench_history_status->SetLabel(m_workbench_history_filter != 0 || !m_workbench_history_search->GetValue().empty()
            ? _L("没有符合筛选条件的模型") : _L("暂无可加载的历史 3D 模型"));
    else
        m_workbench_history_status->SetLabel(wxString::Format(_L("%llu 个模型"),
            static_cast<unsigned long long>(model_count)));
    m_workbench_history_status->Show(model_count == 0);
    m_workbench_history_page_label->SetToolTip(wxString::Format(_L("%llu 个模型"),
        static_cast<unsigned long long>(model_count)));
    m_workbench_history_page_label->SetLabel(wxString::Format("%llu / %llu",
        static_cast<unsigned long long>(m_workbench_history_page + 1), static_cast<unsigned long long>(pages)));
    m_workbench_history_previous->Enable(m_workbench_history_page > 0);
    m_workbench_history_first->Enable(m_workbench_history_page > 0);
    m_workbench_history_next->Enable(end < model_count);
    m_workbench_history_last->Enable(end < model_count);
    m_workbench_history_scroller->Layout();
    m_workbench_history_scroller->FitInside();
    m_workbench_history_panel->Layout();
    if (m_workbench_history_scroller->IsShownOnScreen() && !m_library_scroller->IsShownOnScreen())
        request_library_thumbnails();
}

} // namespace Slic3r::GUI
