#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/AI/AIWindowAppearance.hpp"
#include "ModelLibraryThumbnail.hpp"
#include "ModelLibraryModelThumbnail.hpp"
#include "ModelLibraryFilter.hpp"
#include "ModelLibraryExport.hpp"
#include "ModelGenerationInputStyle.hpp"
#include <wx/menu.h>
#include <wx/dirdlg.h>
#include "ModelGenerationPresentation.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/datetime.h>
#include <wx/dcbuffer.h>
#include <wx/image.h>
#include <wx/simplebook.h>
#include <wx/log.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/weakref.h>

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
        entry.search_text = entry.title;
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

        const nlohmann::json metadata = read_json(library_metadata_path(job_id));
        if (metadata.is_object()) {
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
                entry.search_text = entry.title;
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

int library_columns(wxWindow* window, int width, bool drawer)
{
    const int gap = window->FromDIP(drawer ? 12 : 24);
    const int card = window->FromDIP(drawer ? 164 : 274);
    return std::clamp((width + gap) / (card + gap), 1, 6);
}

// A local card owns its theme and selection border. Content pixels are never
// recolored by either the legacy theme walk or the input-page theme walk.
class LibraryCard final : public wxPanel, public AIThemeOwner {
public:
    LibraryCard(wxWindow* parent, std::string asset_id, bool drawer)
        : wxPanel(parent), id(std::move(asset_id)), m_drawer(drawer)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(ModelGenerationInputStyle::panel)); dc.Clear();
            const auto surface = m_drawer ? ModelGenerationInputStyle::field : ModelGenerationInputStyle::panel;
            dc.SetBrush(wxBrush(surface));
            const bool focused = m_focus_target && m_focus_target->HasFocus();
            dc.SetPen(wxPen(selected || focused ? ModelGenerationInputStyle::yellow : surface,
                            FromDIP(focused ? 2 : 1)));
            auto rect = GetClientRect(); rect.Deflate(FromDIP(1));
            dc.DrawRoundedRectangle(rect, FromDIP(12));
        });
    }
    void bind_selection(std::function<void()> select, wxWindow* focus_target) {
        m_focus_target = focus_target;
        focus_target->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
        focus_target->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
        Bind(wxEVT_CHAR_HOOK, [this, select = std::move(select)](wxKeyEvent& event) {
            if (m_focus_target && m_focus_target->HasFocus() && !event.AltDown() && !event.CmdDown() &&
                (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_SPACE)) {
                select();
                return;
            }
            event.Skip();
        });
    }
    void set_selected(bool value) { if (selected != value) { selected = value; Refresh(false); } }
    void apply_ai_theme(bool fonts) override {
        ModelGenerationInputStyle::apply(this, fonts);
        // Full-page metadata shares the page surface; the compact drawer keeps
        // its inset cards. Real thumbnail colors are never themed.
        for (auto* child : GetChildren())
            child->SetBackgroundColour(m_drawer ? ModelGenerationInputStyle::field : ModelGenerationInputStyle::panel);
        Refresh(false);
    }
    const std::string id;
private:
    bool m_drawer {false};
    bool selected {false};
    wxWindow* m_focus_target {nullptr};
};

// Keep thumbnail painting in one fixed, clipped surface, independently of the
// native title/button controls and the adjacent notebook's OpenGL preview.
class LibraryThumbnail final : public wxPanel
{
public:
    bool AcceptsFocus() const override { return true; }
    bool AcceptsFocusFromKeyboard() const override { return true; }
    void SetFocus() override { SetFocusIgnoringChildren(); }
    explicit LibraryThumbnail(wxWindow* parent) : wxPanel(parent)
    {
        SetMinSize(FromDIP(wxSize(144, 155)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetBackgroundColour()));
            dc.Clear();
            const auto size = GetClientSize();
            if (m_bitmap.IsOk()) {
                dc.DrawBitmap(m_bitmap, (size.x - m_bitmap.GetLogicalWidth()) / 2,
                              (size.y - m_bitmap.GetLogicalHeight()) / 2, true);
            } else {
                dc.SetFont(GetFont());
                dc.SetTextForeground(GetForegroundColour());
                dc.DrawLabel(m_pending ? _L("加载缩略图…") : _L("无缩略图"), wxRect(size), wxALIGN_CENTER);
            }
        });
    }

    void set_image(const wxImage& image)
    {
        m_pending = false;
        m_bitmap = image.IsOk() ? wxBitmap(image) : wxBitmap();
        if (m_bitmap.IsOk()) m_bitmap.SetScaleFactor(GetContentScaleFactor());
        else SetToolTip(_L("缩略图不可用（文件缺失、损坏或过大），仍可通过加载按钮打开资产。"));
        Refresh(false);
    }

    void finish_pending()
    {
        if (m_pending) set_image(wxImage());
    }

private:
    wxBitmap m_bitmap;
    bool m_pending {true};
};

} // namespace

struct ModelGenerationPanel::LibraryLoadState
{
    struct Request {
        uint64_t revision {0};
        bool scan {false};
        bool images_only {false};
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
    // Only plain pixels cross the worker/UI boundary. wxImage reference data
    // is not atomic in every supported wx build.
    struct Thumbnail {
        uint64_t revision;
        size_t index;
        int width {0}, height {0};
        std::vector<unsigned char> rgb, alpha;
    };
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
                    try {
                        if (!request->images_only && !entry.design_only)
                            image = cache.load(library_model_thumbnail_image(request->root, entry.model_path),
                                               {}, request->edge, request->cancelled);
                        if (!image.IsOk())
                            image = cache.load(entry.ai_image_path, entry.reference_image_path, request->edge, request->cancelled);
                    }
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
    ++m_library_revision;
    if (!m_library_scroller || !m_library_scroller->IsShownOnScreen()) m_library_open_pending = false;
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
    m_library_filter_timer.Stop();
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
    if (!m_page_initialized || !m_library_scroller || !m_library_scroller->IsShownOnScreen()) {
        cancel_library_loading();
        return;
    }
    cancel_library_loading();
    start_library_worker();
    auto request = std::make_shared<LibraryLoadState::Request>();
    request->revision = m_library_revision;
    request->scan = true;
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
    cover_library_scroll_track_during_layout();
    m_library_load_state->wake.notify_one();
    m_library_timer.Start(50);
}

void ModelGenerationPanel::on_library_timer(wxTimerEvent&)
{
    if (m_shutdown || !m_library_load_state) return;
    if (!m_library_scroller->IsShownOnScreen()) {
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
        if (snapshot->failed) {
            m_library_empty->SetLabel(_L("历史资产读取失败，已保留当前列表。请点击刷新重试。"));
            m_library_empty->Show();
            m_library_previous->Enable(m_library_page > 0);
            m_library_next->Enable((m_library_page + 1) * m_library_page_size < m_library_filtered_indices.size());
            for (auto* thumbnail : m_library_thumbnails)
                static_cast<LibraryThumbnail*>(thumbnail)->finish_pending();
            m_library_refresh_pending = false;
        } else {
            const size_t anchor_index = m_library_page * m_library_page_size;
            const std::string anchor = anchor_index < m_library_filtered_indices.size()
                ? m_library_entries[m_library_filtered_indices[anchor_index]].job_id : std::string();
            m_library_entries = std::move(snapshot->entries);
            m_library_filtered_indices = filter_model_library(m_library_entries, m_library_category,
                m_library_search->GetValue(), m_library_drawer_open);
            const auto position = std::find_if(m_library_filtered_indices.begin(), m_library_filtered_indices.end(),
                [&](size_t index) { return m_library_entries[index].job_id == anchor; });
            m_library_page = position == m_library_filtered_indices.end() ? 0 :
                size_t(position - m_library_filtered_indices.begin()) / m_library_page_size;
            refresh_library();
        }
    }
    // Applying decoded pixels creates wxImage/wxBitmap objects on the UI
    // thread. Keep each timer tick bounded so opening the history page stays
    // interactive while the worker continues decoding the rest of the page.
    constexpr size_t max_thumbnails_per_tick = 2;
    auto deferred = publish_library_thumbnail_batch(std::move(thumbnails), m_library_revision,
        m_library_thumbnails.size(), max_thumbnails_per_tick, [this](const auto& thumbnail) {
        wxImage image;
        if (!thumbnail.rgb.empty()) {
            if (image.Create(thumbnail.width, thumbnail.height)) {
                std::memcpy(image.GetData(), thumbnail.rgb.data(), thumbnail.rgb.size());
                if (!thumbnail.alpha.empty()) {
                    image.InitAlpha();
                    std::memcpy(image.GetAlpha(), thumbnail.alpha.data(), thumbnail.alpha.size());
                }
            }
        }
        static_cast<LibraryThumbnail*>(m_library_thumbnails[thumbnail.index])->set_image(image);
    });
    if (!deferred.empty()) {
        std::lock_guard<std::mutex> lock(m_library_load_state->mutex);
        m_library_load_state->thumbnails.insert(m_library_load_state->thumbnails.begin(),
            std::make_move_iterator(deferred.begin()), std::make_move_iterator(deferred.end()));
    }
    // DPI changes invalidate only the visible page; the worker cache keys
    // include pixel size, and themes repaint via the normal AI appearance path.
    if (!m_library_refresh_pending &&
        (m_library_thumbnail_edge != int(FromDIP(m_library_drawer_open ? 144 : 206) * GetContentScaleFactor()) ||
         library_columns(this, m_library_layout_width, m_library_drawer_open) !=
         library_columns(this, m_library_scroller->GetClientSize().x, m_library_drawer_open))) refresh_library();

    if (m_library_open_pending) {
        if (m_library_empty->GetLabel() != m_status->GetLabel()) {
            wxString feedback = m_status->GetLabel();
            feedback.Replace("\n", " ");
            m_library_empty->SetLabel(feedback);
            m_library_empty->SetToolTip(feedback);
            m_library_empty->Wrap(std::max(FromDIP(160), m_library_scroller->GetClientSize().x));
            m_library_empty->SetMinSize(wxSize(1, m_library_empty->GetBestSize().y));
            m_library_scroller->GetParent()->Layout();
        }
        if (!m_busy && !m_design_history_loading) m_library_open_pending = false;
    }
    // The timer is only a bridge for worker results. Stop polling once the
    // worker has no queued or active work; it will be restarted by the next
    // refresh or page change.
    bool loading = false;
    {
        std::lock_guard<std::mutex> lock(m_library_load_state->mutex);
        loading = m_library_load_state->active || m_library_load_state->pending ||
                  m_library_load_state->snapshot.has_value() || !m_library_load_state->thumbnails.empty();
    }
    if (!loading && !m_library_refresh_pending && !m_library_open_pending) m_library_timer.Stop();
}

void ModelGenerationPanel::request_library_thumbnails()
{
    cancel_library_loading();
    start_library_worker();
    m_library_refresh_pending = false;
    auto request = std::make_shared<LibraryLoadState::Request>();
    request->revision = m_library_revision;
    request->edge = m_library_thumbnail_edge;
    request->root = generated_models_root();
    request->images_only = m_library_drawer_open || m_library_category == ModelLibraryCategory::Design;
    const auto begin = std::min(m_library_page * m_library_page_size, m_library_filtered_indices.size());
    const auto end = std::min(begin + m_library_page_size, m_library_filtered_indices.size());
    for (size_t i = begin; i < end; ++i)
        request->entries.push_back(m_library_entries[m_library_filtered_indices[i]]);
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
    if (preserve_unsaved_finishing()) return;
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
                if (weak->m_library_drawer_open) weak->set_library_drawer(false);
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
                weak->m_finishing_options.reset();
                weak->m_finishing_before = false;
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

void ModelGenerationPanel::apply_library_filter()
{
    m_library_filter_timer.Stop();
    m_library_page = 0;
    for (size_t i = 0; i < m_library_filter_buttons.size(); ++i) {
        auto* button = m_library_filter_buttons[i];
        button->SetName(i == static_cast<size_t>(m_library_category) ? "input_quiet" : "input_field");
        ModelGenerationInputStyle::apply(button, false);
    }
    if (m_shutdown || m_library_refresh_pending || !m_library_scroller->IsShownOnScreen()) return;
    m_library_scroller->Scroll(0, 0);
    refresh_library();
}

void ModelGenerationPanel::refresh_library(bool prepare_hidden)
{
    if (m_shutdown) return;
    if (m_library_sizer == nullptr || m_library_scroller == nullptr || (!prepare_hidden && !m_library_scroller->IsShownOnScreen())) {
        m_library_refresh_pending = true;
        return;
    }
    m_library_filtered_indices = filter_model_library(m_library_entries, m_library_category,
        m_library_search->GetValue(), m_library_drawer_open);
    cover_library_scroll_track_during_layout();
    const size_t count = m_library_filtered_indices.size();
    const int scroll_y = m_library_scroller->GetViewStart().y;
    m_library_thumbnail_edge = int(FromDIP(m_library_drawer_open ? 144 : 206) * GetContentScaleFactor());
    m_library_layout_width = m_library_scroller->GetClientSize().x;
    m_library_thumbnails.clear();
    m_library_sizer->Clear(true);
    m_library_page = std::min(m_library_page, count == 0 ? size_t(0) : (count - 1) / m_library_page_size);
    if (prepare_hidden)
        m_library_empty->SetLabel(_L("正在读取历史资产…"));
    else if (count == 0)
        m_library_empty->SetLabel(m_library_entries.empty() ? _L("还没有保存的设计图或模型。") :
            _L("没有匹配的资产，请修改搜索词或重置筛选。"));
    m_library_empty->Show(prepare_hidden || count == 0);
    const size_t begin = m_library_page * m_library_page_size;
    const size_t end = std::min(begin + m_library_page_size, count);
    const int columns = library_columns(this, m_library_layout_width, m_library_drawer_open);
    const int gap = FromDIP(m_library_drawer_open ? 12 : 24);
    auto* grid = new wxGridSizer(columns, gap, gap);
    for (size_t index = begin; index < end; ++index) {
        auto* card = create_library_card(m_library_entries[m_library_filtered_indices[index]]);
        refresh_ai_appearance(card);
        grid->Add(card, 1, wxEXPAND);
    }
    m_library_sizer->Add(grid, 0, wxEXPAND | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_library_page_label->SetLabel(wxString::Format(_L("第 %llu / %llu 页 · 共 %llu 条"),
        static_cast<unsigned long long>(m_library_page + 1),
        static_cast<unsigned long long>(std::max(size_t(1), (count + m_library_page_size - 1) / m_library_page_size)),
        static_cast<unsigned long long>(count)));
    m_library_previous->Enable(m_library_page > 0);
    m_library_next->Enable(end < count);
    if (m_library_drawer_open) {
        m_library_drawer_pages->SetLabel(wxString::Format("%llu / %llu",
            static_cast<unsigned long long>(m_library_page + 1),
            static_cast<unsigned long long>(std::max(size_t(1), (count + m_library_page_size - 1) / m_library_page_size))));
        for (size_t i = 0; i < 4; ++i) m_library_drawer_pager[i]->Enable(i < 2 ? m_library_page > 0 : end < count);
        update_drawer_selection();
    }
    m_library_scroller->Layout();
    m_library_scroller->FitInside();
    m_library_scroller->Scroll(0, scroll_y);
    m_library_scroller->GetParent()->Layout();
    m_library_layout_width = m_library_scroller->GetClientSize().x;
    m_library_scroller->Refresh();
    // Reparenting changes the card role, page size and width before the page
    // is exposed. Prepare cached widgets now; only the visible scan queues work.
    if (!prepare_hidden) request_library_thumbnails();
    sync_library_scroll_track();
}

void ModelGenerationPanel::show_library_action_feedback(const wxString& previous_status)
{
    if (!m_library_scroller->IsShownOnScreen() || previous_status == m_status->GetLabel()) return;
    m_library_empty->SetLabel(m_status->GetLabel());
    m_library_empty->SetToolTip(m_status->GetLabel());
    m_library_empty->Wrap(std::max(FromDIP(160), m_library_scroller->GetClientSize().x));
    m_library_empty->SetMinSize(wxSize(1, m_library_empty->GetBestSize().y));
    m_library_empty->Show();
    m_library_scroller->GetParent()->Layout();
    m_library_open_pending = m_busy || m_design_history_loading;
    if (m_library_open_pending) m_library_timer.Start(100);
}

void ModelGenerationPanel::export_library_entry(const GeneratedModelEntry& entry)
{
    if (m_busy || m_finishing_running || m_shutdown) return;
    const bool images_only = m_library_drawer_open || m_library_category == ModelLibraryCategory::Design;
    wxDirDialog picker(this, _L("选择导出位置（新建副本文件夹，同名自动编号）"),
        wxEmptyString, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (picker.ShowModal() != wxID_OK) return;
    ModelLibraryExportRequest request;
    if (!images_only) request.model = entry.model_path;
    request.design = images_only ? entry.ai_image_path :
        entry.preview_path.empty() ? entry.ai_image_path : entry.preview_path;
    request.reference = entry.reference_image_path;
    request.id = entry.job_id;
    request.title = entry.search_text.empty() ? entry.title.ToUTF8().data() : entry.search_text.ToUTF8().data();
    request.task_id = entry.provider_task_id;
    request.conversion_task_id = entry.provider_conversion_task_id;
    const boost::filesystem::path parent(picker.GetPath().ToStdWstring());
    if (m_library_import_worker.joinable()) m_library_import_worker.join();
    const wxString before = m_status->GetLabel();
    m_busy = true;
    m_status->SetLabel(_L("正在导出已保存资产及贴图，原件保持不变……"));
    refresh_controls();
    show_library_action_feedback(before);
    wxWeakRef<ModelGenerationPanel> weak(this);
    try {
        m_library_import_worker = std::thread([weak, request, parent] {
            boost::filesystem::path destination;
            std::string error;
            const bool success = export_model_library_copy(request, parent, destination, error);
            wxGetApp().CallAfter([weak, success, destination, error] {
                if (!weak || weak->m_shutdown) return;
                if (weak->m_library_import_worker.joinable()) weak->m_library_import_worker.join();
                weak->m_busy = false;
                weak->refresh_controls();
                const wxString message = success
                    ? _L("副本已导出，原件保持不变：\n") + wxString(destination.wstring())
                    : _L("导出未完成，原件保持不变：\n") + wxString::FromUTF8(error);
                const wxString before = weak->m_status->GetLabel();
                weak->m_status->SetLabel(message);
                weak->show_library_action_feedback(before);
                MessageDialog dialog(weak.get(), message, _L("导出副本"),
                    wxOK | (success ? wxICON_INFORMATION : wxICON_WARNING));
                dialog.ShowModal();
            });
        });
    } catch (const std::exception& e) {
        m_busy = false;
        refresh_controls();
        const wxString before = m_status->GetLabel();
        m_status->SetLabel(_L("无法开始导出：") + wxString::FromUTF8(e.what()));
        show_library_action_feedback(before);
    }
}

void ModelGenerationPanel::update_drawer_selection()
{
    if (!m_library_drawer_open) return;
    const auto match = std::find_if(m_library_filtered_indices.begin(), m_library_filtered_indices.end(),
        [this](size_t index) { return m_library_entries[index].job_id == m_library_selected_asset_id; });
    const bool selected = match != m_library_filtered_indices.end();
    m_library_drawer_use->Enable(selected && input_editable());
    const bool design = selected && m_library_entries[*match].design_only;
    m_library_drawer_use->SetLabel(!selected || design ? _L("打开设计") : _L("用作参考图"));
    ModelGenerationInputStyle::apply(m_library_drawer_use, false);
    m_library_drawer_selection->SetLabel(selected ? (design ? _L("历史设计 · ") : _L("模型关联图 · ")) + m_library_entries[*match].title : _L("选择图片，再打开设计或用作参考图"));
    m_library_drawer_selection->SetToolTip(selected ? m_library_entries[*match].title : wxString());
}

wxWindow* ModelGenerationPanel::create_library_card(const GeneratedModelEntry& entry)
{
    const bool associated_image = m_library_category == ModelLibraryCategory::Design && !entry.design_only;
    auto* card = new LibraryCard(m_library_scroller, entry.job_id, m_library_drawer_open);
    card->SetName(_L("选择：") + entry.title);
    if (m_library_drawer_open) {
        card->SetMinSize(FromDIP(wxSize(164, 155)));
        card->set_selected(m_library_selected_asset_id == entry.job_id);
        auto* thumbnail = new LibraryThumbnail(card);
        thumbnail->SetMinSize(FromDIP(wxSize(1, 147)));
        m_library_thumbnails.push_back(thumbnail);
        thumbnail->SetToolTip(entry.title + "\n" + (entry.design_only ? _L("历史设计：单击选择，打开后恢复设计记录。") : _L("模型关联图：单击选择，用作参考图后可重新设计；不会加载模型。")) +
            "\n" + wxString::FromUTF8(entry.job_id));
        auto select = [this, card, thumbnail] {
            thumbnail->SetFocusIgnoringChildren();
            m_library_selected_asset_id = card->id;
            for (auto* image : m_library_thumbnails) {
                auto* item = static_cast<LibraryCard*>(image->GetParent());
                item->set_selected(item->id == m_library_selected_asset_id);
            }
            update_drawer_selection();
        };
        card->bind_selection(select, thumbnail);
        thumbnail->Bind(wxEVT_SET_FOCUS, [this, card](wxFocusEvent& event) {
            if (m_library_scroller && m_library_scroller->IsShownOnScreen()) {
                const wxPoint top = m_library_scroller->ScreenToClient(card->ClientToScreen(wxPoint(0, 0)));
                const int margin = FromDIP(4);
                const int bottom = top.y + card->GetClientSize().y;
                const int viewport_bottom = m_library_scroller->GetClientSize().y;
                const int delta = top.y < margin ? top.y - margin :
                    bottom > viewport_bottom - margin ? bottom - viewport_bottom + margin : 0;
                if (delta != 0) {
                    int unit_x = 0, unit_y = 0;
                    m_library_scroller->GetScrollPixelsPerUnit(&unit_x, &unit_y);
                    if (unit_y > 0) {
                        int view_x = 0, view_y = 0;
                        m_library_scroller->GetViewStart(&view_x, &view_y);
                        const int steps = (std::abs(delta) + unit_y - 1) / unit_y;
                        m_library_scroller->Scroll(view_x, std::max(0, view_y + (delta > 0 ? steps : -steps)));
                        if (m_library_scroll_track) m_library_scroll_track->Refresh();
                    }
                }
            }
            event.Skip();
        });
        thumbnail->Bind(wxEVT_LEFT_DOWN, [select](wxMouseEvent&) { select(); });
        card->Bind(wxEVT_LEFT_DOWN, [select](wxMouseEvent&) { select(); });
        thumbnail->SetCursor(wxCursor(wxCURSOR_HAND));
        auto* column = new wxBoxSizer(wxVERTICAL);
        column->Add(thumbnail, 1, wxEXPAND | wxALL, FromDIP(4));
        card->SetSizer(column);
        // A model-associated image is shared with its model record, not independently deletable here.
        if (!entry.design_only) return card;
        auto* remove = new Button(thumbnail, wxEmptyString, "figma-ux/drawer-delete", 0, 16);
        remove->SetName("input_field");
        remove->SetMinSize(FromDIP(wxSize(32, 32)));
        remove->SetPaddingSize(FromDIP(wxSize(8, 8)));
        remove->SetSize(FromDIP(wxSize(32, 32)));
        remove->SetToolTip(_L("删除这份设计；确认前不会删除。"));
        remove->Bind(wxEVT_BUTTON, [this, entry](wxCommandEvent&) {
            const wxString before = m_status->GetLabel();
            delete_library_entry(entry);
            show_library_action_feedback(before);
        });
        auto* overlay = new wxBoxSizer(wxVERTICAL);
        overlay->AddStretchSpacer();
        auto* bottom = new wxBoxSizer(wxHORIZONTAL);
        bottom->AddStretchSpacer();
        bottom->Add(remove, 0);
        overlay->Add(bottom, 0, wxEXPAND);
        thumbnail->SetSizer(overlay);
        return card;
    }
    // Let the sizer include the actual font and action heights. A fixed card
    // height compresses the final action row after increasing the thumbnail.
    card->SetMinSize(FromDIP(wxSize(250, -1)));
    card->set_selected(m_library_selected_asset_id == entry.job_id);
    auto* column = new wxBoxSizer(wxVERTICAL);
    auto* thumbnail = new LibraryThumbnail(card);
    thumbnail->SetMinSize(FromDIP(wxSize(1, 206)));
    m_library_thumbnails.push_back(thumbnail);
    column->Add(thumbnail, 0, wxEXPAND | wxALL, FromDIP(8));
    auto* title = new wxStaticText(card, wxID_ANY, entry.title, wxDefaultPosition,
        FromDIP(wxSize(1, 22)), wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    title->SetFont(wxGetApp().bold_font());
    title->SetToolTip(entry.title + "\n" + entry.details);
    column->Add(title, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    const wxDateTime generated(entry.generated_at);
    const wxString date = generated.IsValid() ? generated.FormatISODate() : _L("未知时间");
    const auto asset_kind = model_library_asset_kind(entry.design_only, entry.job_id);
    const wxString kind = associated_image ? _L("模型关联图") :
        asset_kind == ModelLibraryAssetKind::Design ? _L("设计图") :
        asset_kind == ModelLibraryAssetKind::LocalImport ? _L("本地导入模型") :
        asset_kind == ModelLibraryAssetKind::Finishing ? _L("美颜版本") : _L("3D 模型");
    const bool current = entry.design_only ? entry.job_id == m_job_id :
        m_model_preview_ready && entry.job_id == m_displayed_model_job_id;
    auto* info = new wxStaticText(card, wxID_ANY,
        current ? kind + _L(" · 当前打开") : kind + _L(" · ") + date);
    info->SetName("input_secondary");
    info->SetToolTip(entry.details);
    column->Add(info, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    // Keep the service task discoverable without expanding secondary actions.
    // Local versions have their own identity, never a fabricated provider task.
    const bool has_task = !entry.provider_task_id.empty();
    const wxString identity = wxString::FromUTF8(has_task ? entry.provider_task_id : entry.job_id);
    const wxString identity_label = has_task ? wxString("Task ID: ") : _L("本地 ID: ");
    auto* identity_row = new wxBoxSizer(wxHORIZONTAL);
    auto* identity_text = new wxStaticText(card, wxID_ANY, identity_label + identity,
        wxDefaultPosition, FromDIP(wxSize(1, 22)), wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    identity_text->SetName("input_secondary");
    identity_text->SetToolTip(identity_label + identity);
    identity_row->Add(identity_text, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    auto* copy = new Button(card, _L("复制"));
    copy->SetName("input_field");
    copy->SetMinSize(FromDIP(wxSize(40, 26)));
    copy->SetPaddingSize(FromDIP(wxSize(4, 2)));
    copy->SetToolTip((has_task ? _L("复制完整 Task ID：") : _L("复制完整本地版本 ID：")) + identity);
    copy->Bind(wxEVT_BUTTON, [this, identity, has_task](wxCommandEvent&) {
        bool copied = false;
        if (wxTheClipboard->Open()) {
            copied = wxTheClipboard->SetData(new wxTextDataObject(identity));
            wxTheClipboard->Close();
        }
        m_library_empty->SetLabel(copied
            ? (has_task ? _L("Task ID 已复制。") : _L("本地 ID 已复制（不是服务任务编号）。"))
            : _L("无法访问剪贴板，请稍后重试。"));
        m_library_empty->Show();
        m_library_scroller->GetParent()->Layout();
    });
    identity_row->Add(copy, 0);
    column->Add(identity_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    auto select = [this, card, thumbnail] {
        thumbnail->SetFocusIgnoringChildren();
        m_library_selected_asset_id = card->id;
        for (auto* image : m_library_thumbnails) {
            auto* item = static_cast<LibraryCard*>(image->GetParent());
            item->set_selected(item->id == m_library_selected_asset_id);
        }
    };
    card->bind_selection(select, thumbnail);
    for (wxWindow* item : {static_cast<wxWindow*>(card), static_cast<wxWindow*>(thumbnail),
                           static_cast<wxWindow*>(title), static_cast<wxWindow*>(info)}) {
        item->SetCursor(wxCursor(wxCURSOR_HAND));
        item->Bind(wxEVT_LEFT_DOWN, [select](wxMouseEvent& event) { select(); event.Skip(); });
    }
    thumbnail->SetToolTip(associated_image ? _L("模型关联设计图；单击只选择，用作参考图后才替换创作输入，原模型保留。") :
        entry.design_only ? _L("设计图缩略图；单击选中，点击打开设计继续。") :
        _L("关联设计图缩略图，不代表美颜后的外观；点击加载模型查看实际版本。"));
    // Keep the two asset actions visible at the thumbnail's lower edge, as in
    // the library card design. They operate on the captured entry, not on a
    // later selection or a possibly refreshed page index.
    auto* thumbnail_actions = new wxBoxSizer(wxVERTICAL);
    thumbnail_actions->AddStretchSpacer();
    auto* thumbnail_buttons = new wxBoxSizer(wxHORIZONTAL);
    thumbnail_buttons->AddStretchSpacer();
    auto* export_copy = new Button(thumbnail, wxEmptyString, "figma-ux/library-download", 0, 17);
    export_copy->SetName("input_field");
    export_copy->SetMinSize(FromDIP(wxSize(32, 32)));
    export_copy->SetPaddingSize(FromDIP(wxSize(7, 7)));
    export_copy->SetToolTip(associated_image ? _L("导出设计图和参考图副本；不导出或修改关联模型。") :
        _L("导出该资产的副本；原件保持不变。"));
    export_copy->Bind(wxEVT_BUTTON, [this, entry](wxCommandEvent&) { export_library_entry(entry); });
    thumbnail_buttons->Add(export_copy, 0, wxRIGHT, FromDIP(4));
    auto* remove = new Button(thumbnail, wxEmptyString, "figma-ux/library-delete", 0, 17);
    remove->SetName("input_field");
    remove->SetMinSize(FromDIP(wxSize(32, 32)));
    remove->SetPaddingSize(FromDIP(wxSize(7, 7)));
    remove->Enable(!associated_image);
    remove->SetToolTip(associated_image ? _L("这张图片与模型共用资产记录，不能在图片页单独删除。") :
        _L("删除这个本地版本；确认前不会删除。"));
    remove->Bind(wxEVT_BUTTON, [this, entry](wxCommandEvent&) {
        const wxString before = m_status->GetLabel();
        delete_library_entry(entry);
        show_library_action_feedback(before);
    });
    thumbnail_buttons->Add(remove, 0);
    thumbnail_actions->Add(thumbnail_buttons, 0, wxEXPAND);
    thumbnail->SetSizer(thumbnail_actions);
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    auto* load = new Button(card, associated_image ? _L("用作参考图") : entry.design_only ? _L("打开设计") : _L("加载模型"));
    load->SetName("input_quiet");
    load->SetMinSize(FromDIP(wxSize(-1, 36)));
    load->SetPaddingSize(FromDIP(wxSize(10, 6)));
    load->Bind(wxEVT_BUTTON, [this, entry, select, associated_image](wxCommandEvent&) {
        if (m_busy || m_design_history_loading || m_finishing_running) return;
        if (preserve_unsaved_finishing()) return;
        select();
        const wxString before = m_status->GetLabel();
        if (associated_image) replace_input_image(entry.ai_image_path);
        else load_library_entry(entry.model_path, entry.reference_image_path, entry.ai_image_path,
                entry.palette, entry.palette_roles, entry.use_printable_colors, entry.color_intent_path,
                entry.color_intent_schema, entry.color_intent_sha256, entry.job_id, entry.title);
        show_library_action_feedback(before);
    });
    actions->Add(load, 1, wxRIGHT, FromDIP(6));
    auto* more = new Button(card, _L("更多"));
    more->SetName("input_field");
    more->SetMinSize(FromDIP(wxSize(48, 36)));
    more->SetPaddingSize(FromDIP(wxSize(6, 6)));
    more->Bind(wxEVT_BUTTON, [this, card, entry, select](wxCommandEvent&) {
        select();
        wxMenu menu;
        enum { Details = wxID_HIGHEST + 1, CopyTask, Reuse, Feedback };
        menu.Append(Details, _L("查看资产详情"));
        menu.Bind(wxEVT_MENU, [this, entry](wxCommandEvent&) {
            wxString task = _L("\n\n本地 ID：") + wxString::FromUTF8(entry.job_id);
            if (!entry.provider_task_id.empty())
                task += _L("\nTask ID：") + wxString::FromUTF8(entry.provider_task_id);
            if (!entry.provider_conversion_task_id.empty())
                task += _L("\n转换任务 ID：") + wxString::FromUTF8(entry.provider_conversion_task_id);
            MessageDialog dialog(this, entry.title + "\n\n" + entry.details + task,
                _L("资产详情"), wxOK | wxICON_INFORMATION);
            dialog.ShowModal();
        }, Details);
        if (!entry.provider_task_id.empty()) {
            menu.Append(CopyTask, _L("复制3D任务编号"));
            menu.Bind(wxEVT_MENU, [this, entry](wxCommandEvent&) {
                bool copied = false;
                if (wxTheClipboard->Open()) {
                    copied = wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(entry.provider_task_id)));
                    wxTheClipboard->Close();
                }
                m_library_empty->SetLabel(copied ? _L("任务编号已复制。") : _L("无法访问剪贴板，请稍后重试。"));
                m_library_empty->Show(); m_library_scroller->GetParent()->Layout();
            }, CopyTask);
        }
        if (!entry.design_only) {
            menu.Append(Reuse, _L("复用造型"));
            menu.Enable(Reuse, m_service_available && !m_busy && !m_job_id.empty() &&
                m_job_preview_expected && (m_ready || m_awaiting_confirmation) && entry.job_id != m_job_id);
            menu.Bind(wxEVT_MENU, [this, entry](wxCommandEvent&) {
                const wxString before = m_status->GetLabel();
                on_retexture_from_library(entry.job_id, entry.title);
                show_library_action_feedback(before);
            }, Reuse);
        }
        if (entry.imported_at > 0) {
            menu.Append(Feedback, _L("记录实际打印结果"));
            menu.Bind(wxEVT_MENU, [this, entry](wxCommandEvent&) {
                MessageDialog dialog(this,
                    _L("请根据已经完成的真实打印记录结果。\n不会从打印机自动推断。"),
                    _L("记录实际打印结果"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
                dialog.SetButtonLabel(wxID_YES, _L("打印成功"));
                dialog.SetButtonLabel(wxID_NO, _L("有问题"));
                const int result = dialog.ShowModal();
                if (result == wxID_YES) record_library_print_feedback(entry.job_id, "success");
                else if (result == wxID_NO) record_library_print_feedback(entry.job_id, "issue");
            }, Feedback);
        }
        card->PopupMenu(&menu);
    });
    actions->Add(more, 0, wxRIGHT, FromDIP(6));
    column->Add(actions, 0, wxEXPAND | wxALL, FromDIP(8));
    card->SetSizer(column);
    return card;
}

} // namespace Slic3r::GUI
