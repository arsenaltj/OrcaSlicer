#include "ModelGenerationPanel.hpp"

#include "3DScene.hpp"
#include "AI/Model/VertexColorRegionEditor.hpp"
#include "AISidecarClient.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "GLModel.hpp"
#include "GLShader.hpp"
#include "GuiColor.hpp"
#include "MsgDialog.hpp"
#include "OpenGLManager.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <nlohmann/json.hpp>

#include <glad/gl.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/collpane.h>
#include <wx/colordlg.h>
#include <wx/clrpicker.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/datetime.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/utils.h>
#include <wx/weakref.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <utility>

namespace Slic3r::GUI {
namespace {

constexpr int POLL_TIMER_ID = wxID_HIGHEST + 913;
constexpr size_t MAX_IMAGE_SIZE = 20 * 1024 * 1024;
constexpr double MIN_PREVIEW_ZOOM = 0.5;
constexpr double MAX_PREVIEW_ZOOM = 4.0;
constexpr int MAX_PREVIEW_BITMAP_DIMENSION = 4096;
constexpr const char* GENERATED_MODEL_PREFIX = "orcaslicer-ai-";
constexpr std::array<const char*, 4> PALETTE_ROLE_IDS {"primary", "structure", "light", "accent"};

struct LabColor {
    double l { 0.0 };
    double a { 0.0 };
    double b { 0.0 };
};

LabColor lab_color(const std::string& color)
{
    const wxColour rgb(from_u8(color));
    const auto linear = [](unsigned char channel) {
        const double value = channel / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const double red = linear(rgb.Red());
    const double green = linear(rgb.Green());
    const double blue = linear(rgb.Blue());
    const double x = (0.4124564 * red + 0.3575761 * green + 0.1804375 * blue) / 0.95047;
    const double y = 0.2126729 * red + 0.7151522 * green + 0.0721750 * blue;
    const double z = (0.0193339 * red + 0.1191920 * green + 0.9503041 * blue) / 1.08883;
    const auto transform = [](double value) {
        return value > 0.008856 ? std::cbrt(value) : 7.787 * value + 16.0 / 116.0;
    };
    const double fx = transform(x);
    const double fy = transform(y);
    const double fz = transform(z);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

AIModelGenerationClient::PaletteRoles automatic_palette_roles(const std::vector<std::string>& palette)
{
    AIModelGenerationClient::PaletteRoles result;
    if (palette.empty())
        return result;
    std::map<std::string, LabColor> labs;
    for (const std::string& color : palette)
        labs.emplace(color, lab_color(color));
    std::vector<std::string> available = palette;
    const auto take = [&available, &result](const char* role, const auto& better) {
        if (available.empty())
            return;
        const auto selected = std::max_element(available.begin(), available.end(), better);
        result.emplace(role, *selected);
        available.erase(selected);
    };
    if (palette.size() >= 2)
        take("structure", [&labs](const std::string& left, const std::string& right) { return labs[left].l > labs[right].l; });
    if (palette.size() >= 3)
        take("light", [&labs](const std::string& left, const std::string& right) { return labs[left].l < labs[right].l; });
    take("primary", [&labs](const std::string& left, const std::string& right) {
        return std::hypot(labs[left].a, labs[left].b) < std::hypot(labs[right].a, labs[right].b);
    });
    if (!available.empty())
        result.emplace("accent", available.front());
    return result;
}

bool same_palette_color(const std::string& left, const std::string& right)
{
    return left.size() == right.size() && std::equal(
        left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
            return std::toupper(a) == std::toupper(b);
        });
}

wxString palette_role_label(const std::string& role)
{
    if (role == "primary") return _L("主体");
    if (role == "structure") return _L("结构");
    if (role == "light") return _L("浅色");
    if (role == "accent") return _L("强调");
    return {};
}

double minimum_palette_distance(const std::vector<std::string>& palette)
{
    double minimum = std::numeric_limits<double>::infinity();
    for (size_t left = 0; left < palette.size(); ++left) {
        const LabColor a = lab_color(palette[left]);
        for (size_t right = left + 1; right < palette.size(); ++right) {
            const LabColor b = lab_color(palette[right]);
            minimum = std::min(minimum, std::sqrt(
                std::pow(a.l - b.l, 2.0) + std::pow(a.a - b.a, 2.0) + std::pow(a.b - b.b, 2.0)));
        }
    }
    return minimum;
}

int remap_progress(int value, int input_start, int input_end, int output_start, int output_end)
{
    value = std::clamp(value, input_start, input_end);
    return output_start + (value - input_start) * (output_end - output_start) / (input_end - input_start);
}

int display_progress(const AIModelGenerationClient::JobStatus& status)
{
    if (status.state == "recommending_palette")
        return remap_progress(status.progress, 5, 10, 3, 10);
    if (status.state == "awaiting_palette_confirmation")
        return 10;
    if (status.state == "preprocessing")
        return remap_progress(status.progress, 5, 15, 10, 25);
    if (status.state == "awaiting_confirmation")
        return 35;
    if (status.phase == "generating")
        return remap_progress(status.progress, 20, 70, 40, 78);
    if (status.phase == "converting")
        return remap_progress(status.progress, 75, 95, 80, 90);
    if (status.phase == "downloading_artifact")
        return 92;
    if (status.state == "ready")
        return 95;
    return 0;
}

std::string new_request_id()
{
    return boost::uuids::to_string(boost::uuids::random_generator()());
}

bool is_supported_image(const boost::filesystem::path& path)
{
    if (!boost::filesystem::is_regular_file(path))
        return false;
    const auto size = boost::filesystem::file_size(path);
    if (size == 0 || size > MAX_IMAGE_SIZE)
        return false;
    boost::filesystem::ifstream stream(path, std::ios::binary);
    unsigned char magic[8] {};
    stream.read(reinterpret_cast<char*>(magic), sizeof(magic));
    const auto count = stream.gcount();
    const bool png = count >= 8 && magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G' &&
                     magic[4] == 0x0d && magic[5] == 0x0a && magic[6] == 0x1a && magic[7] == 0x0a;
    const bool jpeg = count >= 3 && magic[0] == 0xff && magic[1] == 0xd8 && magic[2] == 0xff;
    return png || jpeg;
}

bool is_nonempty_obj(const boost::filesystem::path& path)
{
    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(path, ec) || boost::filesystem::file_size(path, ec) == 0)
        return false;
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension == ".obj";
}

boost::filesystem::path generated_models_root()
{
    const char* configured = std::getenv("ORCASLICER_AI_OUTPUT_DIR");
    return configured != nullptr && configured[0] != '\0'
        ? boost::filesystem::path(configured)
        : boost::filesystem::current_path() / "generated_models";
}

boost::filesystem::path temp_path(const std::string& job_id, const std::string& extension)
{
    const boost::filesystem::path root = generated_models_root();
    const boost::filesystem::path downloads = root / "downloads";
    boost::system::error_code ec;
    boost::filesystem::create_directories(downloads, ec);
    return downloads / (std::string(GENERATED_MODEL_PREFIX) + job_id + "." + extension);
}

boost::filesystem::path library_metadata_path(const std::string& job_id)
{
    return temp_path(job_id, "json");
}

std::string download_job_id(const boost::filesystem::path& path)
{
    const std::string stem = path.stem().string();
    if (stem.rfind(GENERATED_MODEL_PREFIX, 0) != 0)
        return {};
    return stem.substr(std::char_traits<char>::length(GENERATED_MODEL_PREFIX));
}

nlohmann::json read_json(const boost::filesystem::path& path)
{
    boost::filesystem::ifstream stream(path);
    if (!stream)
        return {};
    return nlohmann::json::parse(stream, nullptr, false);
}

wxString style_label(const std::string& style)
{
    if (style == "low_poly")
        return _L("低多边形");
    if (style == "cel_shaded")
        return _L("赛璐璐色块");
    if (style == "enamel_inlay")
        return _L("釉彩嵌色摆件");
    if (style == "sculpture")
        return _L("雕塑（适合单色）");
    if (style == "custom")
        return _L("自定义");
    return _L("Q 版设计师玩具");
}

wxStaticText* section_label(wxWindow* parent, const wxString& text)
{
    auto* label = new wxStaticText(parent, wxID_ANY, text);
    wxFont font = label->GetFont();
    font.SetWeight(wxFONTWEIGHT_BOLD);
    label->SetFont(font);
    label->SetForegroundColour(wxColour(40, 55, 58));
    return label;
}

wxString localized_job_status(const AIModelGenerationClient::JobStatus& status)
{
    if (status.state == "recommending_palette")
        return _L("AI 正在分析主体并推荐四种目标色...");
    if (status.state == "awaiting_palette_confirmation")
        return _L("AI 配色已准备好，请修改或确认后生成图片预览。");
    if (status.state == "preprocessing")
        return _L("AI 正在准备提示词和图片预览...");
    if (status.state == "awaiting_confirmation")
        return status.palette_quality_ok
            ? _L("预览已准备完成，请确认后继续生成 3D 模型。")
            : _L("预览未通过打印性检查，请按提示调整后重新生成。");
    if (status.state == "queued")
        return _L("3D 生成任务已排队...");
    if (status.state == "running" && status.phase == "generating")
        return _L("AI 正在生成 3D 模型...");
    if (status.state == "running" && status.phase == "converting")
        return _L("正在转换并优化彩色低模 OBJ...");
    if (status.state == "running" && status.phase == "downloading_artifact")
        return _L("正在整理模型文件...");
    if (status.state == "ready")
        return _L("3D 模型生成完成。");
    if (status.state == "stopping")
        return _L("正在停止生成任务...");
    if (status.state == "cancelled")
        return _L("生成任务已取消。");
    if (status.state == "failed")
        return status.message.empty() ? _L("生成任务失败。") : _L("生成任务失败：") + from_u8(status.message);
    return status.message.empty() ? _L("正在处理...") : from_u8(status.message);
}

wxString model_quality_code_label(const std::string& code)
{
    if (code == "tiny_detached_components") return _L("检测到微小脱离部件，请旋转模型确认是否需要保留。");
    if (code == "floating_disconnected_components") return _L("检测到未接触热床或主体的悬空分离部件，请检查是否可打印。");
    if (code == "thin_structural_components") return _L("检测到整体厚度较薄的连通部件，请检查是否需要加厚。");
    if (code == "tiny_printable_color_regions") return _L("检测到过小的耗材色块，打印时可能产生碎片化换色。");
    if (code == "weak_bed_contact") return _L("模型与热床接触面积较小，请检查底座稳定性。");
    if (code == "extreme_aspect_ratio") return _L("模型比例较极端，请检查缩放和摆放方向。");
    if (code == "high_downward_surface_ratio") return _L("向下表面较多，打印时可能需要更多支撑。");
    if (code == "localized_overhang_regions") return _L("检测到局部悬垂面，请旋转模型检查是否需要支撑。");
    if (code == "dense_micro_triangles") return _L("局部三角面非常密集，请检查细小结构。");
    if (code == "repairable_boundary_edges") return _L("存在少量开放边，将在导入时交给 Orca 修复。");
    if (code == "repairable_non_manifold_edges") return _L("存在少量非流形边，将在导入时交给 Orca 修复。");
    if (code == "repairable_inconsistent_winding_edges") return _L("存在少量面绕序异常，将在导入时交给 Orca 修复。");
    if (code == "boundary_edges") return _L("模型包含开放边，当前不能安全导入切片。");
    if (code == "non_manifold_edges") return _L("模型包含非流形边，当前不能安全导入切片。");
    if (code == "inconsistent_winding_edges") return _L("模型面绕序不一致，当前不能安全导入切片。");
    if (code == "degenerate_faces") return _L("模型包含退化三角面，当前不能安全导入切片。");
    if (code == "flat_or_empty_axis") return _L("模型至少一个方向没有有效尺寸。");
    if (code == "too_many_faces") return _L("模型面数超过当前允许上限。");
    if (code == "missing_geometry") return _L("模型没有可用几何数据。");
    return _L("检测到需要复核的模型结构问题：") + from_u8(code);
}

wxString visual_quality_code_label(const std::string& code)
{
    if (code == "visual_subject_incomplete") return _L("主体可能缺失或截断，请对照原图确认。");
    if (code == "visual_semantic_incoherence") return _L("局部形体或部件关系可能不自然。");
    if (code == "visual_base_relationship") return _L("主体与底座的连接关系需要确认。");
    if (code == "visual_detached_artifacts") return _L("多视角中疑似存在意外漂浮物。");
    if (code == "visual_silhouette_unclear") return _L("部分视角轮廓不够清晰。");
    if (code == "visual_color_regions_unclear") return _L("顶点色色块可能过碎或不易辨认。");
    if (code == "visual_review_unavailable") return _L("AI 视觉服务暂不可用，可稍后重试。");
    return _L("检测到需要人工确认的外观问题：") + from_u8(code);
}

} // namespace

class ModelPreview3D final : public wxPanel
{
public:
    explicit ModelPreview3D(wxWindow* parent)
        : wxPanel(parent)
    {
        SetBackgroundColour(wxColour(241, 244, 245));
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_canvas = OpenGLManager::create_wxglcanvas(*this);
        m_canvas->SetMinSize(wxSize(FromDIP(360), FromDIP(300)));
        sizer->Add(m_canvas, 1, wxEXPAND);
        SetSizer(sizer);

        m_context = wxGetApp().init_glcontext(*m_canvas);
        m_canvas->Bind(wxEVT_PAINT, &ModelPreview3D::on_paint, this);
        m_canvas->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            m_canvas->Refresh(false);
            event.Skip();
        });
        m_canvas->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            m_dragging = true;
            m_drag_moved = false;
            m_drag_start = event.GetPosition();
            m_last_mouse = event.GetPosition();
            if (!m_canvas->HasCapture())
                m_canvas->CaptureMouse();
            m_canvas->SetFocus();
        });
        m_canvas->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            if (m_selection_enabled && !m_drag_moved)
                select_at(event.GetPosition());
            finish_drag();
        });
        m_canvas->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
            if (!wxGetMouseState().LeftIsDown())
                finish_drag();
        });
        m_canvas->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (!m_dragging || !event.LeftIsDown())
                return;
            const wxPoint current = event.GetPosition();
            if (!m_drag_moved) {
                const wxPoint distance = current - m_drag_start;
                m_drag_moved = distance.x * distance.x + distance.y * distance.y > FromDIP(3) * FromDIP(3);
            }
            if (!m_drag_moved)
                return;
            m_yaw += (current.x - m_last_mouse.x) * 0.012;
            m_pitch = std::clamp(m_pitch + (current.y - m_last_mouse.y) * 0.012, -1.45, 1.45);
            m_last_mouse = current;
            m_canvas->Refresh(false);
        });
        m_canvas->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
            const int delta = event.GetWheelDelta();
            if (delta == 0)
                return;
            const double turns = double(event.GetWheelRotation()) / double(delta);
            m_zoom = std::clamp(m_zoom * std::pow(1.15, turns), 0.45, 2.5);
            m_canvas->Refresh(false);
        });
        m_canvas->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (!m_selection_enabled) {
                event.Skip();
                return;
            }
            if (event.ControlDown() && (event.GetKeyCode() == 'Z' || event.GetKeyCode() == 'z')) {
                undo_selection();
                return;
            }
            if (event.GetKeyCode() == WXK_ESCAPE) {
                clear_selection();
                return;
            }
            event.Skip();
        });
    }

    ~ModelPreview3D() override { clear(); }

    bool load_model(const boost::filesystem::path& path, const std::vector<std::string>& palette,
                    size_t& triangle_count, Vec3d& dimensions, size_t& color_count, std::string& error)
    {
        clear();
        if (m_context == nullptr || !m_context->IsOK()) {
            error = "OpenGL preview context is unavailable.";
            return false;
        }

        TriangleMesh mesh;
        ObjInfo obj_info;
        if (!load_obj(path.string().c_str(), &mesh, obj_info, error) || mesh.empty())
            return false;

        const indexed_triangle_set& its = mesh.its;
        const bool has_vertex_colors = obj_info.vertex_colors.size() == its.vertices.size();
        const bool has_face_colors = obj_info.face_colors.size() == its.indices.size();
        const RGBA fallback {ColorRGBA::ORCA().r(), ColorRGBA::ORCA().g(), ColorRGBA::ORCA().b(), 1.0f};
        auto face_color = [&](size_t face_index) {
            RGBA source = fallback;
            const stl_triangle_vertex_indices& indices = its.indices[face_index];
            if (has_vertex_colors) {
                for (size_t channel = 0; channel < 4; ++channel) {
                    source[channel] = (obj_info.vertex_colors[indices[0]][channel] +
                                       obj_info.vertex_colors[indices[1]][channel] +
                                       obj_info.vertex_colors[indices[2]][channel]) / 3.0f;
                }
            } else if (has_face_colors) {
                source = obj_info.face_colors[face_index];
            }
            return source;
        };

        std::vector<ColorRGBA> display_colors;
        decode_colors(palette, display_colors);
        if (display_colors.empty() && (has_vertex_colors || has_face_colors)) {
            struct ColorBucket {
                std::array<double, 3> sum {0.0, 0.0, 0.0};
                size_t count {0};
            };
            std::array<ColorBucket, 64> buckets;
            for (size_t face_index = 0; face_index < its.indices.size(); ++face_index) {
                const RGBA source = face_color(face_index);
                const auto channel = [&source](size_t index) {
                    return static_cast<size_t>(std::clamp(std::lround(source[index] * 3.0f), 0L, 3L));
                };
                ColorBucket& bucket = buckets[(channel(0) * 4 + channel(1)) * 4 + channel(2)];
                for (size_t index = 0; index < 3; ++index)
                    bucket.sum[index] += source[index];
                ++bucket.count;
            }
            for (const ColorBucket& bucket : buckets) {
                if (bucket.count == 0)
                    continue;
                display_colors.emplace_back(
                    static_cast<float>(bucket.sum[0] / bucket.count),
                    static_cast<float>(bucket.sum[1] / bucket.count),
                    static_cast<float>(bucket.sum[2] / bucket.count), 1.0f);
            }
        }
        if (display_colors.empty())
            display_colors.push_back(ColorRGBA::ORCA());

        std::vector<GLModel::Geometry> groups(display_colors.size());
        for (GLModel::Geometry& geometry : groups)
            geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};
        std::vector<bool> used(display_colors.size(), false);

        for (size_t face_index = 0; face_index < its.indices.size(); ++face_index) {
            const stl_triangle_vertex_indices& indices = its.indices[face_index];
            const Vec3f& a = its.vertices[indices[0]];
            const Vec3f& b = its.vertices[indices[1]];
            const Vec3f& c = its.vertices[indices[2]];
            Vec3f normal = (b - a).cross(c - a);
            if (normal.squaredNorm() > 1e-12f)
                normal.normalize();
            else
                normal = Vec3f::UnitZ();

            const RGBA source = face_color(face_index);

            size_t group_index = 0;
            float best_distance = std::numeric_limits<float>::max();
            for (size_t index = 0; index < display_colors.size(); ++index) {
                const float dr = source[0] - display_colors[index].r();
                const float dg = source[1] - display_colors[index].g();
                const float db = source[2] - display_colors[index].b();
                const float distance = dr * dr + dg * dg + db * db;
                if (distance < best_distance) {
                    best_distance = distance;
                    group_index = index;
                }
            }

            GLModel::Geometry& geometry = groups[group_index];
            const unsigned int base = static_cast<unsigned int>(geometry.vertices_count());
            geometry.add_vertex(a, normal);
            geometry.add_vertex(b, normal);
            geometry.add_vertex(c, normal);
            geometry.add_triangle(base, base + 1, base + 2);
            used[group_index] = true;
        }

        for (size_t index = 0; index < groups.size(); ++index) {
            if (groups[index].is_empty())
                continue;
            auto model = std::make_unique<GLModel>();
            model->init_from(std::move(groups[index]));
            model->set_color(display_colors[index]);
            m_models.emplace_back(std::move(model));
        }
        if (m_models.empty()) {
            error = "The OBJ contains no renderable triangles.";
            return false;
        }

        m_bounds = mesh.bounding_box();
        triangle_count = its.indices.size();
        dimensions = m_bounds.size().cast<double>();
        color_count = std::count(used.begin(), used.end(), true);
        if (has_vertex_colors) {
            if (!m_region_editor.initialize(std::move(mesh.its), std::move(obj_info.vertex_colors), error)) {
                m_models.clear();
                return false;
            }
        } else {
            // Preview and structural review do not require vertex colors. Keep the
            // color-dependent editor unavailable instead of rejecting a valid OBJ.
            m_region_editor.clear();
        }
        m_palette = palette;
        m_has_model = true;
        m_paint_diagnostics_logged = false;
        m_render_diagnostics_logged = false;
        reset_view();
        notify_selection_changed();
        return true;
    }

    void clear()
    {
        if (m_context != nullptr && m_canvas != nullptr)
            m_canvas->SetCurrent(*m_context);
        m_models.clear();
        m_selection_model.reset();
        m_region_editor.clear();
        m_selection_history.clear();
        m_palette.clear();
        m_has_model = false;
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
    }

    void reset_view()
    {
        m_yaw = -0.65;
        m_pitch = 0.35;
        m_zoom = 1.0;
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
    }

    void refresh()
    {
        if (m_canvas != nullptr) {
            m_canvas->Refresh(false);
            m_canvas->Update();
        }
    }

    void set_selection_enabled(bool enabled)
    {
        const bool selection_enabled = enabled && m_region_editor.ready();
        if (selection_enabled == m_selection_enabled)
            return;
        m_selection_enabled = selection_enabled;
        if (m_canvas != nullptr)
            m_canvas->SetCursor(wxCursor(m_selection_enabled ? wxCURSOR_CROSS : wxCURSOR_ARROW));
        if (!m_selection_enabled) {
            m_selection_history.clear();
            clear_selection(false);
        }
    }

    void set_selection_operation(AI::RegionSelectionOperation operation)
    {
        m_selection_operation = operation;
    }

    void set_selection_settings(const AI::RegionSelectionSettings& settings)
    {
        m_selection_settings = settings;
    }

    void set_selection_preview_color(const ColorRGBA& color)
    {
        m_selection_preview_color = color;
        if (m_selection_model != nullptr)
            m_selection_model->set_color(m_selection_preview_color);
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
    }

    void set_selection_changed_callback(std::function<void(size_t)> callback)
    {
        m_selection_changed = std::move(callback);
    }

    void clear_selection(bool record_history = true)
    {
        if (record_history && m_region_editor.selected_face_count() > 0)
            push_selection_history(m_region_editor.selected_faces());
        m_region_editor.clear_selection();
        rebuild_selection_model();
        notify_selection_changed();
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
    }

    size_t selected_face_count() const { return m_region_editor.selected_face_count(); }
    bool region_editing_ready() const { return m_region_editor.ready(); }
    bool can_undo_selection() const { return !m_selection_history.empty(); }

    bool undo_selection()
    {
        if (m_selection_history.empty())
            return false;
        std::vector<uint8_t> previous = std::move(m_selection_history.back());
        m_selection_history.pop_back();
        if (!m_region_editor.restore_selection(previous))
            return false;
        rebuild_selection_model();
        notify_selection_changed();
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
        return true;
    }

    bool apply_selection_color(const RGBA& color, const boost::filesystem::path& source,
                               const boost::filesystem::path& destination, std::string& error)
    {
        return m_region_editor.apply_color_to_obj_copy(color, source, destination, error);
    }

    bool select_palette_material(size_t palette_index)
    {
        std::vector<ColorRGBA> decoded;
        decode_colors(m_palette, decoded);
        if (decoded.empty() || palette_index >= decoded.size())
            return false;
        std::vector<RGBA> palette;
        palette.reserve(decoded.size());
        for (const ColorRGBA& color : decoded)
            palette.push_back({color.r(), color.g(), color.b(), color.a()});

        const std::vector<uint8_t> previous = m_region_editor.selected_faces();
        m_region_editor.select_palette_material(palette, palette_index);
        if (previous != m_region_editor.selected_faces())
            push_selection_history(previous);
        rebuild_selection_model();
        notify_selection_changed();
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
        return true;
    }

    size_t select_elevated_overhang_regions()
    {
        const std::vector<uint8_t> previous = m_region_editor.selected_faces();
        const size_t localized = m_region_editor.select_elevated_overhang_regions();
        if (localized == 0)
            return 0;
        if (previous != m_region_editor.selected_faces())
            push_selection_history(previous);
        rebuild_selection_model();
        notify_selection_changed();
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
        return localized;
    }

private:
    void push_selection_history(const std::vector<uint8_t>& selected_faces)
    {
        constexpr size_t max_history = 20;
        if (m_selection_history.size() >= max_history)
            m_selection_history.erase(m_selection_history.begin());
        m_selection_history.push_back(selected_faces);
    }

    void finish_drag()
    {
        m_dragging = false;
        if (m_canvas != nullptr && m_canvas->HasCapture())
            m_canvas->ReleaseMouse();
    }

    void notify_selection_changed()
    {
        if (m_selection_changed)
            m_selection_changed(m_region_editor.selected_face_count());
    }

    void select_at(const wxPoint& point)
    {
        if (!m_region_editor.ready() || m_canvas == nullptr)
            return;
        int width = 0;
        int height = 0;
        m_canvas->GetClientSize(&width, &height);
        if (width <= 0 || height <= 0)
            return;
        const Vec3d size = m_bounds.size().cast<double>();
        const double radius = std::max(0.001, 0.5 * size.norm());
        const double half_height = radius * 1.12 / m_zoom;
        const double half_width = half_height * double(width) / double(height);
        const double screen_x = 2.0 * double(point.x) / double(width) - 1.0;
        const double screen_y = 1.0 - 2.0 * double(point.y) / double(height);
        const Vec3d center = m_bounds.center().cast<double>();
        const Transform3d view_model =
            Geometry::translation_transform(Vec3d(0.0, 0.0, -3.0 * radius)) *
            Geometry::rotation_transform(Vec3d(m_pitch, m_yaw, 0.0)) *
            Geometry::translation_transform(-center);
        const Transform3d inverse = view_model.inverse();
        const Vec3d origin = inverse * Vec3d(screen_x * half_width, screen_y * half_height, 0.0);
        const Vec3d direction = inverse.linear() * Vec3d(0.0, 0.0, -1.0);
        const std::optional<size_t> face = m_region_editor.pick_face(origin, direction);
        if (!face)
            return;
        const std::vector<uint8_t> previous = m_region_editor.selected_faces();
        m_region_editor.update_selection(*face, m_selection_operation, m_selection_settings);
        if (previous != m_region_editor.selected_faces())
            push_selection_history(previous);
        rebuild_selection_model();
        notify_selection_changed();
        m_canvas->Refresh(false);
    }

    void rebuild_selection_model()
    {
        m_selection_model.reset();
        if (!m_region_editor.ready() || m_region_editor.selected_face_count() == 0)
            return;
        GLModel::Geometry geometry;
        geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};
        const indexed_triangle_set& mesh = m_region_editor.mesh();
        const std::vector<uint8_t>& selected = m_region_editor.selected_faces();
        for (size_t face_index = 0; face_index < mesh.indices.size(); ++face_index) {
            if (!selected[face_index])
                continue;
            const stl_triangle_vertex_indices& face = mesh.indices[face_index];
            const Vec3f& a = mesh.vertices[face[0]];
            const Vec3f& b = mesh.vertices[face[1]];
            const Vec3f& c = mesh.vertices[face[2]];
            Vec3f normal = (b - a).cross(c - a);
            if (normal.squaredNorm() > 1e-12f)
                normal.normalize();
            else
                normal = Vec3f::UnitZ();
            const unsigned int base = static_cast<unsigned int>(geometry.vertices_count());
            geometry.add_vertex(a, normal);
            geometry.add_vertex(b, normal);
            geometry.add_vertex(c, normal);
            geometry.add_triangle(base, base + 1, base + 2);
        }
        if (!geometry.is_empty()) {
            m_selection_model = std::make_unique<GLModel>();
            m_selection_model->init_from(std::move(geometry));
            m_selection_model->set_color(m_selection_preview_color);
        }
    }

    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(m_canvas);
        const bool context_ok = m_context != nullptr && m_context->IsOK();
        const bool current_ok = context_ok && m_canvas->SetCurrent(*m_context);
        if (!m_paint_diagnostics_logged) {
            BOOST_LOG_TRIVIAL(info) << "AI model preview paint: has_model=" << m_has_model
                                    << ", context_ok=" << context_ok
                                    << ", current_ok=" << current_ok
                                    << ", shown=" << m_canvas->IsShownOnScreen();
            m_paint_diagnostics_logged = true;
        }
        if (!current_ok)
            return;
        if (!wxGetApp().init_opengl())
            return;

        int width = 0;
        int height = 0;
        m_canvas->GetClientSize(&width, &height);
#if defined(__APPLE__)
        const double dpi_scale = m_canvas->GetDPIScaleFactor();
        width = std::max(1, int(std::lround(width * dpi_scale)));
        height = std::max(1, int(std::lround(height * dpi_scale)));
#else
        width = std::max(1, width);
        height = std::max(1, height);
#endif
        while (::glGetError() != GL_NO_ERROR) {}
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
        glsafe(::glDisable(GL_SCISSOR_TEST));
        glsafe(::glDisable(GL_BLEND));
        glsafe(::glDisable(GL_STENCIL_TEST));
        glsafe(::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glDepthFunc(GL_LESS));
        glsafe(::glViewport(0, 0, width, height));
        glsafe(::glClearColor(241.0f / 255.0f, 244.0f / 255.0f, 245.0f / 255.0f, 1.0f));
        glsafe(::glClearDepth(1.0));
        glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

        if (m_has_model) {
            GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
            if (shader != nullptr) {
                glsafe(::glEnable(GL_DEPTH_TEST));
                glsafe(::glDisable(GL_CULL_FACE));
                shader->start_using();
                shader->set_uniform("emission_factor", 0.12f);

                const Vec3d size = m_bounds.size().cast<double>();
                const double radius = std::max(0.001, 0.5 * size.norm());
                const double aspect = double(width) / double(height);
                const double half_height = radius * 1.12 / m_zoom;
                const double half_width = half_height * aspect;
                const double near_z = 0.01 * radius;
                const double far_z = 8.0 * radius;
                Transform3d projection = Transform3d::Identity();
                projection.matrix().setZero();
                projection.matrix()(0, 0) = 1.0 / half_width;
                projection.matrix()(1, 1) = 1.0 / half_height;
                projection.matrix()(2, 2) = -2.0 / (far_z - near_z);
                projection.matrix()(2, 3) = -(far_z + near_z) / (far_z - near_z);
                projection.matrix()(3, 3) = 1.0;

                const Vec3d center = m_bounds.center().cast<double>();
                const Transform3d view_model =
                    Geometry::translation_transform(Vec3d(0.0, 0.0, -3.0 * radius)) *
                    Geometry::rotation_transform(Vec3d(m_pitch, m_yaw, 0.0)) *
                    Geometry::translation_transform(-center);
                const Matrix3d normal_matrix = view_model.matrix().block(0, 0, 3, 3).inverse().transpose();
                shader->set_uniform("view_model_matrix", view_model);
                shader->set_uniform("projection_matrix", projection);
                shader->set_uniform("view_normal_matrix", normal_matrix);
                for (const std::unique_ptr<GLModel>& model : m_models)
                    model->render(shader);
                if (m_selection_model != nullptr) {
                    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
                    glsafe(::glPolygonOffset(-1.0f, -1.0f));
                    m_selection_model->render(shader);
                    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
                }
                if (!m_render_diagnostics_logged) {
                    const GLenum error = ::glGetError();
                    BOOST_LOG_TRIVIAL(info) << "AI model preview render: groups=" << m_models.size()
                                            << ", viewport=" << width << "x" << height
                                            << ", shader=" << shader->get_id()
                                            << ", gl_error=" << error;
                    m_render_diagnostics_logged = true;
                }
                shader->stop_using();
                glsafe(::glDisable(GL_DEPTH_TEST));
            } else if (!m_render_diagnostics_logged) {
                BOOST_LOG_TRIVIAL(error) << "AI model preview render: gouraud_light shader is unavailable";
                m_render_diagnostics_logged = true;
            }
        }
        m_canvas->SwapBuffers();
    }

    wxGLCanvas* m_canvas {nullptr};
    wxGLContext* m_context {nullptr};
    std::vector<std::unique_ptr<GLModel>> m_models;
    std::unique_ptr<GLModel> m_selection_model;
    AI::VertexColorRegionEditor m_region_editor;
    std::vector<std::vector<uint8_t>> m_selection_history;
    std::vector<std::string> m_palette;
    BoundingBoxf3 m_bounds;
    wxPoint m_last_mouse;
    wxPoint m_drag_start;
    std::function<void(size_t)> m_selection_changed;
    AI::RegionSelectionSettings m_selection_settings;
    AI::RegionSelectionOperation m_selection_operation {AI::RegionSelectionOperation::Replace};
    ColorRGBA m_selection_preview_color {1.0f, 0.55f, 0.0f, 1.0f};
    double m_yaw {-0.65};
    double m_pitch {0.35};
    double m_zoom {1.0};
    bool m_dragging {false};
    bool m_drag_moved {false};
    bool m_selection_enabled {false};
    bool m_has_model {false};
    bool m_paint_diagnostics_logged {false};
    bool m_render_diagnostics_logged {false};
};

ModelGenerationPanel::ModelGenerationPanel(wxWindow* parent, AI::IModelArtifactConsumer& artifact_consumer,
                                           AI::IPrintablePaletteProvider& palette_provider)
    : wxPanel(parent)
    , m_artifact_consumer(artifact_consumer)
    , m_palette_provider(palette_provider)
    , m_client(AISidecarClient::default_endpoint())
    , m_poll_timer(this, POLL_TIMER_ID)
{
    BOOST_LOG_TRIVIAL(info) << "AI model generation panel: build page";
    SetBackgroundColour(*wxWHITE);
    build_page();
    BOOST_LOG_TRIVIAL(info) << "AI model generation panel: refresh palette";
    refresh_palette();
    BOOST_LOG_TRIVIAL(info) << "AI model generation panel: load library entries";
    load_library_entries();
    BOOST_LOG_TRIVIAL(info) << "AI model generation panel: bind events";
    Bind(wxEVT_TIMER, &ModelGenerationPanel::on_poll, this, POLL_TIMER_ID);
    Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        if (event.IsShown()) {
            refresh_controls();
            if (m_model_preview_ready && m_model_preview != nullptr) {
                wxGetApp().CallAfter([this]() {
                    if (!m_shutdown && m_model_preview != nullptr)
                        m_model_preview->refresh();
                });
            }
        }
        event.Skip();
    });
    m_status->SetLabel(_L("正在检查本地 3D 生成服务..."));
    m_result_summary->SetLabel(_L("本地服务就绪后即可使用 3D 生成功能。"));
    refresh_controls();
    BOOST_LOG_TRIVIAL(info) << "AI model generation panel: constructor complete";
}

ModelGenerationPanel::~ModelGenerationPanel()
{
    shutdown();
}

void ModelGenerationPanel::set_service_availability(bool available, const std::string& message)
{
    if (m_shutdown)
        return;
    m_service_available = available;
    if (available && !m_busy) {
        m_status->SetLabel(_L("本地 3D 生成服务已就绪。"));
        m_result_summary->SetLabel(_L("输入描述、选择参考图，或同时提供两者即可开始。"));
        update_workflow();
        if (!m_restore_checked && m_job_id.empty()) {
            m_restore_checked = true;
            restore_latest_job();
        }
    } else if (!m_busy) {
        m_status->SetLabel(message.empty() ? _L("请配置并启动本地 AI 服务后再生成 3D 模型。")
                                           : _L("本地 AI 服务不可用：") + wxString::FromUTF8(message));
        m_result_summary->SetLabel(_L("3D 生成功能当前不可用。"));
    }
    refresh_controls();
}

void ModelGenerationPanel::restore_latest_job()
{
    if (m_shutdown || !m_service_available || !m_job_id.empty())
        return;
    const uint64_t sequence = ++m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.get_latest(
        [weak, sequence](std::optional<AIModelGenerationClient::JobStatus> status) mutable {
            if (!weak || !status) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(*status)]() mutable {
                if (weak) weak->restore_job(std::move(status), sequence);
            });
        },
        [weak](std::string error) {
            if (!weak) return;
            BOOST_LOG_TRIVIAL(warning) << "Unable to restore the latest generated-model job: " << error;
        });
}

void ModelGenerationPanel::restore_job(AIModelGenerationClient::JobStatus status, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence || !m_job_id.empty())
        return;
    m_job_palette = status.palette;
    m_job_palette_roles = status.palette_roles.empty() ? automatic_palette_roles(status.palette) : status.palette_roles;
    m_palette_roles = m_job_palette_roles;
    m_palette_roles_source = status.palette;
    m_job_use_printable_colors = !status.palette.empty() || status.palette_recommendation.available;
    m_palette = status.palette;
    m_job_style = status.style;
    m_job_custom_style = status.custom_style;
    m_palette_quality_ok = status.palette_quality_ok;
    m_meaningful_palette_count = status.meaningful_palette_count;
    m_meaningful_subject_color_count = status.meaningful_subject_color_count;
    m_job_print_settings = status.print_settings;
    m_job_face_limit = status.face_limit;
    m_job_prompt = wxString::FromUTF8(status.user_prompt);
    if (m_prompt != nullptr)
        m_prompt->SetValue(m_job_prompt);
    if (m_use_printable_colors != nullptr)
        m_use_printable_colors->SetValue(m_job_use_printable_colors);
    if (m_style != nullptr) {
        const int selection = status.style == "q_cartoon" ? 0 : status.style == "low_poly" ? 1 :
                              status.style == "cel_shaded" ? 2 : status.style == "enamel_inlay" ? 3 :
                              status.style == "custom" ? 5 : 4;
        m_style->SetSelection(selection);
    }
    if (m_custom_style != nullptr)
        m_custom_style->SetValue(wxString::FromUTF8(status.custom_style));
    if (m_quality != nullptr) {
        const int selection = status.face_limit == 100000 ? 0 : status.face_limit == 300000 ? 1 :
                              status.face_limit == 500000 ? 2 : 3;
        m_quality->SetSelection(selection);
    }
    if (m_print_width != nullptr) m_print_width->SetValue(status.print_settings.width_mm);
    if (m_nozzle_size != nullptr) m_nozzle_size->SetValue(status.print_settings.nozzle_mm);
    if (m_line_width != nullptr) m_line_width->SetValue(status.print_settings.line_width_mm);
    if (m_minimum_feature != nullptr) m_minimum_feature->SetValue(status.print_settings.minimum_feature_mm);
    if (m_shadow_color != nullptr) {
        const int selection = status.print_settings.shadow_color == "red" ? 1 :
                              status.print_settings.shadow_color == "green" ? 2 :
                              status.print_settings.shadow_color == "white" ? 3 : 0;
        m_shadow_color->SetSelection(selection);
    }
    m_job_image_path.clear();
    if (status.source == "image" && status.input_ready) {
        m_job_image_path = temp_path(status.id + "-input", "png");
        m_selected_image_path = m_job_image_path;
        m_restoring_input = true;
    }
    m_status->SetLabel(_L("正在恢复上次模型生成任务..."));
    handle_status(std::move(status), sequence);
    if (m_restoring_input)
        download_restored_input(sequence);
}

void ModelGenerationPanel::download_restored_input(uint64_t sequence)
{
    const std::string job_id = m_job_id;
    const boost::filesystem::path path = m_job_image_path;
    if (job_id.empty() || path.empty())
        return;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_input(job_id, path,
        [weak, sequence](boost::filesystem::path restored) {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, restored = std::move(restored)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence) return;
                weak->m_selected_image_path = restored;
                weak->m_job_image_path = restored;
                weak->m_restoring_input = false;
                weak->m_selected_image->SetLabel(_L("已恢复上次参考图"));
                weak->show_selected_image_preview();
                if (!weak->m_awaiting_palette_confirmation && weak->m_preview_path.empty() && !weak->m_style_preview_ready)
                    weak->download_preview(sequence);
                weak->refresh_controls();
            });
        },
        [weak, sequence](std::string error) {
            if (!weak) return;
            BOOST_LOG_TRIVIAL(warning) << "Unable to restore the generated-model input image: " << error;
            wxGetApp().CallAfter([weak, sequence]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence) return;
                weak->m_restoring_input = false;
                if (!weak->m_awaiting_palette_confirmation)
                    weak->download_preview(sequence);
            });
        });
}

void ModelGenerationPanel::shutdown()
{
    if (m_shutdown)
        return;
    m_shutdown = true;
    ++m_sequence;
    m_poll_timer.Stop();
    m_client.cancel_current();
    cleanup_files();
}

void ModelGenerationPanel::build_page()
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxPanel(this);
    header->SetBackgroundColour(wxColour(246, 249, 249));
    auto* header_sizer = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(header, wxID_ANY, _L("3D 生成"));
    wxFont title_font = title->GetFont();
    title_font.SetPointSize(title_font.GetPointSize() + 5);
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title->SetFont(title_font);
    title->SetForegroundColour(wxColour(31, 55, 59));
    header_sizer->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    header_sizer->AddSpacer(FromDIP(4));
    header_sizer->Add(new wxStaticText(header, wxID_ANY, _L("通过文字、参考图或两者组合生成可预览、可打印的彩色 3D 模型。")),
                      0, wxLEFT | wxRIGHT, FromDIP(12));
    header_sizer->AddSpacer(FromDIP(10));
    header->SetSizer(header_sizer);
    root->Add(header, 0, wxEXPAND);

    auto* content = new wxBoxSizer(wxHORIZONTAL);
    content->Add(build_workflow_panel(this), 0, wxEXPAND | wxALL, FromDIP(12));

    content->Add(build_preview_panel(this), 1, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(12));
    root->Add(content, 1, wxEXPAND);
    SetSizer(root);
}

wxWindow* ModelGenerationPanel::build_workflow_panel(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(400), -1), wxBORDER_SIMPLE);
    panel->SetMinSize(wxSize(FromDIP(360), -1));
    panel->SetBackgroundColour(wxColour(250, 251, 251));
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* journey = new wxPanel(panel);
    journey->SetBackgroundColour(wxColour(244, 248, 248));
    auto* journey_sizer = new wxBoxSizer(wxVERTICAL);
    auto* step_row = new wxBoxSizer(wxHORIZONTAL);
    const std::array<wxString, 4> step_names = {
        _L("1 输入"), _L("2 图片确认"), _L("3 生成 3D"), _L("4 导入")
    };
    for (size_t index = 0; index < step_names.size(); ++index) {
        m_step_labels[index] = new wxStaticText(journey, wxID_ANY, step_names[index],
                                                wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
        m_step_labels[index]->SetForegroundColour(index == 0 ? wxColour(24, 112, 105) : wxColour(132, 143, 145));
        wxFont step_font = m_step_labels[index]->GetFont();
        step_font.SetWeight(index == 0 ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
        m_step_labels[index]->SetFont(step_font);
        step_row->Add(m_step_labels[index], 1, wxALIGN_CENTER_VERTICAL);
    }
    journey_sizer->Add(step_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    m_workflow_steps = new wxStaticText(journey, wxID_ANY, _L("输入文字、图片，或同时使用两者"));
    m_workflow_steps->SetForegroundColour(wxColour(91, 104, 107));
    journey_sizer->Add(m_workflow_steps, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    auto* progress_header = new wxBoxSizer(wxHORIZONTAL);
    m_workflow_phase = new wxStaticText(journey, wxID_ANY, _L("检查本地服务"));
    wxFont workflow_font = m_workflow_phase->GetFont();
    workflow_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_workflow_phase->SetFont(workflow_font);
    m_progress_percent = new wxStaticText(journey, wxID_ANY, "0%");
    m_progress_percent->SetForegroundColour(wxColour(91, 104, 107));
    progress_header->Add(m_workflow_phase, 1, wxALIGN_CENTER_VERTICAL);
    progress_header->Add(m_progress_percent, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    journey_sizer->Add(progress_header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
    m_generation_progress = new wxGauge(journey, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, FromDIP(6)));
    m_generation_progress->SetValue(0);
    journey_sizer->Add(m_generation_progress, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    journey_sizer->AddSpacer(FromDIP(8));
    journey->SetSizer(journey_sizer);
    outer->Add(journey, 0, wxEXPAND);

    auto* scroll = new wxScrolledWindow(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scroll->SetBackgroundColour(wxColour(250, 251, 251));
    scroll->SetScrollRate(0, FromDIP(12));
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    sizer->Add(section_label(scroll, _L("输入内容")), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    sizer->AddSpacer(FromDIP(4));
    auto* input_hint = new wxStaticText(scroll, wxID_ANY,
                                        _L("文字和图片至少提供一项。\n同时提供时，文字用于描述调整方向。"));
    input_hint->SetForegroundColour(wxColour(91, 104, 107));
    sizer->Add(input_hint, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(6));
    m_prompt_label = new wxStaticText(scroll, wxID_ANY, _L("文字描述（可选）"));
    sizer->Add(m_prompt_label, 0, wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(4));
    m_prompt = new wxTextCtrl(scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(50)),
                              wxTE_MULTILINE | wxTE_NO_VSCROLL);
    m_prompt->SetHint(_L("例如：一只坐在圆形底座上的机械猫"));
    sizer->Add(m_prompt, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(8));

    auto* style_row = new wxBoxSizer(wxHORIZONTAL);
    auto* style_label = new wxStaticText(scroll, wxID_ANY, _L("风格"));
    wxArrayString styles;
    styles.Add(_L("Q 版设计师玩具（彩色推荐）"));
    styles.Add(_L("低多边形"));
    styles.Add(_L("赛璐璐色块"));
    styles.Add(_L("釉彩嵌色摆件"));
    styles.Add(_L("雕塑（适合单色）"));
    styles.Add(_L("自定义"));
    m_style = new wxChoice(scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, styles);
    m_style->SetSelection(0);
    style_row->Add(style_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    style_row->Add(m_style, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(style_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(8));

    m_custom_style_panel = new wxPanel(scroll);
    m_custom_style_panel->SetBackgroundColour(wxColour(250, 251, 251));
    auto* custom_style_sizer = new wxBoxSizer(wxVERTICAL);
    auto* custom_style_label = new wxStaticText(m_custom_style_panel, wxID_ANY, _L("自定义风格描述"));
    m_custom_style = new wxTextCtrl(m_custom_style_panel, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                    wxSize(-1, FromDIP(50)), wxTE_MULTILINE | wxTE_NO_VSCROLL);
    m_custom_style->SetHint(_L("例如：复古木刻玩具，粗轮廓、大色块、哑光材质"));
    m_custom_style->SetMaxLength(240);
    custom_style_sizer->Add(custom_style_label, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    custom_style_sizer->Add(m_custom_style, 0, wxEXPAND);
    m_custom_style_panel->SetSizer(custom_style_sizer);
    m_custom_style_panel->Hide();
    sizer->Add(m_custom_style_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(8));

    auto* image_row = new wxBoxSizer(wxHORIZONTAL);
    m_choose_image = new wxButton(scroll, wxID_ANY, _L("选择图片"));
    m_clear_image = new wxButton(scroll, wxID_ANY, _L("移除"));
    m_selected_image = new wxStaticText(scroll, wxID_ANY, _L("未选择图片"),
                                        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_selected_image->SetMinSize(wxSize(FromDIP(70), -1));
    image_row->Add(m_choose_image, 0, wxRIGHT, FromDIP(8));
    image_row->Add(m_clear_image, 0, wxRIGHT, FromDIP(8));
    image_row->Add(m_selected_image, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(image_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    m_upload_notice = new wxStaticText(scroll, wxID_ANY, _L("仅会将选中的图片和文字描述发送给 AI。"));
    m_upload_notice->Wrap(FromDIP(310));
    m_upload_notice->SetForegroundColour(wxColour(91, 104, 107));
    sizer->AddSpacer(FromDIP(4));
    sizer->Add(m_upload_notice, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(10));

    sizer->Add(section_label(scroll, _L("打印颜色")), 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(6));
    m_use_printable_colors = new wxCheckBox(scroll, wxID_ANY, _L("限制为打印机耗材颜色"));
    m_use_printable_colors->SetValue(true);
    m_use_printable_colors->SetToolTip(_L("开启后只使用下方 1–4 种耗材颜色，生成结果更适合多色打印。"));
    sizer->Add(m_use_printable_colors, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(6));
    wxArrayString palette_sources;
    palette_sources.Add(_L("使用当前耗材"));
    palette_sources.Add(_L("自定义 1–4 种颜色"));
    palette_sources.Add(_L("AI 推荐目标色（用户匹配耗材）"));
    m_palette_source = new wxChoice(scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, palette_sources);
    m_palette_source->SetSelection(0);
    sizer->Add(m_palette_source, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(6));
    m_palette_panel = new wxPanel(scroll);
    m_palette_panel->SetBackgroundColour(wxColour(250, 251, 251));
    m_palette_sizer = new wxGridSizer(6, FromDIP(6), FromDIP(6));
    m_palette_panel->SetSizer(m_palette_sizer);
    sizer->Add(m_palette_panel, 0, wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(5));
    m_palette_summary = new wxStaticText(scroll, wxID_ANY, wxEmptyString);
    m_palette_summary->Wrap(FromDIP(310));
    m_palette_summary->SetForegroundColour(wxColour(91, 104, 107));
    sizer->Add(m_palette_summary, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(6));
    m_custom_color_panel = new wxPanel(scroll);
    m_custom_color_panel->SetBackgroundColour(wxColour(250, 251, 251));
    auto* custom_color_row = new wxBoxSizer(wxHORIZONTAL);
    m_custom_color = new wxColourPickerCtrl(m_custom_color_panel, wxID_ANY, *wxWHITE);
    m_add_custom_color = new wxButton(m_custom_color_panel, wxID_ANY, _L("添加颜色"));
    custom_color_row->Add(m_custom_color, 1, wxRIGHT, FromDIP(8));
    custom_color_row->Add(m_add_custom_color, 0);
    m_custom_color_panel->SetSizer(custom_color_row);
    sizer->Add(m_custom_color_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(6));

    m_palette_recommendation_panel = new wxPanel(scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    m_palette_recommendation_panel->SetBackgroundColour(*wxWHITE);
    auto* recommendation_sizer = new wxBoxSizer(wxVERTICAL);
    auto* recommendation_actions = new wxBoxSizer(wxVERTICAL);
    m_recommend_palette = new wxButton(m_palette_recommendation_panel, wxID_ANY, _L("AI 推荐四色"));
    m_recommend_palette->SetToolTip(_L("根据文字、参考图和风格推荐四个设计目标色；不会修改打印机耗材槽"));
    m_confirm_recommended_palette = new wxButton(
        m_palette_recommendation_panel, wxID_ANY, _L("确认配色并生成预览"));
    recommendation_actions->Add(m_recommend_palette, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    recommendation_actions->Add(m_confirm_recommended_palette, 0, wxEXPAND);
    recommendation_sizer->Add(recommendation_actions, 0, wxEXPAND | wxALL, FromDIP(8));
    m_palette_recommendation_summary = new wxStaticText(
        m_palette_recommendation_panel, wxID_ANY,
        _L("AI 会推荐理想目标色；确认后再由你匹配实际耗材。"));
    m_palette_recommendation_summary->SetForegroundColour(wxColour(91, 104, 107));
    m_palette_recommendation_summary->Wrap(FromDIP(300));
    recommendation_sizer->Add(
        m_palette_recommendation_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    for (size_t index = 0; index < m_palette_recommendation_cards.size(); ++index) {
        auto* card = new wxPanel(m_palette_recommendation_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
        card->SetBackgroundColour(wxColour(250, 251, 251));
        auto* card_sizer = new wxBoxSizer(wxVERTICAL);
        auto* content = new wxBoxSizer(wxHORIZONTAL);
        auto* swatch = new wxPanel(card, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(30, 30)), wxBORDER_SIMPLE);
        swatch->SetMinSize(FromDIP(wxSize(30, 30)));
        auto* details = new wxStaticText(card, wxID_ANY, wxEmptyString);
        details->Wrap(FromDIP(230));
        auto* replace = new wxButton(
            card, wxID_ANY, _L("替换"), wxDefaultPosition, FromDIP(wxSize(52, 28)), wxBU_EXACTFIT);
        auto* remove = new wxButton(
            card, wxID_ANY, _L("删除"), wxDefaultPosition, FromDIP(wxSize(52, 28)), wxBU_EXACTFIT);
        content->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(6));
        content->Add(details, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(6));
        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        actions->AddStretchSpacer();
        actions->Add(replace, 0, wxRIGHT, FromDIP(4));
        actions->Add(remove, 0);
        actions->AddSpacer(FromDIP(20));
        card_sizer->Add(content, 0, wxEXPAND);
        card_sizer->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
        card->SetSizer(card_sizer);
        recommendation_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
        m_palette_recommendation_cards[index] = card;
        m_palette_recommendation_swatches[index] = swatch;
        m_palette_recommendation_details[index] = details;
        m_palette_recommendation_replace[index] = replace;
        m_palette_recommendation_remove[index] = remove;
        replace->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) { replace_recommended_color(index); });
        remove->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            if (index < m_custom_palette.size())
                remove_custom_color(m_custom_palette[index]);
        });
    }
    m_palette_recommendation_panel->SetSizer(recommendation_sizer);
    m_palette_recommendation_panel->Hide();
    sizer->Add(m_palette_recommendation_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(6));

    m_advanced_toggle = new wxButton(scroll, wxID_ANY, _L("显示高级设置"), wxDefaultPosition,
                                     wxSize(-1, FromDIP(30)), wxBU_LEFT);
    m_advanced_toggle->SetToolTip(_L("显示颜色用途、打印尺寸和最小色块设置。"));
    sizer->Add(m_advanced_toggle, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(6));

    m_advanced_options = new wxPanel(scroll);
    m_advanced_options->SetBackgroundColour(wxColour(250, 251, 251));
    auto* advanced = m_advanced_options;
    auto* advanced_sizer = new wxBoxSizer(wxVERTICAL);

    auto* palette_roles_label = new wxStaticText(advanced, wxID_ANY, _L("颜色用途"));
    wxFont palette_roles_font = palette_roles_label->GetFont();
    palette_roles_font.SetWeight(wxFONTWEIGHT_BOLD);
    palette_roles_label->SetFont(palette_roles_font);
    advanced_sizer->Add(palette_roles_label, 0, wxEXPAND);
    auto* palette_roles_hint = new wxStaticText(advanced, wxID_ANY, _L("系统已自动分配；只有效果不理想时才调整。"));
    palette_roles_hint->SetForegroundColour(wxColour(91, 104, 107));
    advanced_sizer->Add(palette_roles_hint, 0, wxEXPAND | wxTOP, FromDIP(4));
    advanced_sizer->AddSpacer(FromDIP(5));

    m_palette_roles_panel = new wxPanel(advanced);
    m_palette_roles_panel->SetBackgroundColour(advanced->GetBackgroundColour());
    auto* palette_roles_sizer = new wxBoxSizer(wxVERTICAL);
    const std::array<wxString, 4> role_labels {_L("主色"), _L("轮廓 / 暗部"), _L("浅色"), _L("点缀色")};
    for (size_t index = 0; index < m_palette_role_choices.size(); ++index) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(m_palette_roles_panel, wxID_ANY, role_labels[index]), 0,
                 wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        m_palette_role_choices[index] = new wxChoice(m_palette_roles_panel, wxID_ANY);
        row->Add(m_palette_role_choices[index], 1, wxALIGN_CENTER_VERTICAL);
        palette_roles_sizer->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    }
    m_palette_roles_panel->SetSizer(palette_roles_sizer);
    advanced_sizer->Add(m_palette_roles_panel, 0, wxEXPAND | wxBOTTOM, FromDIP(6));

    auto* print_constraints_label = new wxStaticText(advanced, wxID_ANY, _L("打印尺寸与细节"));
    wxFont print_constraints_font = print_constraints_label->GetFont();
    print_constraints_font.SetWeight(wxFONTWEIGHT_BOLD);
    print_constraints_label->SetFont(print_constraints_font);
    advanced_sizer->Add(print_constraints_label, 0, wxEXPAND | wxTOP, FromDIP(4));
    const auto add_print_number = [this, advanced, advanced_sizer](const wxString& label, wxSpinCtrlDouble*& control,
                                                                  double value, double minimum, double maximum,
                                                                  double increment, int digits) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(advanced, wxID_ANY, label), 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        control = new wxSpinCtrlDouble(advanced, wxID_ANY);
        control->SetRange(minimum, maximum);
        control->SetIncrement(increment);
        control->SetDigits(digits);
        control->SetValue(value);
        row->Add(control, 0, wxALIGN_CENTER_VERTICAL);
        advanced_sizer->Add(row, 0, wxEXPAND | wxTOP, FromDIP(5));
    };
    add_print_number(_L("打印宽度（mm）"), m_print_width, 160.0, 20.0, 2000.0, 10.0, 1);
    add_print_number(_L("喷嘴直径（mm）"), m_nozzle_size, 0.4, 0.1, 2.0, 0.1, 2);
    add_print_number(_L("挤出线宽（mm）"), m_line_width, 0.4, 0.1, 3.0, 0.05, 2);
    add_print_number(_L("最小特征（mm）"), m_minimum_feature, 0.8, 0.1, 20.0, 0.1, 2);
    m_minimum_feature->SetToolTip(_L("建议不小于两条挤出线宽；过小色块会合并到相邻主色块。"));
    advanced->SetSizer(advanced_sizer);
    m_advanced_options->Hide();
    sizer->Add(m_advanced_options, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(12));

    m_model_settings_panel = new wxPanel(scroll);
    m_model_settings_panel->SetBackgroundColour(wxColour(250, 251, 251));
    auto* model_settings_sizer = new wxBoxSizer(wxVERTICAL);
    model_settings_sizer->Add(section_label(m_model_settings_panel, _L("3D 生成设置")), 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto* quality_row = new wxBoxSizer(wxHORIZONTAL);
    auto* quality_label = new wxStaticText(m_model_settings_panel, wxID_ANY, _L("模型精度"));
    wxArrayString quality_levels;
    quality_levels.Add(_L("10 万面（较快）"));
    quality_levels.Add(_L("30 万面（推荐）"));
    quality_levels.Add(_L("50 万面（精细）"));
    quality_levels.Add(_L("100 万面（最高）"));
    m_quality = new wxChoice(m_model_settings_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, quality_levels);
    m_quality->SetSelection(1);
    m_quality->SetToolTip(_L("面数越高，生成、下载、预览和切片所需时间越长。"));
    quality_row->Add(quality_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    quality_row->Add(m_quality, 1, wxALIGN_CENTER_VERTICAL);
    model_settings_sizer->Add(quality_row, 0, wxEXPAND);
    m_model_settings_panel->SetSizer(model_settings_sizer);
    sizer->Insert(0, m_model_settings_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_import_settings_panel = new wxPanel(scroll);
    m_import_settings_panel->SetBackgroundColour(wxColour(250, 251, 251));
    auto* import_settings_sizer = new wxBoxSizer(wxVERTICAL);
    import_settings_sizer->Add(section_label(m_import_settings_panel, _L("导入与切片")), 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    auto* import_color_row = new wxBoxSizer(wxHORIZONTAL);
    auto* import_color_label = new wxStaticText(m_import_settings_panel, wxID_ANY, _L("颜色处理"));
    wxArrayString import_color_modes;
    import_color_modes.Add(_L("手动匹配打印机耗材（推荐）"));
    import_color_modes.Add(_L("自动匹配当前耗材"));
    import_color_modes.Add(_L("单色导入"));
    m_import_color_mode = new wxChoice(m_import_settings_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, import_color_modes);
    m_import_color_mode->SetSelection(0);
    m_import_color_mode->SetToolTip(
        _L("手动匹配会在导入时确认模型颜色与打印机耗材槽；自动匹配使用当前耗材颜色；单色导入忽略模型颜色。"));
    import_color_row->Add(import_color_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    import_color_row->Add(m_import_color_mode, 1, wxALIGN_CENTER_VERTICAL);
    import_settings_sizer->Add(import_color_row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));

    m_auto_slice_after_import = new wxCheckBox(m_import_settings_panel, wxID_ANY, _L("导入后自动切片"));
    m_auto_slice_after_import->SetValue(true);
    m_auto_slice_after_import->SetToolTip(
        _L("关闭后仍会导入颜色、检查模型并自动摆放，但会停在准备页等待手动切片。"));
    import_settings_sizer->Add(m_auto_slice_after_import, 0, wxEXPAND);
    m_import_settings_panel->SetSizer(import_settings_sizer);
    sizer->Insert(0, m_import_settings_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_preprocess_section = section_label(scroll, _L("确认提示词"));
    sizer->Add(m_preprocess_section, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_prepared_prompt_label = new wxStaticText(scroll, wxID_ANY, _L("用于 3D 生成的提示词"));
    sizer->Add(m_prepared_prompt_label, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_prepared_prompt = new wxTextCtrl(scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(72)), wxTE_MULTILINE);
    sizer->Add(m_prepared_prompt, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    scroll->SetSizer(sizer);
    scroll->FitInside();
    outer->Add(scroll, 1, wxEXPAND);

    auto* action_panel = new wxPanel(panel);
    action_panel->SetBackgroundColour(*wxWHITE);
    auto* action_panel_sizer = new wxBoxSizer(wxVERTICAL);
    m_status = new wxStaticText(action_panel, wxID_ANY, _L("空闲"));
    m_status->Wrap(FromDIP(310));
    m_status->SetForegroundColour(wxColour(60, 75, 78));
    action_panel_sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    auto* action_buttons = new wxBoxSizer(wxHORIZONTAL);
    m_preprocess = new wxButton(action_panel, wxID_ANY, _L("生成图片预览"),
                                wxDefaultPosition, wxSize(-1, FromDIP(38)));
    m_generate = new wxButton(action_panel, wxID_ANY, _L("确认并生成 3D"),
                              wxDefaultPosition, wxSize(-1, FromDIP(38)));
    m_stop = new wxButton(action_panel, wxID_ANY, _L("停止生成"),
                          wxDefaultPosition, wxSize(-1, FromDIP(38)));
    m_import = new wxButton(action_panel, wxID_ANY, _L("导入到准备页"),
                            wxDefaultPosition, wxSize(-1, FromDIP(38)));
    m_discard = new wxButton(action_panel, wxID_ANY, _L("重新开始"),
                             wxDefaultPosition, wxSize(-1, FromDIP(38)));
    action_buttons->Add(m_preprocess, 1, wxRIGHT, FromDIP(8));
    action_buttons->Add(m_generate, 1, wxRIGHT, FromDIP(8));
    action_buttons->Add(m_stop, 1, wxRIGHT, FromDIP(8));
    action_buttons->Add(m_import, 1, wxRIGHT, FromDIP(8));
    action_buttons->Add(m_discard, 0);
    action_panel_sizer->Add(action_buttons, 0, wxEXPAND | wxALL, FromDIP(12));
    action_panel->SetSizer(action_panel_sizer);
    outer->Add(action_panel, 0, wxEXPAND);
    panel->SetSizer(outer);

    m_prompt->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { refresh_controls(); });
    m_style->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_controls(); });
    m_custom_style->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { refresh_controls(); });
    m_quality->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_controls(); });
    m_choose_image->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_choose_image, this);
    m_clear_image->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_clear_image, this);
    m_use_printable_colors->Bind(wxEVT_CHECKBOX, &ModelGenerationPanel::on_printable_colors_toggled, this);
    m_palette_source->Bind(wxEVT_CHOICE, &ModelGenerationPanel::on_palette_source_changed, this);
    m_import_color_mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_controls(); });
    m_auto_slice_after_import->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { refresh_controls(); });
    m_add_custom_color->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_add_custom_color, this);
    m_recommend_palette->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_recommend_palette, this);
    m_confirm_recommended_palette->Bind(
        wxEVT_BUTTON, &ModelGenerationPanel::on_confirm_recommended_palette, this);
    for (size_t index = 0; index < m_palette_role_choices.size(); ++index)
        m_palette_role_choices[index]->Bind(wxEVT_CHOICE, [this, index](wxCommandEvent&) { on_palette_role_changed(index); });
    for (wxSpinCtrlDouble* control : {m_print_width, m_nozzle_size, m_line_width, m_minimum_feature})
        control->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_controls(); });
    m_advanced_toggle->Bind(wxEVT_BUTTON, [this, scroll](wxCommandEvent&) {
        m_advanced_options_expanded = !m_advanced_options_expanded;
        m_advanced_options->Show(m_advanced_options_expanded);
        m_advanced_toggle->SetLabel(m_advanced_options_expanded ? _L("收起高级设置") : _L("显示高级设置"));
        scroll->Layout();
        scroll->FitInside();
    });
    m_preprocess->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_preprocess, this);
    m_generate->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_generate, this);
    m_stop->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_stop, this);
    m_import->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_import, this);
    m_discard->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_discard, this);
    return panel;
}

wxWindow* ModelGenerationPanel::build_preview_panel(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    panel->SetBackgroundColour(*wxWHITE);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    header->Add(section_label(panel, _L("预览结果")), 1, wxALIGN_CENTER_VERTICAL);
    m_preview_kind = new wxStaticText(panel, wxID_ANY, _L("暂无预览"));
    m_preview_kind->SetForegroundColour(wxColour(91, 104, 107));
    wxArrayString preview_stages;
    preview_stages.Add(_L("AI 原图"));
    preview_stages.Add(_L("严格色板"));
    preview_stages.Add(_L("可打印清理"));
    preview_stages.Add(_L("问题热力图"));
    m_preview_stage = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, preview_stages);
    m_preview_stage->SetSelection(2);
    m_zoom_out = new wxButton(panel, wxID_ANY, "-", wxDefaultPosition, wxSize(FromDIP(30), FromDIP(28)));
    m_zoom_fit = new wxButton(panel, wxID_ANY, _L("适应"), wxDefaultPosition, wxSize(FromDIP(54), FromDIP(28)));
    m_zoom_in = new wxButton(panel, wxID_ANY, "+", wxDefaultPosition, wxSize(FromDIP(30), FromDIP(28)));
    m_preview_zoom = new wxStaticText(panel, wxID_ANY, "100%", wxDefaultPosition, wxSize(FromDIP(48), -1), wxALIGN_CENTER_HORIZONTAL);
    m_zoom_out->SetToolTip(_L("缩小图片预览"));
    m_zoom_fit->SetToolTip(_L("完整显示图片"));
    m_zoom_in->SetToolTip(_L("放大图片预览"));
    header->Add(m_preview_kind, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    header->Add(m_preview_stage, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    header->Add(m_zoom_out, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    header->Add(m_zoom_fit, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    header->Add(m_zoom_in, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    header->Add(m_preview_zoom, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    sizer->Add(header, 0, wxEXPAND | wxALL, FromDIP(18));

    m_preview_book = new wxNotebook(panel, wxID_ANY);
    m_preview_area = new wxScrolledWindow(m_preview_book, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxVSCROLL);
    m_preview_area->SetBackgroundColour(wxColour(241, 244, 245));
    m_preview_area->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_preview_area->SetMinSize(wxSize(FromDIP(360), FromDIP(300)));
    m_preview_area->SetScrollRate(FromDIP(12), FromDIP(12));
    m_preview_area->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(m_preview_area);
        dc.SetBackground(wxBrush(m_preview_area->GetBackgroundColour()));
        dc.Clear();
        if (m_reference_preview_pane.IsEmpty() && m_style_preview_pane.IsEmpty())
            return;
        int view_x = 0;
        int view_y = 0;
        int unit_x = 1;
        int unit_y = 1;
        m_preview_area->GetViewStart(&view_x, &view_y);
        m_preview_area->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        const wxPoint offset(view_x * unit_x, view_y * unit_y);
        const int label_height = FromDIP(32);

        auto draw_pane = [&](const wxRect& virtual_rect, const wxString& label, const wxBitmap& bitmap,
                             const wxString& placeholder, bool ai_result) {
            if (virtual_rect.IsEmpty())
                return;
            wxRect rect = virtual_rect;
            rect.Offset(-offset.x, -offset.y);
            dc.SetPen(wxPen(wxColour(204, 213, 215)));
            dc.SetBrush(wxBrush(wxColour(255, 255, 255)));
            dc.DrawRectangle(rect);

            const wxRect label_rect(rect.x, rect.y, rect.width, label_height);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(ai_result ? wxColour(229, 244, 242) : wxColour(235, 239, 240)));
            dc.DrawRectangle(label_rect);
            wxFont label_font = dc.GetFont();
            label_font.SetWeight(wxFONTWEIGHT_BOLD);
            dc.SetFont(label_font);
            dc.SetTextForeground(ai_result ? wxColour(24, 112, 105) : wxColour(60, 75, 78));
            const wxSize label_size = dc.GetTextExtent(label);
            dc.DrawText(label, label_rect.x + FromDIP(10), label_rect.y + (label_rect.height - label_size.y) / 2);

            const wxRect image_rect(rect.x, rect.y + label_height, rect.width, rect.height - label_height);
            if (bitmap.IsOk()) {
                const int x = image_rect.x + (image_rect.width - bitmap.GetWidth()) / 2;
                const int y = image_rect.y + (image_rect.height - bitmap.GetHeight()) / 2;
                if (ai_result) {
                    const int tile = FromDIP(12);
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    for (int row = 0; row * tile < bitmap.GetHeight(); ++row) {
                        for (int column = 0; column * tile < bitmap.GetWidth(); ++column) {
                            const bool alternate = (row + column) % 2 != 0;
                            dc.SetBrush(wxBrush(alternate ? wxColour(228, 232, 233) : wxColour(248, 249, 249)));
                            dc.DrawRectangle(x + column * tile, y + row * tile,
                                             std::min(tile, bitmap.GetWidth() - column * tile),
                                             std::min(tile, bitmap.GetHeight() - row * tile));
                        }
                    }
                }
                dc.DrawBitmap(bitmap, x, y, true);
            } else if (!placeholder.empty()) {
                wxFont placeholder_font = dc.GetFont();
                placeholder_font.SetWeight(wxFONTWEIGHT_NORMAL);
                dc.SetFont(placeholder_font);
                dc.SetTextForeground(wxColour(108, 120, 123));
                const wxSize text_size = dc.GetTextExtent(placeholder);
                dc.DrawText(placeholder,
                            image_rect.x + std::max(FromDIP(8), (image_rect.width - text_size.x) / 2),
                            image_rect.y + std::max(FromDIP(8), (image_rect.height - text_size.y) / 2));
            }
        };

        const wxString reference_label = m_reference_image.IsOk() ? _L("参考图") : _L("AI 原图");
        draw_pane(m_reference_preview_pane, reference_label, m_reference_bitmap, wxEmptyString, false);
        const wxString result_label = m_preview_stage != nullptr && m_preview_stage->GetSelection() != wxNOT_FOUND
            ? m_preview_stage->GetStringSelection() : _L("AI 处理图");
        draw_pane(m_style_preview_pane, result_label, m_style_preview_bitmap, m_style_preview_placeholder, true);
    });
    m_preview_area->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        update_preview_view();
        event.Skip();
    });
    m_preview_book->AddPage(m_preview_area, _L("图片确认"), true);

    auto* model_page = new wxPanel(m_preview_book);
    model_page->SetBackgroundColour(wxColour(241, 244, 245));
    auto* model_sizer = new wxBoxSizer(wxVERTICAL);
    auto* model_toolbar = new wxBoxSizer(wxHORIZONTAL);
    m_model_stats = new wxStaticText(model_page, wxID_ANY, _L("模型生成后将在这里显示"));
    m_model_stats->SetForegroundColour(wxColour(91, 104, 107));
    m_reset_model_view = new wxButton(model_page, wxID_ANY, _L("重置视角"));
    m_reset_model_view->SetToolTip(_L("恢复模型的默认视角和缩放"));
    model_toolbar->Add(m_model_stats, 1, wxALIGN_CENTER_VERTICAL);
    model_toolbar->Add(m_reset_model_view, 0, wxLEFT, FromDIP(12));
    model_sizer->Add(model_toolbar, 0, wxEXPAND | wxALL, FromDIP(12));
    m_model_preview = new ModelPreview3D(model_page);
    model_sizer->Add(m_model_preview, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    m_local_recolor_panel = new wxPanel(
        model_page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    m_local_recolor_panel->SetBackgroundColour(*wxWHITE);
    auto* recolor_sizer = new wxBoxSizer(wxVERTICAL);
    auto* recolor_header = new wxBoxSizer(wxHORIZONTAL);
    m_local_recolor_toggle = new wxToggleButton(
        m_local_recolor_panel, wxID_ANY, _L("编辑局部颜色"));
    m_local_recolor_toggle->SetMinSize(wxSize(FromDIP(150), FromDIP(38)));
    m_local_recolor_toggle->SetToolTip(_L("打开局部改色工具，在模型上直接选择需要换色的部位"));
    auto* recolor_intro = new wxStaticText(
        m_local_recolor_panel, wxID_ANY, _L("在模型上选择部位，并换成当前打印机耗材颜色"));
    recolor_intro->SetForegroundColour(wxColour(91, 104, 107));
    recolor_header->Add(m_local_recolor_toggle, 0, wxALIGN_CENTER_VERTICAL);
    recolor_header->Add(recolor_intro, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    recolor_sizer->Add(recolor_header, 0, wxEXPAND | wxALL, FromDIP(10));

    m_local_recolor_controls = new wxPanel(m_local_recolor_panel);
    m_local_recolor_controls->SetBackgroundColour(*wxWHITE);
    auto* controls_sizer = new wxBoxSizer(wxVERTICAL);
    auto* material_row = new wxBoxSizer(wxHORIZONTAL);
    material_row->Add(new wxStaticText(m_local_recolor_controls, wxID_ANY, _L("智能区域")), 0,
                      wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    for (size_t index = 0; index < m_region_material_buttons.size(); ++index) {
        m_region_material_buttons[index] = new wxButton(
            m_local_recolor_controls, wxID_ANY,
            wxString::Format(_L("材料 %llu"), static_cast<unsigned long long>(index + 1)));
        m_region_material_buttons[index]->SetMinSize(wxSize(FromDIP(112), FromDIP(42)));
        material_row->Add(m_region_material_buttons[index], 0,
                          wxALIGN_CENTER_VERTICAL | (index == 0 ? 0 : wxLEFT), FromDIP(6));
    }
    auto* material_hint = new wxStaticText(
        m_local_recolor_controls, wxID_ANY, _L("按 AI 色彩角色选中全部同类材料面，可再手动增减"));
    material_hint->SetForegroundColour(wxColour(91, 104, 107));
    material_row->Add(material_hint, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    controls_sizer->Add(material_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    auto* selection_row = new wxBoxSizer(wxHORIZONTAL);
    selection_row->Add(new wxStaticText(m_local_recolor_controls, wxID_ANY, _L("选择部位")), 0,
                       wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    const std::array<wxString, 3> operation_labels {
        _L("自动识别"), _L("添加区域"), _L("减去区域")
    };
    const std::array<wxString, 3> operation_tips {
        _L("点击一个部位，自动识别相邻的同色连续区域"),
        _L("在现有选区上继续添加局部区域"),
        _L("从现有选区中擦除局部区域")
    };
    for (size_t index = 0; index < m_region_operation_buttons.size(); ++index) {
        m_region_operation_buttons[index] = new wxToggleButton(
            m_local_recolor_controls, wxID_ANY, operation_labels[index]);
        m_region_operation_buttons[index]->SetMinSize(wxSize(FromDIP(94), FromDIP(36)));
        m_region_operation_buttons[index]->SetToolTip(operation_tips[index]);
        selection_row->Add(m_region_operation_buttons[index], 0,
                           wxALIGN_CENTER_VERTICAL | (index == 0 ? 0 : wxLEFT), FromDIP(4));
    }
    wxArrayString region_ranges;
    region_ranges.Add(_L("精细"));
    region_ranges.Add(_L("标准"));
    region_ranges.Add(_L("宽松"));
    m_region_range = new wxChoice(
        m_local_recolor_controls, wxID_ANY, wxDefaultPosition, wxDefaultSize, region_ranges);
    m_region_range->SetSelection(1);
    selection_row->Add(new wxStaticText(m_local_recolor_controls, wxID_ANY, _L("识别范围")), 0,
                       wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(12));
    selection_row->Add(m_region_range, 0, wxALIGN_CENTER_VERTICAL);
    m_region_selection_summary = new wxStaticText(
        m_local_recolor_controls, wxID_ANY, _L("点击模型选择要改色的部位"));
    m_region_selection_summary->SetForegroundColour(wxColour(91, 104, 107));
    selection_row->Add(m_region_selection_summary, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    m_undo_region_selection = new wxButton(m_local_recolor_controls, wxID_ANY, _L("撤销"));
    m_undo_region_selection->SetToolTip(_L("撤销最近一次选区变化（Ctrl+Z）"));
    m_clear_region_selection = new wxButton(m_local_recolor_controls, wxID_ANY, _L("清空"));
    m_clear_region_selection->SetToolTip(_L("清空当前选区（Esc）"));
    selection_row->Add(m_undo_region_selection, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    selection_row->Add(m_clear_region_selection, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    controls_sizer->Add(selection_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    auto* color_row = new wxBoxSizer(wxHORIZONTAL);
    color_row->Add(new wxStaticText(m_local_recolor_controls, wxID_ANY, _L("改成")), 0,
                   wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    for (size_t index = 0; index < m_region_color_buttons.size(); ++index) {
        m_region_color_buttons[index] = new wxToggleButton(
            m_local_recolor_controls, wxID_ANY,
            wxString::Format(_L("耗材 %llu"), static_cast<unsigned long long>(index + 1)));
        m_region_color_buttons[index]->SetMinSize(wxSize(FromDIP(96), FromDIP(46)));
        color_row->Add(m_region_color_buttons[index], 0,
                       wxALIGN_CENTER_VERTICAL | (index == 0 ? 0 : wxLEFT), FromDIP(6));
    }
    color_row->AddStretchSpacer();
    m_apply_region_color = new wxButton(
        m_local_recolor_controls, wxID_ANY, _L("选择部位后应用"));
    m_apply_region_color->SetMinSize(wxSize(FromDIP(150), FromDIP(42)));
    color_row->Add(m_apply_region_color, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    controls_sizer->Add(color_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    auto* recolor_hint = new wxStaticText(
        m_local_recolor_controls, wxID_ANY,
        _L("短按选择 · 拖动旋转 · 滚轮缩放 · Esc 清空 · Ctrl+Z 撤销"));
    recolor_hint->SetForegroundColour(wxColour(91, 104, 107));
    controls_sizer->Add(recolor_hint, 0, wxEXPAND | wxALL, FromDIP(10));
    m_local_recolor_controls->SetSizer(controls_sizer);
    m_local_recolor_controls->Hide();
    recolor_sizer->Add(m_local_recolor_controls, 0, wxEXPAND);
    m_local_recolor_panel->SetSizer(recolor_sizer);
    model_sizer->Add(m_local_recolor_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    m_model_quality_panel = new wxPanel(model_page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    auto* quality_sizer = new wxBoxSizer(wxVERTICAL);
    auto* quality_header = new wxBoxSizer(wxHORIZONTAL);
    m_model_quality_status = new wxStaticText(m_model_quality_panel, wxID_ANY, _L("尚未检查"));
    wxFont quality_font = m_model_quality_status->GetFont();
    quality_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_model_quality_status->SetFont(quality_font);
    m_recheck_model = new wxButton(m_model_quality_panel, wxID_ANY, _L("重新检查"));
    m_recheck_model->SetToolTip(_L("使用本地结构门禁重新检查当前 OBJ，不会调用付费 AI"));
    m_locate_overhang_regions = new wxButton(m_model_quality_panel, wxID_ANY, _L("定位悬垂面"));
    m_locate_overhang_regions->SetToolTip(
        _L("高亮显著的离床向下面，便于旋转检查；不会自动添加支撑或改变切片参数"));
    quality_header->Add(m_model_quality_status, 1, wxALIGN_CENTER_VERTICAL);
    quality_header->Add(m_locate_overhang_regions, 0, wxLEFT, FromDIP(12));
    quality_header->Add(m_recheck_model, 0, wxLEFT, FromDIP(12));
    quality_sizer->Add(quality_header, 0, wxEXPAND | wxALL, FromDIP(10));
    m_model_quality_summary = new wxStaticText(m_model_quality_panel, wxID_ANY, _L("模型生成或加载后可进行结构检查。"));
    m_model_quality_summary->Wrap(FromDIP(500));
    quality_sizer->Add(m_model_quality_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_model_quality_details_pane = new wxCollapsiblePane(m_model_quality_panel, wxID_ANY, _L("查看检查指标"));
    auto* quality_details_sizer = new wxBoxSizer(wxVERTICAL);
    m_model_quality_details = new wxStaticText(m_model_quality_details_pane->GetPane(), wxID_ANY, wxEmptyString);
    m_model_quality_details->Wrap(FromDIP(480));
    quality_details_sizer->Add(m_model_quality_details, 0, wxEXPAND | wxALL, FromDIP(8));
    m_model_quality_details_pane->GetPane()->SetSizer(quality_details_sizer);
    quality_sizer->Add(m_model_quality_details_pane, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    auto* visual_header = new wxBoxSizer(wxHORIZONTAL);
    m_visual_quality_status = new wxStaticText(m_model_quality_panel, wxID_ANY, _L("AI 视觉复核：未运行"));
    wxFont visual_font = m_visual_quality_status->GetFont();
    visual_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_visual_quality_status->SetFont(visual_font);
    m_visual_review_model = new wxButton(m_model_quality_panel, wxID_ANY, _L("AI 视觉复核"));
    m_visual_review_model->SetToolTip(_L("生成最终 OBJ 的五视图并调用付费 AI 检查外观；结果仅供复核，不会阻断导入"));
    visual_header->Add(m_visual_quality_status, 1, wxALIGN_CENTER_VERTICAL);
    visual_header->Add(m_visual_review_model, 0, wxLEFT, FromDIP(12));
    quality_sizer->Add(visual_header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    m_visual_quality_summary = new wxStaticText(m_model_quality_panel, wxID_ANY,
        _L("模型准备好后可按需生成五视图并进行 AI 外观复核。"));
    m_visual_quality_summary->Wrap(FromDIP(500));
    quality_sizer->Add(m_visual_quality_summary, 0, wxEXPAND | wxALL, FromDIP(10));
    m_model_quality_panel->SetSizer(quality_sizer);
    model_sizer->Add(m_model_quality_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    m_model_preview_message = new wxStaticText(model_page, wxID_ANY, _L("生成完成后可拖动旋转模型，并使用滚轮缩放。"));
    m_model_preview_message->SetForegroundColour(wxColour(91, 104, 107));
    model_sizer->Add(m_model_preview_message, 0, wxEXPAND | wxALL, FromDIP(12));
    model_page->SetSizer(model_sizer);
    m_preview_book->AddPage(model_page, _L("3D 预览"), false);
    m_preview_book->AddPage(build_model_library(m_preview_book), _L("历史模型"), false);
    sizer->Add(m_preview_book, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));

    m_preview_message = new wxStaticText(panel, wxID_ANY, _L("请先输入描述或选择参考图。"));
    m_preview_message->SetForegroundColour(wxColour(91, 104, 107));
    sizer->Add(m_preview_message, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));

    m_result_summary = new wxStaticText(panel, wxID_ANY, _L("尚未生成模型。"));
    m_result_summary->Wrap(FromDIP(520));
    sizer->Add(m_result_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));
    panel->SetSizer(sizer);

    m_zoom_out->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_preview_zoom(m_preview_zoom_factor / 1.25); });
    m_zoom_fit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_preview_zoom(1.0); });
    m_zoom_in->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_preview_zoom(m_preview_zoom_factor * 1.25); });
    m_preview_stage->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { apply_preview_stage(true); });
    m_reset_model_view->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_model_preview != nullptr)
            m_model_preview->reset_view();
    });
    m_recheck_model->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_recheck_model, this);
    m_locate_overhang_regions->Bind(wxEVT_BUTTON, [this, model_page](wxCommandEvent&) {
        if (m_model_preview == nullptr)
            return;
        const size_t localized = m_model_preview->select_elevated_overhang_regions();
        if (localized == 0) {
            m_status->SetLabel(_L("当前模型没有达到显著阈值的离床悬垂区域。"));
            return;
        }
        m_local_recolor_toggle->SetValue(true);
        refresh_local_recolor_controls();
        model_page->Layout();
        if (m_preview_area != nullptr)
            m_preview_area->FitInside();
        m_model_preview_message->SetLabel(wxString::Format(
            _L("已高亮 %llu 个悬垂三角面；可旋转检查，或在局部区域工具中手动增减。"),
            static_cast<unsigned long long>(localized)));
        m_status->SetLabel(_L("已定位显著局部悬垂；这里只做风险复核，不会自动生成支撑。"));
    });
    m_visual_review_model->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_visual_review_model, this);
    m_local_recolor_toggle->Bind(wxEVT_TOGGLEBUTTON, [this, model_page](wxCommandEvent&) {
        refresh_local_recolor_controls();
        model_page->Layout();
    });
    const auto update_region_mode = [this]() {
        if (m_model_preview == nullptr)
            return;
        m_model_preview->set_selection_operation(
            m_region_operation_index == 2 ? AI::RegionSelectionOperation::Remove :
            m_region_operation_index == 1 ? AI::RegionSelectionOperation::Add :
                                            AI::RegionSelectionOperation::Replace);
        const int range = m_region_range->GetSelection();
        AI::RegionSelectionSettings settings;
        if (range == 0)
            settings = {0.06f, 50.0f, 0.020f};
        else if (range == 2)
            settings = {0.24f, 85.0f, 0.060f};
        m_model_preview->set_selection_settings(settings);
    };
    for (size_t index = 0; index < m_region_operation_buttons.size(); ++index) {
        m_region_operation_buttons[index]->Bind(wxEVT_TOGGLEBUTTON, [this, index, update_region_mode](wxCommandEvent&) {
            m_region_operation_index = static_cast<int>(index);
            update_region_mode();
            refresh_local_recolor_controls();
        });
    }
    m_region_range->Bind(wxEVT_CHOICE, [update_region_mode](wxCommandEvent&) { update_region_mode(); });
    for (size_t index = 0; index < m_region_color_buttons.size(); ++index) {
        m_region_color_buttons[index]->Bind(wxEVT_TOGGLEBUTTON, [this, index](wxCommandEvent&) {
            m_region_color_index = static_cast<int>(index);
            refresh_local_recolor_controls();
        });
    }
    for (size_t index = 0; index < m_region_material_buttons.size(); ++index) {
        m_region_material_buttons[index]->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            if (m_model_preview != nullptr)
                m_model_preview->select_palette_material(index);
        });
    }
    m_undo_region_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_model_preview != nullptr)
            m_model_preview->undo_selection();
    });
    m_clear_region_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_model_preview != nullptr)
            m_model_preview->clear_selection();
    });
    m_apply_region_color->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_apply_local_recolor, this);
    m_model_preview->set_selection_changed_callback([this](size_t selected_faces) {
        if (m_region_selection_summary != nullptr) {
            m_region_selection_summary->SetLabel(selected_faces == 0
                ? _L("点击模型选择要改色的部位")
                : wxString::Format(_L("已选择区域 · %llu 个三角面"),
                                   static_cast<unsigned long long>(selected_faces)));
        }
        if (m_model_preview_message != nullptr) {
            m_model_preview_message->SetLabel(selected_faces == 0
                ? _L("生成完成后可拖动旋转模型，并使用滚轮缩放。")
                : wxString::Format(_L("当前选区包含 %llu 个三角面；可继续检查或手动增减。"),
                                   static_cast<unsigned long long>(selected_faces)));
        }
        refresh_local_recolor_controls();
    });
    update_region_mode();
    m_preview_book->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this, panel](wxBookCtrlEvent& event) {
        const int selection = event.GetSelection();
        const bool image_page = selection == 0;
        m_zoom_out->Show(image_page);
        m_zoom_fit->Show(image_page);
        m_zoom_in->Show(image_page);
        m_preview_zoom->Show(image_page);
        m_preview_stage->Show(image_page);
        m_preview_kind->SetLabel(selection == 0 ? _L("图片对照") : selection == 1 ? _L("3D 模型") : _L("模型库"));
        panel->Layout();
        if (selection == 1 && m_model_preview != nullptr)
            m_model_preview->refresh();
        event.Skip();
    });
    return panel;
}

wxWindow* ModelGenerationPanel::build_model_library(wxWindow* parent)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    panel->SetBackgroundColour(*wxWHITE);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(section_label(panel, _L("模型库")), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(18));
    auto* session = new wxStaticText(panel, wxID_ANY, _L("历史生成结果 · 双击加载预览"));
    session->SetForegroundColour(wxColour(91, 104, 107));
    sizer->Add(session, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(18));
    m_library_empty = new wxStaticText(panel, wxID_ANY, _L("generated_models 中还没有可用的 OBJ 模型。"));
    m_library_empty->SetForegroundColour(wxColour(110, 122, 125));
    sizer->Add(m_library_empty, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(18));
    m_library_scroller = new wxScrolledWindow(panel, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(-1, 180)), wxVSCROLL);
    m_library_scroller->SetScrollRate(0, FromDIP(8));
    m_library_sizer = new wxBoxSizer(wxVERTICAL);
    m_library_scroller->SetSizer(m_library_sizer);
    sizer->Add(m_library_scroller, 1, wxEXPAND | wxALL, FromDIP(12));
    panel->SetSizer(sizer);
    refresh_library();
    return panel;
}

void ModelGenerationPanel::on_choose_image(wxCommandEvent&)
{
    wxFileDialog dialog(this, _L("选择参考图"), wxEmptyString, wxEmptyString,
                        _L("PNG 和 JPEG 图片 (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;
    boost::filesystem::path path(dialog.GetPath().ToStdWstring());
    if (!is_supported_image(path)) {
        MessageDialog error(this, _L("请选择不超过 20 MB 的有效 PNG 或 JPEG 图片。"), wxEmptyString, wxOK | wxICON_ERROR);
        error.ShowModal();
        return;
    }
    if (!m_job_id.empty() && !m_awaiting_palette_confirmation)
        reset(true);
    m_selected_image_path = std::move(path);
    m_style_preview_ready = false;
    m_raw_preview_available = false;
    m_strict_preview_available = false;
    m_heatmap_available = false;
    const size_t bytes = boost::filesystem::file_size(m_selected_image_path);
    m_selected_image->SetLabel(wxString::FromUTF8(m_selected_image_path.filename().string()) +
                               wxString::Format(" (%llu KB)", static_cast<unsigned long long>((bytes + 1023) / 1024)));
    show_selected_image_preview();
    refresh_controls();
}

void ModelGenerationPanel::on_clear_image(wxCommandEvent&)
{
    if (!m_job_id.empty() && !m_awaiting_palette_confirmation)
        reset(true);
    m_selected_image_path.clear();
    m_selected_image->SetLabel(_L("未选择图片"));
    set_preview_empty(_L("请输入描述、选择参考图，或同时提供两者。"));
    refresh_controls();
}

void ModelGenerationPanel::on_palette_source_changed(wxCommandEvent&)
{
    if (m_palette_source->GetSelection() == 1 && m_custom_palette.empty())
        m_custom_palette = project_palette();
    refresh_palette();
    refresh_controls();
}

void ModelGenerationPanel::on_printable_colors_toggled(wxCommandEvent&)
{
    refresh_palette();
    refresh_controls();
}

void ModelGenerationPanel::on_add_custom_color(wxCommandEvent&)
{
    if (m_palette_source->GetSelection() == 0)
        return;
    if (m_custom_palette.size() >= 4) {
        MessageDialog dlg(this, _L("可打印风格最多支持 4 种耗材颜色。"), wxEmptyString, wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }
    std::string color = m_custom_color->GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    std::transform(color.begin(), color.end(), color.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    if (std::find(m_custom_palette.begin(), m_custom_palette.end(), color) == m_custom_palette.end()) {
        m_custom_palette.emplace_back(std::move(color));
        if (m_palette_source->GetSelection() == 2)
            m_user_adjusted_palette_colors.emplace_back(m_custom_palette.back());
    }
    refresh_palette();
    refresh_controls();
}

void ModelGenerationPanel::on_recommend_palette(wxCommandEvent&)
{
    const std::string prompt = m_prompt->GetValue().ToUTF8().data();
    const bool image_mode = has_image_input();
    if (prompt.empty() && !image_mode) {
        MessageDialog dlg(this, _L("请先输入描述或选择参考图。"), wxEmptyString, wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }
    if (current_style() == "custom" && current_custom_style().empty()) {
        MessageDialog dlg(this, _L("请描述希望使用的自定义风格。"), wxEmptyString, wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        m_custom_style->SetFocus();
        return;
    }
    MessageDialog confirm(
        this,
        image_mode
            ? _L("要让 AI 根据文字、参考图和风格推荐四个设计目标色吗？\n\n此操作可能消耗 API 额度；不会修改打印机耗材槽。")
            : _L("要让 AI 根据文字和风格推荐四个设计目标色吗？\n\n此操作可能消耗 API 额度；不会修改打印机耗材槽。"),
        _L("AI 推荐四色"), wxYES_NO | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;

    reset(true);
    m_palette_source->SetSelection(2);
    m_job_palette.clear();
    m_job_palette_roles.clear();
    m_job_use_printable_colors = true;
    m_job_prompt = m_prompt->GetValue();
    m_job_style = current_style();
    m_job_custom_style = current_custom_style();
    m_job_print_settings = current_print_settings();
    m_job_image_path = m_selected_image_path;
    m_palette_recommendation_confirmed = false;
    m_awaiting_palette_confirmation = false;
    m_busy = true;
    const uint64_t sequence = ++m_sequence;
    update_progress(3, 1, _L("推荐打印配色"));
    m_status->SetLabel(_L("AI 正在分析主体、风格和打印色区..."));
    m_result_summary->SetLabel(_L("推荐完成后可以替换、删除或补充颜色，再确认生成图片预览。"));
    refresh_controls();

    wxWeakRef<ModelGenerationPanel> weak(this);
    auto success = [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
        if (!weak) return;
        wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
            if (weak) weak->handle_status(std::move(status), sequence);
        });
    };
    auto failure = [weak, sequence](std::string error) mutable {
        if (!weak) return;
        wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
            if (weak) weak->handle_error(error, sequence);
        });
    };
    if (image_mode) {
        m_client.recommend_image_palette(new_request_id(), prompt, m_selected_image_path, m_job_style,
                                         m_job_custom_style, m_job_print_settings,
                                         std::move(success), std::move(failure));
    } else {
        m_client.recommend_text_palette(new_request_id(), prompt, m_job_style, m_job_custom_style,
                                        m_job_print_settings, std::move(success), std::move(failure));
    }
}

void ModelGenerationPanel::on_confirm_recommended_palette(wxCommandEvent& event)
{
    if (!m_awaiting_palette_confirmation || m_job_id.empty() || m_custom_palette.empty())
        return;
    if (!job_base_inputs_match()) {
        MessageDialog confirm(
            this,
            _L("输入内容已经变化。要保留当前推荐配色，并用新的输入生成图片预览吗？\n\n此操作可能消耗 API 额度。"),
            _L("继续使用当前配色"), wxYES_NO | wxICON_QUESTION);
        if (confirm.ShowModal() != wxID_YES)
            return;
        on_preprocess(event);
        return;
    }
    MessageDialog confirm(
        this,
        _L("确认使用当前目标色生成可打印图片预览吗？\n\n目标色不会自动绑定耗材槽；此操作可能消耗 API 额度。"),
        _L("确认推荐配色"), wxYES_NO | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;

    m_job_palette = current_palette();
    m_job_palette_roles = current_palette_roles();
    m_palette_recommendation_confirmed = true;
    m_awaiting_palette_confirmation = false;
    m_busy = true;
    const uint64_t sequence = m_sequence;
    update_progress(10, 2, _L("生成可打印预览"));
    m_status->SetLabel(_L("正在应用确认的目标色并生成图片预览..."));
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.confirm_palette(
        m_job_id, m_job_palette, m_job_palette_roles,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak) weak->handle_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::on_preprocess(wxCommandEvent&)
{
    const std::string entered_prompt = m_prompt->GetValue().ToUTF8().data();
    const bool image_mode = has_image_input();
    if (entered_prompt.empty() && !image_mode) {
        MessageDialog dlg(this, _L("请先输入描述或选择参考图。"), wxEmptyString, wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }
    const std::string custom_style = current_custom_style();
    if (current_style() == "custom" && custom_style.empty()) {
        MessageDialog dlg(this, _L("请描述希望使用的自定义风格。"), wxEmptyString, wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        m_custom_style->SetFocus();
        return;
    }
    const std::string prompt = entered_prompt;
    const std::vector<std::string> palette = current_palette();
    if (use_printable_colors() && palette.empty()) {
        MessageDialog dlg(this, _L("生成可打印模型前，请至少配置一种有效耗材颜色。"), wxEmptyString, wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }
    if (use_printable_colors() && m_minimum_feature->GetValue() < m_line_width->GetValue()) {
        MessageDialog dlg(this, _L("最小特征不能小于挤出线宽。建议设置为两条线宽，例如 0.8 mm。"),
                          wxEmptyString, wxOK | wxICON_INFORMATION);
        dlg.ShowModal();
        return;
    }
    if (image_mode) {
        static const std::regex absolute_path(R"(^\s*(?:[A-Za-z]:[\\/]|/).*)");
        if (!entered_prompt.empty() && std::regex_match(entered_prompt, absolute_path)) {
            MessageDialog dlg(this, _L("请描述希望 AI 如何处理图片，不要在描述中粘贴本地文件路径。"), wxEmptyString, wxOK | wxICON_INFORMATION);
            dlg.ShowModal();
            return;
        }
        wxString message;
        message << _L("要使用这张图片生成 AI 风格预览吗？\n\n")
                << wxString::FromUTF8(m_selected_image_path.filename().string()) << "\n"
                << _L("仅会发送这张图片和文字描述，此操作可能消耗 API 额度。 ");
        MessageDialog confirm(this, message, _L("生成风格预览"), wxYES_NO | wxICON_QUESTION);
        if (confirm.ShowModal() != wxID_YES)
            return;
    } else if (use_printable_colors()) {
        MessageDialog confirm(this,
            _L("要根据文字生成 AI 可打印颜色预览吗？\n\n将生成 AI 原图、严格色板图和可打印清理图，此操作可能消耗 API 额度。"),
            _L("生成可打印预览"), wxYES_NO | wxICON_QUESTION);
        if (confirm.ShowModal() != wxID_YES)
            return;
    }

    reset(true);
    m_job_palette = palette;
    m_job_palette_roles = current_palette_roles();
    m_job_use_printable_colors = use_printable_colors();
    m_job_prompt = m_prompt->GetValue();
    m_job_style = current_style();
    m_job_custom_style = custom_style;
    m_job_face_limit = current_face_limit();
    m_job_print_settings = current_print_settings();
    m_job_image_path = m_selected_image_path;
    m_palette_recommendation_confirmed = m_palette_source->GetSelection() == 2 &&
                                         m_palette_recommendation.available;
    m_busy = true;
    const bool preview_mode = image_mode || m_job_use_printable_colors;
    if (preview_mode) {
        m_style_preview_placeholder = _L("正在生成...");
        if (m_reference_image.IsOk()) {
            m_preview_message->SetLabel(
                wxString::Format(_L("参考图 %d × %d px  ·  正在生成 AI 处理图"),
                                 m_reference_image.GetWidth(), m_reference_image.GetHeight()));
        }
        update_preview_view();
    }
    const uint64_t sequence = ++m_sequence;
    const wxString prepare_phase = preview_mode ? _L("生成可打印预览") : _L("准备提示词");
    update_progress(10, 2, prepare_phase);
    m_workflow_phase->SetLabel(prepare_phase);
    m_status->SetLabel(preview_mode ? _L("正在生成并清理可打印颜色预览...") : _L("正在准备 3D 提示词..."));
    m_result_summary->SetLabel(preview_mode ? _L("完成后可对照 AI 原图、严格色板图和清理结果。")
                                            : _L("正在整理用于 3D 生成的提示词。"));
    refresh_controls();

    wxWeakRef<ModelGenerationPanel> weak(this);
    auto success = [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
        if (!weak)
            return;
        wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
            if (weak)
                weak->handle_status(std::move(status), sequence);
        });
    };
    auto failure = [weak, sequence](std::string error) mutable {
        if (!weak)
            return;
        wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
            if (weak)
                weak->handle_error(error, sequence);
        });
    };
    if (image_mode)
        m_client.preprocess_image(new_request_id(), prompt, m_selected_image_path, m_job_palette, m_job_palette_roles,
                                  m_job_style, m_job_custom_style,
                                  m_job_print_settings,
                                  std::move(success), std::move(failure));
    else
        m_client.preprocess_text(new_request_id(), prompt, m_job_palette, m_job_palette_roles, m_job_style, m_job_custom_style,
                                 m_job_print_settings,
                                 std::move(success), std::move(failure));
}

void ModelGenerationPanel::on_generate(wxCommandEvent&)
{
    const bool image_mode = job_uses_image() || m_job_use_printable_colors;
    if (!m_awaiting_confirmation || m_job_id.empty() || !job_inputs_match() || (image_mode && !m_style_preview_ready))
        return;
    if (use_printable_colors() != m_job_use_printable_colors || current_palette() != m_job_palette) {
        MessageDialog changed(this, _L("颜色模式或耗材色板发生了变化，请先重新生成预览。"), wxEmptyString, wxOK | wxICON_INFORMATION);
        changed.ShowModal();
        return;
    }
    m_job_face_limit = current_face_limit();
    wxString message = image_mode
        ? _L("要根据这张 AI 风格预览生成 3D 模型吗？此操作可能消耗 API 额度。")
        : _L("要根据已确认的提示词生成 3D 模型吗？此操作可能消耗 API 额度。");
    message += wxString::Format(_L("\n\n目标精度：%d 万个三角面。"), m_job_face_limit / 10000);
    MessageDialog confirm(this, message, _L("确认生成 3D 模型"), wxYES_NO | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;
    m_busy = true;
    m_awaiting_confirmation = false;
    m_artifact_download_started = false;
    m_model_preview_ready = false;
    m_library_model_loaded = false;
    m_displayed_model_path.clear();
    m_displayed_model_job_id.clear();
    m_displayed_model_palette.clear();
    m_displayed_model_palette_roles.clear();
    clear_model_quality();
    if (m_model_preview != nullptr)
        m_model_preview->clear();
    const uint64_t sequence = m_sequence;
    update_progress(40, 3, _L("生成模型"));
    m_workflow_phase->SetLabel(_L("生成模型"));
    m_status->SetLabel(_L("正在提交 3D 生成请求..."));
    refresh_controls();
    const std::string prepared = image_mode ? std::string() : m_prepared_prompt->GetValue().ToUTF8().data();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.generate(m_job_id, prepared, m_job_palette, m_job_face_limit,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak) weak->handle_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::on_stop(wxCommandEvent&)
{
    if (m_job_id.empty())
        return;
    m_poll_timer.Stop();
    m_client.cancel_current();
    m_status->SetLabel(_L("正在停止本地任务；已提交的远端任务可能仍会继续。"));
    const uint64_t sequence = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.stop(m_job_id,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak) weak->handle_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::on_import(wxCommandEvent&)
{
    if (m_ready && !m_model_preview_ready && !m_artifact_download_started) {
        m_artifact_download_started = true;
        download_model_preview(m_sequence);
        return;
    }
    download_and_import();
}
void ModelGenerationPanel::on_discard(wxCommandEvent&) { reset(true); }
void ModelGenerationPanel::on_poll(wxTimerEvent&) { schedule_poll(); }

void ModelGenerationPanel::handle_error(const std::string& error, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    m_poll_timer.Stop();
    m_busy = false;
    m_awaiting_confirmation = false;
    m_awaiting_palette_confirmation = false;
    m_palette_recommendation_confirmed = false;
    m_ready = false;
    m_artifact_download_started = false;
    m_model_preview_ready = false;
    m_library_model_loaded = false;
    m_displayed_model_path.clear();
    m_displayed_model_palette.clear();
    m_displayed_model_palette_roles.clear();
    if (m_model_preview != nullptr)
        m_model_preview->clear();
    m_artifact_format.clear();
    m_artifact_color_encoding.clear();
    wxString message = wxString::FromUTF8(error);
    if (message.Contains("Could not connect to the preprocessing service"))
        message = _L("无法连接图片生成服务，请检查网络和服务地址后重试。");
    else if (message.Contains("preprocessing service is temporarily unavailable"))
        message = _L("图片生成服务暂时不可用，请稍后重试。");
    else if (message.Contains("preprocessing service is rate limiting"))
        message = _L("图片生成请求过于频繁，请稍后重试。");
    else if (message.Contains("preprocessing service rejected the request"))
        message = _L("图片生成服务拒绝了请求，请检查图片和提示词后重试。");
    else if (message.Contains("not reachable") || message.Contains("Couldn't connect") || message.Contains("Failed to connect") || message.Contains("Connection refused"))
        message = _L("无法连接本地 AI 服务，请确认正式服务已启动后重试。");
    else
        message = _L("操作失败：") + message;
    m_status->SetLabel(message);
    m_result_summary->SetLabel(_L("模型尚未生成完成。"));
    if (job_uses_image() && m_reference_image.IsOk() && !m_style_preview_ready) {
        m_style_preview_placeholder = _L("预览不可用");
        update_preview_view();
    }
    refresh_controls();
}

void ModelGenerationPanel::handle_status(AIModelGenerationClient::JobStatus status, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    m_job_id = status.id;
    if (status.palette_recommendation.available) {
        const bool new_recommendation = m_palette_recommendation_job_id != status.id;
        m_palette_recommendation = status.palette_recommendation;
        m_palette_recommendation_confirmed = status.palette_recommendation.confirmed;
        if (new_recommendation) {
            m_palette_recommendation_job_id = status.id;
            m_user_adjusted_palette_colors.clear();
            m_custom_palette.clear();
            m_palette_roles.clear();
            if (status.palette_recommendation.confirmed && !status.palette.empty()) {
                m_custom_palette = status.palette;
                m_palette_roles = status.palette_roles.empty() ? automatic_palette_roles(status.palette) : status.palette_roles;
                for (const std::string& color : status.palette) {
                    const auto recommended = std::find_if(
                        status.palette_recommendation.colors.begin(), status.palette_recommendation.colors.end(),
                        [&color](const AIModelGenerationClient::PaletteRecommendationColor& item) { return item.hex == color; });
                    if (recommended == status.palette_recommendation.colors.end())
                        m_user_adjusted_palette_colors.emplace_back(color);
                }
            } else {
                for (const auto& color : status.palette_recommendation.colors) {
                    m_custom_palette.emplace_back(color.hex);
                    m_palette_roles[color.role] = color.hex;
                }
            }
            m_palette_roles_source = m_custom_palette;
            if (m_palette_source != nullptr)
                m_palette_source->SetSelection(2);
            m_job_use_printable_colors = true;
        }
    }
    if (!status.palette.empty()) {
        m_job_palette = status.palette;
        m_job_use_printable_colors = true;
    }
    m_status->SetLabel(localized_job_status(status));
    m_busy = status.state == "recommending_palette" || status.state == "preprocessing" ||
             status.state == "queued" || status.state == "running" || status.state == "stopping";
    m_awaiting_palette_confirmation = status.state == "awaiting_palette_confirmation";
    m_awaiting_confirmation = status.state == "awaiting_confirmation";
    m_ready = status.state == "ready" && status.artifact_ready;
    m_artifact_format = status.artifact_format;
    m_artifact_color_encoding = status.artifact_color_encoding;
    m_raw_preview_available = status.raw_preview_ready;
    m_strict_preview_available = status.strict_preview_ready;
    m_heatmap_available = status.heatmap_ready;
    m_palette_quality_ok = status.palette_quality_ok;
    m_meaningful_palette_count = status.meaningful_palette_count;
    m_meaningful_subject_color_count = status.meaningful_subject_color_count;
    if (m_ready) {
        m_displayed_model_job_id = status.id;
        apply_model_quality(status.model_quality);
        apply_visual_quality(status.visual_quality);
    }
    if (!status.palette_roles.empty())
        m_job_palette_roles = status.palette_roles;
    if (!status.prepared_prompt.empty())
        m_prepared_prompt->SetValue(wxString::FromUTF8(status.prepared_prompt));
    if (status.preview_ready && m_preview_path.empty() && !m_restoring_input) {
        m_status->SetLabel(_L("正在加载 AI 风格预览..."));
        m_style_preview_placeholder = _L("正在加载 AI 处理图...");
        update_preview_view();
        download_preview(sequence);
    }
    if (m_ready) {
        wxString summary;
        summary << (m_model_preview_ready ? _L("3D 模型已可预览") : _L("模型已生成，正在准备 3D 预览"))
                << _L(" · ") << wxString::FromUTF8(m_artifact_format);
        if (status.artifact_size > 0)
            summary << wxString::Format(_L(" · %.1f MB"), double(status.artifact_size) / (1024.0 * 1024.0));
        m_result_summary->SetLabel(summary);
    } else if (m_awaiting_palette_confirmation) {
        m_result_summary->SetLabel(
            _L("AI 已推荐四个设计目标色。可以替换、删除或补充颜色；确认后再由你匹配实际耗材。"));
    } else if (m_awaiting_confirmation) {
        if (status.preview_ready && !status.palette.empty() && !status.palette_quality_ok) {
            const int required_colors = std::min<int>(status.palette.size(), 3);
            if (status.meaningful_subject_color_count < required_colors) {
                m_result_summary->SetLabel(wxString::Format(
                    _L("配色不足：主体只有 %d 种有效耗材色，至少需要 %d 种。请重新生成预览。"),
                    status.meaningful_subject_color_count, required_colors));
            } else if (status.printable_subject_area_ratio < 0.18) {
                m_result_summary->SetLabel(_L("主体占画面比例过小，请放大主体后重新生成预览。"));
            } else if (status.largest_subject_component_ratio < 0.90) {
                m_result_summary->SetLabel(_L("主体被背景分成多个不相连区域，请调整构图后重新生成预览。"));
            } else {
                m_result_summary->SetLabel(_L("预览未通过打印性检查，请调整构图或配色后重新生成。"));
            }
        } else if (status.preview_ready && !status.palette.empty()) {
            m_result_summary->SetLabel(wxString::Format(
                _L("严格耗材色板 · 最小特征 %d px · 清理后小区域 %.2f%% · 边界复杂度 %.3f"),
                status.minimum_feature_px, status.small_region_ratio * 100.0, status.boundary_complexity));
        } else {
            m_result_summary->SetLabel(job_uses_image()
                ? _L("AI 风格预览加载完成后即可生成 3D 模型。")
                : _L("请确认提示词后再开始生成 3D 模型。"));
        }
    } else {
        m_result_summary->SetLabel(localized_job_status(status));
    }
    update_workflow(&status);
    if (m_busy)
        m_poll_timer.StartOnce(1500);
    refresh_controls();
    if (m_ready && !m_artifact_download_started && !m_model_preview_ready) {
        m_artifact_download_started = true;
        download_model_preview(sequence);
    }
}

void ModelGenerationPanel::schedule_poll()
{
    if (m_shutdown || m_job_id.empty() || !m_busy)
        return;
    const uint64_t sequence = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.get_status(m_job_id,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak) weak->handle_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::download_preview(uint64_t sequence)
{
    m_preview_path = temp_path(m_job_id, "png");
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_preview(m_job_id, m_preview_path,
        [weak, sequence](boost::filesystem::path path) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, path = std::move(path)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                    return;
                wxImage image(path.wstring());
                if (!image.IsOk()) {
                    weak->m_preview_path.clear();
                    weak->m_style_preview_ready = false;
                    weak->m_style_preview_placeholder = _L("预览不可用");
                    weak->m_status->SetLabel(_L("无法显示 AI 风格预览，请重试。"));
                    weak->m_result_summary->SetLabel(_L("获得有效风格预览后才能继续生成 3D 模型。"));
                    weak->update_preview_view();
                    weak->refresh_controls();
                    return;
                }
                weak->m_clean_preview_image = image;
                weak->m_preview_zoom_factor = 1.0;
                weak->m_style_preview_ready = true;
                weak->m_style_preview_placeholder.clear();
                weak->m_preview_kind->SetLabel(_L("图片对照"));
                weak->apply_preview_stage();
                if (weak->m_reference_image.IsOk()) {
                    weak->m_preview_message->SetLabel(
                        wxString::Format(_L("参考图 %d × %d px  ·  AI 处理图 %d × %d px"),
                                         weak->m_reference_image.GetWidth(), weak->m_reference_image.GetHeight(),
                                         image.GetWidth(), image.GetHeight()));
                } else {
                    weak->m_preview_message->SetLabel(
                        wxString::Format(_L("AI 处理图 · %d × %d px"), image.GetWidth(), image.GetHeight()));
                }
                weak->m_status->SetLabel(_L("风格预览已生成，请确认后继续生成 3D 模型。"));
                weak->update_preview_view(true);
                weak->refresh_controls();
                weak->Layout();
                weak->download_auxiliary_previews(sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak && sequence == weak->m_sequence) {
                    weak->m_preview_path.clear();
                    weak->m_style_preview_ready = false;
                    weak->m_style_preview_placeholder = _L("预览不可用");
                    weak->m_status->SetLabel(_L("风格预览下载失败：") + wxString::FromUTF8(error));
                    weak->m_result_summary->SetLabel(_L("获得有效风格预览后才能继续生成 3D 模型。"));
                    weak->update_preview_view();
                    weak->refresh_controls();
                }
            });
        });
}

void ModelGenerationPanel::download_auxiliary_previews(uint64_t sequence, int stage)
{
    if (m_shutdown || sequence != m_sequence || m_job_id.empty() || stage >= 3)
        return;
    const bool available[] = {m_raw_preview_available, m_strict_preview_available, m_heatmap_available};
    const char* routes[] = {"raw-preview", "strict-preview", "heatmap"};
    const char* suffixes[] = {"raw", "strict", "heatmap"};
    if (!available[stage]) {
        download_auxiliary_previews(sequence, stage + 1);
        return;
    }
    const boost::filesystem::path path = temp_path(m_job_id + "-" + suffixes[stage], "png");
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_image_output(m_job_id, routes[stage], path,
        [weak, sequence, stage](boost::filesystem::path downloaded) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, stage, downloaded = std::move(downloaded)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence) return;
                wxImage image(downloaded.wstring());
                if (image.IsOk()) {
                    if (stage == 0) weak->m_raw_preview_image = image;
                    else if (stage == 1) weak->m_strict_preview_image = image;
                    else weak->m_heatmap_image = image;
                    weak->apply_preview_stage();
                }
                weak->download_auxiliary_previews(sequence, stage + 1);
            });
        },
        [weak, sequence, stage](std::string error) {
            if (!weak) return;
            BOOST_LOG_TRIVIAL(warning) << "Unable to download printable preview stage: " << error;
            wxGetApp().CallAfter([weak, sequence, stage]() {
                if (weak && !weak->m_shutdown && sequence == weak->m_sequence)
                    weak->download_auxiliary_previews(sequence, stage + 1);
            });
        });
}

void ModelGenerationPanel::download_model_preview(uint64_t sequence)
{
    if (!m_ready || m_job_id.empty() || m_shutdown)
        return;
    if (m_artifact_format != "obj") {
        m_artifact_download_started = false;
        m_status->SetLabel(_L("只能预览和导入生成的 OBJ 模型。"));
        m_result_summary->SetLabel(_L("当前生成结果不是受支持的 OBJ 格式。"));
        refresh_controls();
        return;
    }
    if (m_artifact_color_encoding != "vertex_colors") {
        m_artifact_download_started = false;
        m_status->SetLabel(_L("生成的 OBJ 不包含受支持的顶点颜色。"));
        m_result_summary->SetLabel(_L("缺少颜色信息，无法继续彩色模型流程。"));
        refresh_controls();
        return;
    }

    m_artifact_path = temp_path(m_job_id, m_artifact_format);
    m_busy = true;
    update_progress(94, 4, _L("下载模型"));
    m_status->SetLabel(_L("正在下载并校验生成的 OBJ 模型..."));
    m_model_stats->SetLabel(_L("正在加载模型..."));
    m_model_preview_message->SetLabel(_L("下载完成后将在此处显示彩色 3D 预览。"));
    refresh_controls();

    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_artifact(m_job_id, m_artifact_format, m_artifact_path,
        [weak, sequence](boost::filesystem::path path) mutable {
            if (!weak)
                return;
            wxGetApp().CallAfter([weak, sequence, path = std::move(path)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                    return;
                size_t triangle_count = 0;
                size_t color_count = 0;
                Vec3d dimensions = Vec3d::Zero();
                std::string error;
                if (weak->m_model_preview == nullptr ||
                    !weak->m_model_preview->load_model(path, weak->m_job_palette, triangle_count, dimensions, color_count, error)) {
                    weak->m_busy = false;
                    weak->m_artifact_download_started = false;
                    weak->m_model_preview_ready = false;
                    weak->m_status->SetLabel(_L("OBJ 模型解析失败，已保留本地文件。"));
                    weak->m_result_summary->SetLabel(_L("无法显示 3D 预览：") + from_u8(error));
                    weak->m_model_stats->SetLabel(_L("模型预览不可用"));
                    weak->m_model_preview_message->SetLabel(_L("请重试下载，或检查 generated_models/downloads 中的 OBJ 文件。"));
                    weak->refresh_controls();
                    return;
                }

                weak->m_artifact_path = path;
                weak->m_displayed_model_path = path;
                weak->m_displayed_model_job_id = weak->m_job_id;
                weak->m_displayed_model_palette = weak->m_job_palette;
                weak->m_displayed_model_palette_roles = weak->m_job_palette_roles;
                weak->m_busy = false;
                weak->m_model_preview_ready = true;
                weak->m_library_model_loaded = false;
                weak->update_progress(95, 4, _L("检查 3D 模型"));
                weak->m_status->SetLabel(_L("3D 模型已生成，请确认外观后再生成 G-code。"));
                weak->m_model_stats->SetLabel(wxString::Format(
                    _L("%llu 个三角面 · %llu 种颜色\n%.1f × %.1f × %.1f mm"),
                    static_cast<unsigned long long>(triangle_count), static_cast<unsigned long long>(color_count),
                    dimensions.x(), dimensions.y(), dimensions.z()));
                weak->m_model_preview_message->SetLabel(_L("拖动可旋转，滚轮可缩放；确认无误后点击“确认并生成 G-code”。"));
                weak->m_result_summary->SetLabel(_L("模型已下载并通过 OBJ 解析，可继续导入和切片。"));
                const size_t artifact_size = boost::filesystem::file_size(path);
                weak->save_library_entry(artifact_size, triangle_count, dimensions.x(), dimensions.y(),
                                         dimensions.z(), color_count);
                if (weak->m_preview_book != nullptr)
                    weak->m_preview_book->SetSelection(1);
                wxGetApp().CallAfter([weak]() {
                    if (weak && weak->m_model_preview != nullptr)
                        weak->m_model_preview->refresh();
                });
                weak->refresh_controls();
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak)
                return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                    return;
                weak->m_busy = false;
                weak->m_artifact_download_started = false;
                weak->m_model_preview_ready = false;
                weak->m_status->SetLabel(_L("模型下载失败，请重试。"));
                weak->m_result_summary->SetLabel(_L("下载错误：") + from_u8(error));
                weak->m_model_stats->SetLabel(_L("模型尚未下载"));
                weak->refresh_controls();
            });
        });
}

void ModelGenerationPanel::download_and_import()
{
    if (!m_ready || !m_model_preview_ready || m_busy)
        return;
    if (m_artifact_format != "obj") {
        m_status->SetLabel(_L("只能导入生成的 OBJ 模型。"));
        return;
    }
    if (m_artifact_color_encoding != "vertex_colors") {
        m_status->SetLabel(_L("生成的 OBJ 不包含受支持的顶点颜色。"));
        return;
    }
    boost::filesystem::path local_path;
    if (is_nonempty_obj(m_artifact_path))
        local_path = m_artifact_path;
    else if (!m_job_id.empty()) {
        const boost::filesystem::path downloaded_path = temp_path(m_job_id, m_artifact_format);
        if (is_nonempty_obj(downloaded_path))
            local_path = downloaded_path;
        else
            m_artifact_path = downloaded_path;
    }

    if (local_path.empty() && m_job_id.empty()) {
        m_status->SetLabel(_L("本地 OBJ 模型已不存在。"));
        m_result_summary->SetLabel(_L("请从模型库重新加载有效模型后再生成 G-code。"));
        refresh_controls();
        return;
    }

    m_busy = true;
    update_progress(96, 5, _L("导入并生成 G-code"));
    m_workflow_phase->SetLabel(_L("导入并生成 G-code"));
    const uint64_t sequence = m_sequence;
    m_status->SetLabel(local_path.empty() ? _L("正在从本地服务读取生成的模型...")
                                         : _L("正在读取本地 OBJ 模型..."));
    refresh_controls();

    if (!local_path.empty()) {
        import_local_artifact(local_path, sequence);
        return;
    }

    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_artifact(m_job_id, m_artifact_format, m_artifact_path,
        [weak, sequence](boost::filesystem::path path) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, path = std::move(path)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                    return;
                weak->import_local_artifact(path, sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (!weak) return;
                weak->cleanup_files();
                weak->handle_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::import_local_artifact(const boost::filesystem::path& path, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    if (!is_nonempty_obj(path)) {
        m_busy = false;
        m_status->SetLabel(_L("本地 OBJ 模型无效或已不存在。"));
        m_result_summary->SetLabel(_L("请从模型库重新加载有效模型后再生成 G-code。"));
        refresh_controls();
        return;
    }

    m_artifact_path = path;
    const bool auto_slice_after_import = m_auto_slice_after_import == nullptr ||
                                         m_auto_slice_after_import->GetValue();
    update_progress(98, 5, _L("导入模型"));
    m_status->SetLabel(auto_slice_after_import
                           ? _L("正在导入颜色、自动摆放模型并准备 G-code...")
                           : _L("正在导入颜色、检查模型并自动摆放..."));
    refresh_controls();

    const int color_selection = m_import_color_mode != nullptr ? m_import_color_mode->GetSelection() : 0;
    AI::ModelImportRequest request;
    request.artifact.local_path = path;
    request.artifact.job_id = m_job_id;
    request.artifact.format = m_artifact_format;
    request.artifact.color_encoding = m_artifact_color_encoding;
    request.artifact.generation_palette =
        !m_displayed_model_palette.empty() ? m_displayed_model_palette : m_job_palette;
    request.artifact.used_printable_colors = m_job_use_printable_colors;
    request.color_mode = color_selection == 2
        ? AI::ImportColorMode::SingleColor
        : color_selection == 1 ? AI::ImportColorMode::AutoMap : AI::ImportColorMode::ManualMatch;
    request.auto_slice_after_import = auto_slice_after_import;

    const AI::ModelImportResult result = m_artifact_consumer.import_artifact(request);
    if (!result.imported()) {
        m_busy = false;
        if (result.outcome == AI::ModelImportOutcome::InvalidArtifact) {
            m_status->SetLabel(_L("本地 OBJ 模型无效或已不存在。"));
            m_result_summary->SetLabel(_L("请从模型库重新加载有效模型后再生成 G-code。"));
        } else if (result.outcome == AI::ModelImportOutcome::RepairFailed) {
            m_status->SetLabel(_L("自动网格修复失败，未导入也未开始切片。"));
            m_result_summary->SetLabel(
                _L("原始 OBJ 和修复诊断已保留在 generated_models。") + from_u8(result.error));
        } else {
            m_status->SetLabel(_L("无法导入生成的模型。"));
            m_result_summary->SetLabel(_L("OBJ 已保留在本地，请调整耗材配置后重试。"));
        }
        refresh_controls();
        return;
    }

    const std::string job_id = m_job_id;
    cleanup_files();
    if (!job_id.empty())
        m_client.remove(job_id, [] {}, [](std::string) {});
    m_poll_timer.Stop();
    m_job_id.clear();
    m_job_palette.clear();
    m_job_palette_roles.clear();
    m_job_use_printable_colors = false;
    m_job_prompt.clear();
    m_job_style.clear();
    m_job_custom_style.clear();
    m_job_image_path.clear();
    m_artifact_format.clear();
    m_artifact_color_encoding.clear();
    m_busy = false;
    m_awaiting_confirmation = false;
    m_awaiting_palette_confirmation = false;
    m_palette_recommendation_confirmed = false;
    m_ready = false;
    m_artifact_download_started = false;
    m_library_model_loaded = false;

    m_workflow_phase->SetLabel(result.slice_after_import
                                   ? _L("生成 G-code")
                                   : result.manual_repair_required
                                       ? _L("手动修复")
                                       : result.manual_coloring_required ? _L("手动上色") : _L("等待手动切片"));
    update_progress(100, 5, result.slice_after_import ? _L("生成 G-code") : _L("已导入准备页"));
    m_prepared_prompt->Clear();

    if (!result.error.empty()) {
        m_status->SetLabel(_L("模型已导入，但无法继续自动流程。"));
        m_result_summary->SetLabel(from_u8(result.error));
    } else if (result.slice_after_import) {
        if (result.color_mode == AI::ImportColorMode::SingleColor) {
            m_status->SetLabel(_L("模型已按单色导入，正在切片当前打印板..."));
            m_result_summary->SetLabel(_L("模型已忽略原有颜色，并按当前耗材进入 G-code 预览。"));
        } else if (result.color_mode == AI::ImportColorMode::AutoMap) {
            m_status->SetLabel(_L("模型已自动匹配耗材颜色，正在切片当前打印板..."));
            m_result_summary->SetLabel(_L("可打印模型已自动摆放，并已进入 G-code 预览。"));
        } else {
            m_status->SetLabel(_L("颜色匹配已确认，正在切片当前打印板..."));
            m_result_summary->SetLabel(_L("模型颜色已匹配到当前打印机耗材槽，并进入 G-code 预览。"));
        }
    } else if (result.manual_repair_required) {
        m_status->SetLabel(_L("模型已导入准备页，请手动修复后再切片。"));
        m_result_summary->SetLabel(_L("未自动生成 G-code；原始 OBJ 和修复诊断仍保留在 generated_models。"));
    } else if (result.manual_coloring_required) {
        if (result.color_mapping_collapsed) {
            m_status->SetLabel(_L("多种模型颜色只匹配到一个耗材槽，模型已导入准备页。"));
            m_result_summary->SetLabel(_L("请重新匹配至少两个耗材槽，或在准备页手动上色后再切片。"));
        } else if (result.color_mode == AI::ImportColorMode::ManualMatch) {
            m_status->SetLabel(_L("颜色匹配未完成，模型已导入准备页。"));
            m_result_summary->SetLabel(_L("未自动生成 G-code；请完成颜色匹配或手动上色后再切片。"));
        } else {
            m_status->SetLabel(_L("自动匹配当前耗材失败，模型已导入准备页。"));
            m_result_summary->SetLabel(_L("未自动生成 G-code；请改用手动匹配或在准备页手动上色。"));
        }
    } else {
        m_status->SetLabel(_L("模型已导入并自动摆放，正在进入准备页。"));
        m_result_summary->SetLabel(_L("已按你的选择跳过自动切片，可调整模型或颜色后手动切片。"));
    }
    refresh_controls();
}

void ModelGenerationPanel::update_adaptive_text_height(wxTextCtrl* control, int minimum_lines, int maximum_lines)
{
    if (control == nullptr || minimum_lines <= 0 || maximum_lines < minimum_lines)
        return;

    wxClientDC dc(control);
    dc.SetFont(control->GetFont());
    const int line_height = std::max(1, dc.GetTextExtent("Ag").GetHeight() + FromDIP(3));
    int available_width = control->GetClientSize().GetWidth() - FromDIP(18);
    if (available_width < FromDIP(120))
        available_width = FromDIP(280);

    const wxString value = control->GetValue();
    int visual_lines = 0;
    size_t start = 0;
    while (start <= value.length()) {
        const size_t end = value.find('\n', start);
        const wxString line = end == wxString::npos ? value.Mid(start) : value.Mid(start, end - start);
        const int line_width = dc.GetTextExtent(line.empty() ? " " : line).GetWidth();
        visual_lines += std::max(1, (line_width + available_width - 1) / available_width);
        if (end == wxString::npos)
            break;
        start = end + 1;
    }

    const int rows = std::clamp(visual_lines, minimum_lines, maximum_lines);
    const int desired_height = rows * line_height + FromDIP(8);
    if (control->GetMinSize().GetHeight() == desired_height &&
        control->GetMaxSize().GetHeight() == desired_height)
        return;
    control->SetMinSize(wxSize(-1, desired_height));
    control->SetMaxSize(wxSize(-1, desired_height));
    control->InvalidateBestSize();
}

void ModelGenerationPanel::refresh_controls()
{
    if (m_shutdown)
        return;
    update_adaptive_text_height(m_prompt, 2, 6);
    update_adaptive_text_height(m_custom_style, 2, 5);
    refresh_palette();
    m_status->Wrap(FromDIP(310));
    const bool image_input = has_image_input();
    const bool image_job = job_uses_image();
    const bool custom_style_selected = current_style() == "custom";
    const bool custom_style_ready = !custom_style_selected || !current_custom_style().empty();
    const bool valid_input = (!m_prompt->GetValue().empty() || image_input) && custom_style_ready;
    const bool printable_colors = use_printable_colors();
    const bool ai_palette_source = printable_colors && m_palette_source->GetSelection() == 2;
    const bool palette_matches = m_awaiting_palette_confirmation || m_job_id.empty() ||
        (printable_colors == m_job_use_printable_colors && (!printable_colors || m_palette == m_job_palette));
    const bool stale_job = !m_job_id.empty() && (!job_inputs_match() || !palette_matches);
    const bool auto_slice_after_import = m_auto_slice_after_import == nullptr ||
                                         m_auto_slice_after_import->GetValue();
    const bool show_review = m_awaiting_confirmation && !image_job && !stale_job;
    const bool local_artifact = is_nonempty_obj(m_artifact_path) ||
        (!m_job_id.empty() && is_nonempty_obj(temp_path(m_job_id, "obj")));

    m_preprocess->SetLabel(ai_palette_source && m_palette_recommendation_confirmed
                               ? _L("使用当前目标色生成预览")
                               : m_awaiting_confirmation && !m_palette_quality_ok
                               ? _L("重新生成设计师玩具预览")
                               : image_input && printable_colors ? _L("生成设计师玩具图片")
                               : image_input ? _L("生成风格图片预览")
                               : printable_colors ? _L("生成图片预览") : _L("准备 3D 提示词"));
    m_generate->SetLabel((image_job || m_job_use_printable_colors)
                             ? _L("确认图片并生成 3D") : _L("确认提示词并生成 3D"));
    m_import->SetLabel(!m_model_preview_ready
                           ? _L("重新加载 3D 模型")
                           : auto_slice_after_import
                               ? _L("导入并自动切片")
                               : _L("导入到准备页"));
    if (m_library_model_loaded) {
        m_model_preview_message->SetLabel(auto_slice_after_import
                                              ? _L("已从模型库加载。确认外观后点击“导入并切片”。")
                                              : _L("已从模型库加载。确认外观后点击“导入到准备页”。"));
        m_result_summary->SetLabel(auto_slice_after_import
                                       ? _L("历史模型已加载到当前 3D 预览，可继续导入准备页并切片。")
                                       : _L("历史模型已加载到当前 3D 预览，将导入准备页等待手动切片。"));
    }
    m_discard->SetLabel(_L("重新开始"));
    m_clear_image->Show(image_input);
    m_upload_notice->Show(image_input);
    const bool show_advanced = printable_colors && !m_busy && !m_ready;
    m_advanced_toggle->Show(show_advanced);
    m_advanced_options->Show(show_advanced && m_advanced_options_expanded);
    m_advanced_toggle->SetLabel(m_advanced_options_expanded ? _L("收起高级设置") : _L("显示高级设置"));
    m_model_settings_panel->Show(m_awaiting_confirmation && !stale_job);
    m_import_settings_panel->Show(m_ready && !stale_job);
    m_preprocess_section->Show(show_review);
    m_prepared_prompt_label->Show(show_review);
    m_prepared_prompt->Show(show_review);

    m_prompt->Enable(!m_busy);
    m_style->Enable(!m_busy);
    m_custom_style_panel->Show(custom_style_selected);
    m_custom_style->Enable(!m_busy && custom_style_selected);
    m_quality->Enable(!m_busy);
    m_choose_image->Enable(!m_busy);
    m_clear_image->Enable(!m_busy);
    m_use_printable_colors->Enable(!m_busy);
    m_palette_source->Enable(!m_busy && printable_colors);
    m_import_color_mode->Enable(!m_busy);
    m_auto_slice_after_import->Enable(!m_busy);
    m_custom_color->Enable(!m_busy && printable_colors && m_palette_source->GetSelection() != 0);
    m_add_custom_color->Enable(!m_busy && printable_colors && m_palette_source->GetSelection() != 0 && m_custom_palette.size() < 4);
    for (wxSpinCtrlDouble* control : {m_print_width, m_nozzle_size, m_line_width, m_minimum_feature})
        control->Enable(!m_busy && printable_colors);
    if (m_shadow_color != nullptr)
        m_shadow_color->Enable(!m_busy && printable_colors);
    m_preprocess->Enable(m_service_available && !m_busy && valid_input && (!printable_colors || !m_palette.empty()));
    m_prepared_prompt->Enable(m_service_available && !m_busy && show_review);
    m_generate->Enable(m_service_available && !m_busy && m_awaiting_confirmation && !stale_job &&
                       m_palette_quality_ok &&
                       (!(image_job || m_job_use_printable_colors) || m_style_preview_ready));
    m_stop->Enable(m_service_available && m_busy && !m_job_id.empty());
    const bool quality_rejected = m_model_quality.available && m_model_quality.status == "reject";
    m_import->Enable((local_artifact || m_service_available) && !m_busy && !m_quality_check_busy && !m_visual_check_busy &&
                     m_ready && !stale_job && !quality_rejected &&
                     (m_model_preview_ready || !m_artifact_download_started));
    m_recheck_model->Enable(m_service_available && !m_busy && !m_quality_check_busy && !m_visual_check_busy &&
                            m_model_preview_ready && !m_displayed_model_job_id.empty());
    m_visual_review_model->Enable(m_service_available && !m_busy && !m_quality_check_busy && !m_visual_check_busy &&
                                  m_model_preview_ready && !m_displayed_model_job_id.empty());
    m_discard->Enable(!m_busy && (!m_job_id.empty() || m_ready));

    const bool show_preprocess = !m_busy && (!m_ready || stale_job) &&
        (!ai_palette_source || m_palette_recommendation_confirmed) &&
        (m_job_id.empty() || stale_job || (!m_awaiting_confirmation && !m_ready) ||
         (m_awaiting_confirmation && !m_palette_quality_ok));
    m_preprocess->Show(show_preprocess);
    m_generate->Show(!m_busy && m_awaiting_confirmation && !stale_job && m_palette_quality_ok);
    m_stop->Show(m_busy);
    m_import->Show(!m_busy && m_ready && !stale_job);
    m_discard->Show(!m_busy && (!m_job_id.empty() || m_ready));
    if (!m_busy && ((m_job_id.empty() && !m_ready) || stale_job))
        update_progress(0, 1, _L("输入"));
    if (!m_busy && stale_job)
        m_status->SetLabel(m_awaiting_palette_confirmation
                               ? _L("输入已变化；可重新推荐，或继续使用当前配色。")
                               : _L("输入或颜色已变化，请重新生成图片预览。"));
    if (m_ready && !stale_job)
        m_workflow_steps->SetLabel(_L("检查右侧 3D 模型，然后选择导入方式"));
    else if (m_awaiting_palette_confirmation)
        m_workflow_steps->SetLabel(stale_job
                                       ? _L("输入已变化：重新推荐或确认继续使用当前配色")
                                       : _L("修改或确认 AI 推荐的四个设计目标色"));
    else if (m_awaiting_confirmation && !stale_job)
        m_workflow_steps->SetLabel(m_palette_quality_ok
                                       ? _L("确认右侧图片效果，并选择 3D 模型精度")
                                       : _L("当前配色未通过检查，请重新生成图片"));
    else if (!m_busy)
        m_workflow_steps->SetLabel(custom_style_selected && !custom_style_ready
                                       ? _L("请补充自定义风格描述")
                                       : valid_input ? _L("下一步：生成图片预览")
                                                     : _L("输入文字、图片，或同时使用两者"));
    if (stale_job && (m_awaiting_confirmation || m_ready))
        m_result_summary->SetLabel(_L("输入内容或颜色模式发生变化，请重新生成预览后继续。"));

    const bool has_preview = m_reference_image.IsOk() || m_style_preview_image.IsOk();
    m_zoom_out->Enable(has_preview && m_preview_zoom_factor > MIN_PREVIEW_ZOOM);
    m_zoom_fit->Enable(has_preview && std::abs(m_preview_zoom_factor - 1.0) > 0.001);
    m_zoom_in->Enable(has_preview && m_preview_zoom_factor < MAX_PREVIEW_ZOOM);
    m_reset_model_view->Enable(m_model_preview_ready);
    refresh_local_recolor_controls();
    if (auto* scroll = dynamic_cast<wxScrolledWindow*>(m_prompt->GetParent())) {
        scroll->Layout();
        scroll->FitInside();
    }
    Layout();
}

void ModelGenerationPanel::apply_model_quality(const AIModelGenerationClient::ModelQuality& quality)
{
    m_model_quality = quality;
    refresh_model_quality_card();
}

void ModelGenerationPanel::apply_visual_quality(const AIModelGenerationClient::VisualQuality& quality)
{
    m_visual_quality = quality;
    refresh_model_quality_card();
}

void ModelGenerationPanel::clear_model_quality()
{
    m_model_quality = {};
    m_visual_quality = {};
    m_quality_check_busy = false;
    m_visual_check_busy = false;
    refresh_model_quality_card();
}

void ModelGenerationPanel::refresh_model_quality_card()
{
    if (m_model_quality_panel == nullptr)
        return;
    wxColour foreground(91, 104, 107);
    wxColour background(246, 248, 248);
    wxString status = _L("尚未检查");
    wxString summary = m_model_preview_ready
        ? _L("此历史模型还没有结构质量报告，可点击“重新检查”。")
        : _L("模型生成或加载后可进行结构检查。");
    if (m_quality_check_busy) {
        status = _L("正在检查...");
        summary = _L("正在本地分析拓扑、组件、接地和悬垂，请稍候。");
        foreground = wxColour(31, 122, 116);
        background = wxColour(229, 244, 242);
    } else if (m_model_quality.available) {
        if (m_model_quality.status == "pass") {
            status = _L("结构检查通过");
            summary = _L("未发现需要阻断导入的结构问题，可继续检查外观和颜色。");
            foreground = wxColour(31, 122, 90);
            background = wxColour(232, 246, 238);
        } else if (m_model_quality.status == "review") {
            status = _L("建议复核");
            foreground = wxColour(174, 112, 22);
            background = wxColour(255, 246, 225);
        } else if (m_model_quality.status == "reject") {
            status = _L("未通过结构检查");
            foreground = wxColour(188, 62, 54);
            background = wxColour(253, 235, 233);
        }
        const auto& codes = m_model_quality.status == "reject" ? m_model_quality.errors : m_model_quality.warnings;
        if (!codes.empty()) {
            summary.clear();
            const size_t visible = std::min<size_t>(2, codes.size());
            for (size_t index = 0; index < visible; ++index) {
                if (!summary.empty()) summary += "\n";
                summary += _L("• ") + model_quality_code_label(codes[index]);
            }
            if (codes.size() > visible)
                summary += wxString::Format(_L("\n另有 %llu 项，请展开查看。"),
                    static_cast<unsigned long long>(codes.size() - visible));
        }
    }
    m_model_quality_status->SetLabel(status);
    m_model_quality_status->SetForegroundColour(foreground);
    m_model_quality_panel->SetBackgroundColour(background);
    m_model_quality_summary->SetLabel(summary);
    wxString details;
    if (m_model_quality.available) {
        details << wxString::Format(_L("三角面：%llu · 顶点：%llu · 连通部件：%llu\n"),
                    static_cast<unsigned long long>(m_model_quality.face_count),
                    static_cast<unsigned long long>(m_model_quality.vertex_count),
                    static_cast<unsigned long long>(m_model_quality.component_count));
        if (m_model_quality.bed_contact_area_available) {
            details << wxString::Format(_L("最大部件占比：%.1f%% · 接地跨度：%.1f%% · 接地面积：%.1f%%\n"),
                        m_model_quality.largest_component_face_ratio * 100.0,
                        m_model_quality.contact_span_ratio * 100.0,
                        m_model_quality.bed_contact_area_ratio * 100.0);
            details << wxString::Format(
                        m_model_quality.elevated_downward_surface_ratio_available
                            ? _L("离床向下表面：%.1f%%") : _L("向下表面：%.1f%%"),
                        (m_model_quality.elevated_downward_surface_ratio_available
                            ? m_model_quality.elevated_downward_surface_ratio
                            : m_model_quality.downward_surface_ratio) * 100.0);
            if (m_model_quality.overhang_region_metrics_available)
                details << wxString::Format(_L(" · 显著局部悬垂：%llu 个"),
                            static_cast<unsigned long long>(m_model_quality.significant_overhang_region_count));
            if (m_model_quality.component_thickness_available &&
                m_model_quality.minimum_component_thickness_mm > 0.0)
                details << wxString::Format(_L("\n最薄组件：%.2f mm · 薄型组件：%llu 个"),
                            m_model_quality.minimum_component_thickness_mm,
                            static_cast<unsigned long long>(m_model_quality.thin_component_count));
        } else {
            details << wxString::Format(_L("最大部件占比：%.1f%% · 接地覆盖：%.1f%% · 向下表面：%.1f%%"),
                        m_model_quality.largest_component_face_ratio * 100.0,
                        m_model_quality.contact_span_ratio * 100.0,
                        m_model_quality.downward_surface_ratio * 100.0);
        }
        const auto& codes = m_model_quality.status == "reject" ? m_model_quality.errors : m_model_quality.warnings;
        for (const std::string& code : codes)
            details += _L("\n• ") + model_quality_code_label(code);
    } else {
        details = _L("尚无结构化质量指标。");
    }
    m_model_quality_details->SetLabel(details);
    m_model_quality_details_pane->Show(m_model_quality.available);

    wxString visual_status = _L("AI 视觉复核：未运行");
    wxString visual_summary = m_model_preview_ready
        ? _L("点击“AI 视觉复核”后，将生成五视图并检查外观；会调用付费 AI，但不会阻断导入。")
        : _L("模型准备好后可按需生成五视图并进行 AI 外观复核。");
    wxColour visual_foreground(91, 104, 107);
    if (m_visual_check_busy) {
        visual_status = _L("AI 视觉复核中...");
        visual_summary = _L("正在本地生成前后左右和等轴视图，然后检查主体、底座、轮廓和色块，请稍候。");
        visual_foreground = wxColour(31, 122, 116);
    } else if (m_visual_quality.available) {
        if (m_visual_quality.status == "pass") {
            visual_status = wxString::Format(_L("AI 视觉复核通过 · %d 分"), m_visual_quality.score);
            visual_foreground = wxColour(31, 122, 90);
        } else if (m_visual_quality.status == "review") {
            visual_status = wxString::Format(_L("AI 建议人工复核 · %d 分"), m_visual_quality.score);
            visual_foreground = wxColour(174, 112, 22);
        } else {
            visual_status = _L("AI 视觉复核暂不可用");
            visual_foreground = wxColour(174, 112, 22);
        }
        visual_summary = from_u8(m_visual_quality.summary);
        const auto& codes = m_visual_quality.status == "unavailable"
            ? m_visual_quality.errors : m_visual_quality.warnings;
        const size_t visible = std::min<size_t>(2, codes.size());
        for (size_t index = 0; index < visible; ++index)
            visual_summary += _L("\n• ") + visual_quality_code_label(codes[index]);
    }
    m_visual_quality_status->SetLabel(visual_status);
    m_visual_quality_status->SetForegroundColour(visual_foreground);
    m_visual_quality_summary->SetLabel(visual_summary);
    m_model_quality_panel->Layout();
    m_model_quality_panel->GetParent()->Layout();
}

void ModelGenerationPanel::on_recheck_model(wxCommandEvent&)
{
    if (m_quality_check_busy || m_displayed_model_job_id.empty() || !m_model_preview_ready)
        return;
    const std::string job_id = m_displayed_model_job_id;
    const uint64_t sequence = m_sequence;
    m_quality_check_busy = true;
    m_status->SetLabel(_L("正在重新检查当前 3D 模型..."));
    refresh_model_quality_card();
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.recheck(job_id,
        [weak, sequence, job_id](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                weak->m_quality_check_busy = false;
                weak->apply_model_quality(status.model_quality);
                weak->apply_visual_quality(status.visual_quality);
                weak->m_status->SetLabel(status.model_quality.status == "pass"
                    ? _L("结构检查通过。") : status.model_quality.status == "review"
                    ? _L("结构检查完成，建议复核提示项。") : _L("结构检查未通过，已禁用导入。"));
                weak->refresh_controls();
            });
        },
        [weak, sequence, job_id](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, error = std::move(error)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                weak->m_quality_check_busy = false;
                weak->m_status->SetLabel(_L("无法重新检查历史模型：") + from_u8(error));
                weak->refresh_model_quality_card();
                weak->refresh_controls();
            });
        });
}

void ModelGenerationPanel::on_visual_review_model(wxCommandEvent&)
{
    if (m_visual_check_busy || m_quality_check_busy || m_displayed_model_job_id.empty() || !m_model_preview_ready)
        return;
    const std::string job_id = m_displayed_model_job_id;
    const uint64_t sequence = m_sequence;
    m_visual_check_busy = true;
    m_status->SetLabel(_L("正在生成多视角并进行 AI 外观复核..."));
    refresh_model_quality_card();
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.visual_review(job_id,
        [weak, sequence, job_id](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                weak->m_visual_check_busy = false;
                weak->apply_visual_quality(status.visual_quality);
                weak->m_status->SetLabel(status.visual_quality.status == "pass"
                    ? _L("AI 视觉复核完成，未发现明显外观风险。")
                    : status.visual_quality.status == "review"
                    ? _L("AI 视觉复核完成，建议人工确认提示项。")
                    : _L("AI 视觉复核暂不可用，可稍后重试。"));
                weak->refresh_controls();
            });
        },
        [weak, sequence, job_id](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, error = std::move(error)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                weak->m_visual_check_busy = false;
                weak->m_status->SetLabel(_L("无法完成 AI 视觉复核：") + from_u8(error));
                weak->refresh_model_quality_card();
                weak->refresh_controls();
            });
        });
}

std::vector<std::string> ModelGenerationPanel::local_recolor_palette() const
{
    std::vector<std::string> palette = project_palette();
    if (palette.empty())
        palette = !m_displayed_model_palette.empty() ? m_displayed_model_palette : m_job_palette;
    if (palette.size() > 4)
        palette.resize(4);
    return palette;
}

void ModelGenerationPanel::refresh_local_recolor_controls()
{
    if (m_local_recolor_panel == nullptr || m_local_recolor_toggle == nullptr ||
        m_local_recolor_controls == nullptr)
        return;
    const bool ready = m_model_preview_ready && m_model_preview != nullptr &&
                       m_model_preview->region_editing_ready();
    if (!ready)
        m_local_recolor_toggle->SetValue(false);
    const bool editing = ready && m_local_recolor_toggle->GetValue();
    m_local_recolor_panel->Show(ready);
    m_local_recolor_controls->Show(editing);
    m_local_recolor_toggle->SetLabel(editing ? _L("收起改色工具") : _L("编辑局部颜色"));
    m_local_recolor_toggle->Enable(ready && !m_busy);
    if (m_locate_overhang_regions != nullptr)
        m_locate_overhang_regions->Enable(ready && !m_busy);
    if (m_model_preview != nullptr)
        m_model_preview->set_selection_enabled(editing);

    const std::vector<std::string> palette = local_recolor_palette();
    if (palette != m_region_palette) {
        m_region_palette = palette;
        m_region_color_index = palette.empty()
            ? 0 : std::clamp(m_region_color_index, 0, int(palette.size()) - 1);
    }
    const bool has_selection = m_model_preview != nullptr && m_model_preview->selected_face_count() > 0;
    const bool can_undo = m_model_preview != nullptr && m_model_preview->can_undo_selection();
    m_region_selection_summary->SetLabel(has_selection
        ? wxString::Format(_L("已选择区域 · %llu 个三角面"),
                           static_cast<unsigned long long>(m_model_preview->selected_face_count()))
        : _L("点击模型选择要改色的部位"));

    std::vector<std::string> model_palette = m_displayed_model_palette;
    if (model_palette.size() > m_region_material_buttons.size())
        model_palette.resize(m_region_material_buttons.size());
    AIModelGenerationClient::PaletteRoles model_roles = m_displayed_model_palette_roles;
    if (model_roles.empty())
        model_roles = automatic_palette_roles(model_palette);
    for (size_t index = 0; index < m_region_material_buttons.size(); ++index) {
        wxButton* button = m_region_material_buttons[index];
        const bool visible = index < model_palette.size();
        button->Show(visible);
        if (!visible)
            continue;
        std::string role;
        for (const char* candidate : PALETTE_ROLE_IDS) {
            const auto found = model_roles.find(candidate);
            if (found != model_roles.end() && same_palette_color(found->second, model_palette[index])) {
                role = candidate;
                break;
            }
        }
        const wxString label = palette_role_label(role);
        button->SetLabel((label.empty()
            ? wxString::Format(_L("材料 %llu\n"), static_cast<unsigned long long>(index + 1))
            : label + "\n") + from_u8(model_palette[index]));
        button->SetToolTip((label.empty() ? _L("选择模型中属于此颜色的全部材料面：")
                                          : _L("选择模型中属于此语义角色的全部材料面：")) +
                           from_u8(model_palette[index]));
        const wxColour color(from_u8(model_palette[index]));
        if (color.IsOk()) {
            button->SetBackgroundColour(color);
            const double luminance = 0.299 * color.Red() + 0.587 * color.Green() + 0.114 * color.Blue();
            button->SetForegroundColour(luminance >= 150.0 ? *wxBLACK : *wxWHITE);
        }
        button->Enable(editing && !m_busy);
    }

    for (size_t index = 0; index < m_region_operation_buttons.size(); ++index) {
        wxToggleButton* button = m_region_operation_buttons[index];
        const bool selected = int(index) == m_region_operation_index;
        button->SetValue(selected);
        button->SetBackgroundColour(selected ? wxColour(221, 242, 240) : wxColour(248, 249, 249));
        button->SetForegroundColour(selected ? wxColour(0, 114, 110) : wxColour(37, 48, 50));
        button->Enable(editing && !m_busy);
    }
    m_region_range->Enable(editing && !m_busy);

    for (size_t index = 0; index < m_region_color_buttons.size(); ++index) {
        wxToggleButton* button = m_region_color_buttons[index];
        const bool visible = index < palette.size();
        button->Show(visible);
        if (!visible)
            continue;
        const wxColour color(from_u8(palette[index]));
        const bool selected = int(index) == m_region_color_index;
        button->SetValue(selected);
        button->SetLabel(wxString::Format(
            selected ? _L("耗材 %llu（已选）\n") : _L("耗材 %llu\n"),
            static_cast<unsigned long long>(index + 1)) + from_u8(palette[index]));
        button->SetToolTip(wxString::Format(
            _L("将选中区域改为耗材 %llu："), static_cast<unsigned long long>(index + 1)) +
            from_u8(palette[index]));
        if (color.IsOk()) {
            button->SetBackgroundColour(color);
            const double luminance = 0.299 * color.Red() + 0.587 * color.Green() + 0.114 * color.Blue();
            button->SetForegroundColour(luminance >= 150.0 ? *wxBLACK : *wxWHITE);
        }
        button->Enable(editing && !m_busy);
    }

    if (m_model_preview != nullptr && m_region_color_index < int(palette.size())) {
        const wxColour preview(from_u8(palette[m_region_color_index]));
        if (preview.IsOk()) {
            m_model_preview->set_selection_preview_color(ColorRGBA(
                preview.Red() / 255.0f,
                preview.Green() / 255.0f,
                preview.Blue() / 255.0f,
                1.0f));
        }
    }
    m_undo_region_selection->Enable(editing && !m_busy && can_undo);
    m_clear_region_selection->Enable(editing && !m_busy && has_selection);
    m_apply_region_color->SetLabel(palette.empty()
        ? _L("没有可用耗材颜色")
        : wxString::Format(_L("应用为耗材 %d"), m_region_color_index + 1));
    m_apply_region_color->Enable(editing && !m_busy && has_selection && !palette.empty());
    m_local_recolor_panel->Layout();
    if (m_local_recolor_panel->GetParent() != nullptr)
        m_local_recolor_panel->GetParent()->Layout();
}

void ModelGenerationPanel::on_apply_local_recolor(wxCommandEvent&)
{
    if (m_busy || !m_model_preview_ready || m_model_preview == nullptr ||
        m_model_preview->selected_face_count() == 0)
        return;
    const std::vector<std::string> palette = local_recolor_palette();
    const int color_index = m_region_color_index;
    if (color_index == wxNOT_FOUND || color_index >= int(palette.size())) {
        m_status->SetLabel(_L("请先选择一个当前打印机耗材颜色。"));
        return;
    }
    const boost::filesystem::path source = is_nonempty_obj(m_displayed_model_path)
        ? m_displayed_model_path : m_artifact_path;
    if (!is_nonempty_obj(source)) {
        m_status->SetLabel(_L("当前 OBJ 文件已不存在，请重新加载模型。"));
        return;
    }
    const wxColour selected_color(from_u8(palette[color_index]));
    if (!selected_color.IsOk()) {
        m_status->SetLabel(_L("当前耗材颜色无效，请重新配置耗材。"));
        return;
    }

    const std::string edit_id = "edit-" + new_request_id();
    const boost::filesystem::path destination = temp_path(edit_id, "obj");
    const bool source_uses_printable_colors = m_job_use_printable_colors;
    const std::vector<std::string> display_palette = source_uses_printable_colors
        ? palette : std::vector<std::string> {};
    AIModelGenerationClient::PaletteRoles display_palette_roles =
        display_palette == m_displayed_model_palette ? m_displayed_model_palette_roles
                                                     : automatic_palette_roles(display_palette);
    if (display_palette_roles.empty())
        display_palette_roles = automatic_palette_roles(display_palette);
    const RGBA color {
        selected_color.Red() / 255.0f,
        selected_color.Green() / 255.0f,
        selected_color.Blue() / 255.0f,
        1.0f
    };
    m_busy = true;
    m_status->SetLabel(_L("正在保存局部改色 OBJ..."));
    refresh_controls();
    wxBusyCursor busy;
    std::string error;
    if (!m_model_preview->apply_selection_color(color, source, destination, error)) {
        m_busy = false;
        m_status->SetLabel(_L("局部改色保存失败：") + from_u8(error));
        refresh_controls();
        return;
    }

    size_t triangle_count = 0;
    size_t color_count = 0;
    Vec3d dimensions = Vec3d::Zero();
    if (!m_model_preview->load_model(destination, display_palette, triangle_count, dimensions, color_count, error)) {
        m_busy = false;
        m_model_preview_ready = false;
        m_status->SetLabel(_L("改色文件已保存，但重新加载失败：") + from_u8(error));
        refresh_controls();
        return;
    }

    m_artifact_path = destination;
    m_displayed_model_path = destination;
    m_displayed_model_job_id.clear();
    m_displayed_model_palette = display_palette;
    m_displayed_model_palette_roles = display_palette_roles;
    m_job_palette = display_palette;
    m_job_palette_roles = display_palette_roles;
    m_job_use_printable_colors = source_uses_printable_colors;
    m_model_preview_ready = true;
    m_library_model_loaded = false;
    m_busy = false;
    m_visual_quality = {};
    m_model_stats->SetLabel(wxString::Format(
        _L("%llu 个三角面 · %llu 种颜色\n%.1f × %.1f × %.1f mm"),
        static_cast<unsigned long long>(triangle_count), static_cast<unsigned long long>(color_count),
        dimensions.x(), dimensions.y(), dimensions.z()));
    m_model_preview_message->SetLabel(_L("局部改色已保存；可继续选择其他区域，或导入准备页。"));
    m_status->SetLabel(_L("局部改色完成，原始 OBJ 已保留。"));
    m_result_summary->SetLabel(_L("已生成新的顶点色 OBJ，可继续预览、改色或导入切片。"));

    nlohmann::json metadata {
        {"schema_version", 2},
        {"job_id", edit_id},
        {"model_path", destination.lexically_relative(generated_models_root()).generic_string()},
        {"source", "local_recolor"},
        {"prompt", "局部改色模型"},
        {"palette", display_palette},
        {"palette_roles", display_palette_roles},
        {"use_printable_colors", source_uses_printable_colors},
        {"recolor_target", palette[color_index]},
        {"recolor_target_palette", palette},
        {"preserves_unselected_vertex_colors", true},
        {"generated_at", std::time(nullptr)},
        {"triangle_count", triangle_count},
        {"color_count", color_count},
        {"dimensions", {dimensions.x(), dimensions.y(), dimensions.z()}},
        {"source_model", source.lexically_relative(generated_models_root()).generic_string()}
    };
    boost::filesystem::ofstream metadata_stream(library_metadata_path(edit_id));
    if (metadata_stream) {
        metadata_stream << metadata.dump(2);
        metadata_stream.close();
    } else {
        BOOST_LOG_TRIVIAL(warning) << "Unable to write local recolor metadata for " << edit_id;
    }
    load_library_entries();
    refresh_model_quality_card();
    refresh_controls();
    m_model_preview->set_selection_enabled(m_local_recolor_toggle->GetValue());
    m_model_preview->refresh();
}

std::vector<size_t> ModelGenerationPanel::valid_project_slots() const
{
    return m_palette_provider.printable_palette().valid_slots;
}

std::vector<size_t> ModelGenerationPanel::compatible_project_slots() const
{
    return m_palette_provider.printable_palette().compatible_slots;
}

std::vector<std::string> ModelGenerationPanel::project_palette() const
{
    return m_palette_provider.printable_palette().compatible_colors;
}

std::vector<std::string> ModelGenerationPanel::current_palette() const
{
    if (!use_printable_colors())
        return {};
    return m_palette_source != nullptr && m_palette_source->GetSelection() != 0 ? m_custom_palette : project_palette();
}

AIModelGenerationClient::PaletteRoles ModelGenerationPanel::current_palette_roles() const
{
    return use_printable_colors() ? m_palette_roles : AIModelGenerationClient::PaletteRoles {};
}

void ModelGenerationPanel::refresh_palette_roles(const std::vector<std::string>& palette)
{
    if (palette != m_palette_roles_source) {
        m_palette_roles_source = palette;
        m_palette_roles = automatic_palette_roles(palette);
    }
    for (size_t index = 0; index < m_palette_role_choices.size(); ++index) {
        wxChoice* choice = m_palette_role_choices[index];
        if (choice == nullptr)
            continue;
        choice->Freeze();
        choice->Clear();
        for (const std::string& color : palette)
            choice->Append(from_u8(color));
        const auto role = m_palette_roles.find(PALETTE_ROLE_IDS[index]);
        if (role != m_palette_roles.end()) {
            const auto color = std::find(palette.begin(), palette.end(), role->second);
            choice->SetSelection(color == palette.end() ? wxNOT_FOUND : int(std::distance(palette.begin(), color)));
            choice->Enable(!m_busy && use_printable_colors());
        } else {
            choice->SetSelection(wxNOT_FOUND);
            choice->Enable(false);
        }
        choice->Thaw();
    }
}

void ModelGenerationPanel::on_palette_role_changed(size_t role_index)
{
    if (m_busy || role_index >= m_palette_role_choices.size())
        return;
    wxChoice* choice = m_palette_role_choices[role_index];
    const int selection = choice == nullptr ? wxNOT_FOUND : choice->GetSelection();
    const std::vector<std::string> palette = current_palette();
    if (selection == wxNOT_FOUND || selection >= int(palette.size()))
        return;
    const std::string role = PALETTE_ROLE_IDS[role_index];
    const std::string selected = palette[selection];
    const std::string previous = m_palette_roles[role];
    for (auto& [other_role, color] : m_palette_roles) {
        if (other_role != role && color == selected) {
            color = previous;
            break;
        }
    }
    m_palette_roles[role] = selected;
    refresh_palette_roles(palette);
    refresh_controls();
}

bool ModelGenerationPanel::use_printable_colors() const
{
    return m_use_printable_colors == nullptr || m_use_printable_colors->GetValue();
}

std::string ModelGenerationPanel::current_style() const
{
    switch (m_style == nullptr ? wxNOT_FOUND : m_style->GetSelection()) {
    case 0: return "q_cartoon";
    case 1: return "low_poly";
    case 2: return "cel_shaded";
    case 3: return "enamel_inlay";
    case 4: return "sculpture";
    case 5: return "custom";
    default: return "q_cartoon";
    }
}

std::string ModelGenerationPanel::current_custom_style() const
{
    if (m_custom_style == nullptr || current_style() != "custom")
        return {};
    wxString value = m_custom_style->GetValue();
    value.Trim(true).Trim(false);
    return value.ToUTF8().data();
}

wxString ModelGenerationPanel::current_style_label() const
{
    return m_style == nullptr || m_style->GetSelection() == wxNOT_FOUND
        ? _L("雕塑（适合单色）")
        : m_style->GetStringSelection();
}

int ModelGenerationPanel::current_face_limit() const
{
    static constexpr std::array<int, 4> limits {100000, 300000, 500000, 1000000};
    const int selection = m_quality == nullptr ? 1 : m_quality->GetSelection();
    return limits[selection >= 0 && selection < static_cast<int>(limits.size()) ? selection : 1];
}

AIModelGenerationClient::ImagePrintSettings ModelGenerationPanel::current_print_settings() const
{
    AIModelGenerationClient::ImagePrintSettings settings;
    if (m_print_width != nullptr) settings.width_mm = m_print_width->GetValue();
    if (m_nozzle_size != nullptr) settings.nozzle_mm = m_nozzle_size->GetValue();
    if (m_line_width != nullptr) settings.line_width_mm = m_line_width->GetValue();
    if (m_minimum_feature != nullptr) settings.minimum_feature_mm = m_minimum_feature->GetValue();
    static constexpr std::array<const char*, 4> shadows {"blue", "red", "green", "white"};
    const int selection = m_shadow_color == nullptr ? 0 : m_shadow_color->GetSelection();
    settings.shadow_color = shadows[selection >= 0 && selection < int(shadows.size()) ? selection : 0];
    return settings;
}

bool ModelGenerationPanel::has_image_input() const
{
    return !m_selected_image_path.empty();
}

bool ModelGenerationPanel::job_uses_image() const
{
    return !m_job_image_path.empty();
}

bool ModelGenerationPanel::job_inputs_match() const
{
    return job_base_inputs_match() &&
           (m_awaiting_palette_confirmation || current_palette_roles() == m_job_palette_roles);
}

bool ModelGenerationPanel::job_base_inputs_match() const
{
    const auto settings = current_print_settings();
    const bool print_matches = std::abs(settings.width_mm - m_job_print_settings.width_mm) < 0.001 &&
        std::abs(settings.nozzle_mm - m_job_print_settings.nozzle_mm) < 0.001 &&
        std::abs(settings.line_width_mm - m_job_print_settings.line_width_mm) < 0.001 &&
        std::abs(settings.minimum_feature_mm - m_job_print_settings.minimum_feature_mm) < 0.001 &&
        settings.shadow_color == m_job_print_settings.shadow_color;
    return m_job_id.empty() || (m_prompt->GetValue() == m_job_prompt && m_selected_image_path == m_job_image_path &&
                                current_style() == m_job_style && current_custom_style() == m_job_custom_style &&
                                print_matches);
}

void ModelGenerationPanel::remove_custom_color(const std::string& color)
{
    if (m_busy || m_palette_source->GetSelection() == 0)
        return;
    const auto item = std::find(m_custom_palette.begin(), m_custom_palette.end(), color);
    if (item != m_custom_palette.end())
        m_custom_palette.erase(item);
    refresh_palette();
    refresh_controls();
}

void ModelGenerationPanel::replace_recommended_color(size_t index)
{
    if (m_busy || m_palette_source->GetSelection() != 2 || index >= m_custom_palette.size())
        return;
    wxColourData data;
    data.SetChooseFull(true);
    data.SetColour(wxColour(from_u8(m_custom_palette[index])));
    wxColourDialog dialog(this, &data);
    if (dialog.ShowModal() != wxID_OK)
        return;
    std::string replacement = dialog.GetColourData().GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    std::transform(replacement.begin(), replacement.end(), replacement.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    const auto duplicate = std::find(m_custom_palette.begin(), m_custom_palette.end(), replacement);
    if (duplicate != m_custom_palette.end() && size_t(std::distance(m_custom_palette.begin(), duplicate)) != index) {
        MessageDialog warning(this, _L("这个颜色已经在当前目标色板中。"), wxEmptyString, wxOK | wxICON_INFORMATION);
        warning.ShowModal();
        return;
    }
    const std::string previous = m_custom_palette[index];
    m_custom_palette[index] = replacement;
    for (auto& [role, color] : m_palette_roles)
        if (color == previous) color = replacement;
    for (auto& color : m_palette_recommendation.colors)
        if (color.hex == previous) color.hex = replacement;
    if (std::find(m_user_adjusted_palette_colors.begin(), m_user_adjusted_palette_colors.end(), replacement) ==
        m_user_adjusted_palette_colors.end())
        m_user_adjusted_palette_colors.emplace_back(replacement);
    refresh_palette();
    refresh_controls();
}

void ModelGenerationPanel::refresh_palette_recommendation()
{
    if (m_palette_recommendation_panel == nullptr || m_palette_source == nullptr)
        return;
    const bool ai_source = use_printable_colors() && m_palette_source->GetSelection() == 2;
    m_palette_recommendation_panel->Show(ai_source);
    if (!ai_source)
        return;

    const bool stale = !m_job_id.empty() && !job_base_inputs_match();
    if (m_busy && !m_awaiting_confirmation)
        m_palette_recommendation_summary->SetLabel(_L("AI 正在分析主体、风格和适合打印的大色区..."));
    else if (m_palette_recommendation.available) {
        wxString text = from_u8(m_palette_recommendation.summary);
        if (stale)
            text += _L("\n输入已变化：可重新推荐，或确认继续使用当前配色。");
        else if (m_palette_recommendation_confirmed)
            text += _L("\n已确认；导入时请把这些目标色匹配到实际耗材。");
        else
            text += _L("\n可替换、删除或补充颜色，确认后再生成图片预览。");
        m_palette_recommendation_summary->SetLabel(text);
    } else {
        m_palette_recommendation_summary->SetLabel(
            _L("AI 会推荐理想目标色；确认后再由你匹配实际耗材。"));
    }
    m_palette_recommendation_summary->Wrap(FromDIP(300));

    const std::vector<std::string> palette = current_palette();
    for (size_t index = 0; index < m_palette_recommendation_cards.size(); ++index) {
        const bool visible = index < palette.size();
        m_palette_recommendation_cards[index]->Show(visible);
        if (!visible)
            continue;
        const std::string& hex = palette[index];
        m_palette_recommendation_swatches[index]->SetBackgroundColour(wxColour(from_u8(hex)));
        const auto detail = std::find_if(
            m_palette_recommendation.colors.begin(), m_palette_recommendation.colors.end(),
            [&hex](const AIModelGenerationClient::PaletteRecommendationColor& color) { return color.hex == hex; });
        wxString label = from_u8(hex);
        if (detail != m_palette_recommendation.colors.end()) {
            label += _L(" · ") + from_u8(detail->name) + _L(" · ") + from_u8(detail->usage) +
                     "\n" + from_u8(detail->reason);
        } else {
            label += _L(" · 用户添加的目标色");
        }
        if (std::find(m_user_adjusted_palette_colors.begin(), m_user_adjusted_palette_colors.end(), hex) !=
            m_user_adjusted_palette_colors.end())
            label += _L("（用户已调整）");
        m_palette_recommendation_details[index]->SetLabel(label);
        m_palette_recommendation_details[index]->Wrap(FromDIP(230));
        m_palette_recommendation_replace[index]->Enable(!m_busy);
        m_palette_recommendation_remove[index]->Enable(!m_busy && palette.size() > 1);
    }
    const bool valid_input = !m_prompt->GetValue().empty() || has_image_input();
    m_recommend_palette->SetLabel(m_palette_recommendation.available ? _L("重新推荐四色") : _L("AI 推荐四色"));
    m_recommend_palette->Enable(m_service_available && !m_busy && valid_input);
    m_confirm_recommended_palette->Show(m_awaiting_palette_confirmation);
    m_confirm_recommended_palette->SetLabel(stale ? _L("继续使用此配色") : _L("确认配色并生成预览"));
    m_confirm_recommended_palette->Enable(
        m_service_available && !m_busy && m_awaiting_palette_confirmation && !palette.empty());
    m_palette_recommendation_panel->Layout();
}

void ModelGenerationPanel::refresh_palette()
{
    if (m_palette_sizer == nullptr || m_palette_summary == nullptr)
        return;
    const std::vector<std::string> palette = current_palette();
    refresh_palette_roles(palette);
    const bool enabled = use_printable_colors();
    const bool custom = m_palette_source->GetSelection() != 0;
    const bool palette_changed = palette != m_palette || custom != m_palette_is_custom;
    if (palette_changed) {
        m_palette = palette;
        m_palette_is_custom = custom;
        m_palette_sizer->Clear(true);
        for (const std::string& color : m_palette) {
            auto* swatch = new wxPanel(m_palette_panel, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(24), FromDIP(24)), wxBORDER_SIMPLE);
            swatch->SetMinSize(wxSize(FromDIP(24), FromDIP(24)));
            swatch->SetBackgroundColour(wxColour(wxString::FromUTF8(color)));
            swatch->SetToolTip(wxString::FromUTF8(color) + (custom ? _L(" · 点击移除") : wxString()));
            if (custom) {
                swatch->SetCursor(wxCursor(wxCURSOR_HAND));
                swatch->Bind(wxEVT_LEFT_UP, [this, color](wxMouseEvent&) { remove_custom_color(color); });
            }
            m_palette_sizer->Add(swatch);
        }
    }
    if (!enabled) {
        m_palette_summary->SetLabel(_L("保留自然颜色生成，不限制到耗材色板。\n导入时按“导入颜色”设置处理。"));
        m_palette_summary->SetForegroundColour(wxColour(91, 104, 107));
    } else if (m_palette.empty() && m_palette_source->GetSelection() == 2) {
        m_palette_summary->SetLabel(_L("尚未生成 AI 设计目标色。"));
        m_palette_summary->SetForegroundColour(wxColour(91, 104, 107));
    } else if (m_palette.empty()) {
        m_palette_summary->SetLabel(_L("当前没有配置有效的耗材颜色。"));
        m_palette_summary->SetForegroundColour(wxColour(180, 55, 55));
    } else if (!m_job_palette.empty() && m_palette != m_job_palette) {
        m_palette_summary->SetLabel(_L("耗材颜色已变化，请重新生成预览以使用当前色板。"));
        m_palette_summary->SetForegroundColour(wxColour(180, 55, 55));
    } else if (m_palette_source->GetSelection() == 2) {
        m_palette_summary->SetLabel(wxString::Format(
            _L("%llu 种 AI 设计目标色 · 导入时由你匹配实际耗材"),
            static_cast<unsigned long long>(m_palette.size())));
        m_palette_summary->SetForegroundColour(wxColour(91, 104, 107));
    } else if (m_palette_source->GetSelection() == 1) {
        m_palette_summary->SetLabel(wxString::Format(_L("%llu 种自定义颜色 · 点击色块可移除"),
                                                     static_cast<unsigned long long>(m_palette.size())));
        m_palette_summary->SetForegroundColour(wxColour(91, 104, 107));
    } else {
        const size_t valid_slots = valid_project_slots().size();
        const size_t compatible_slots = compatible_project_slots().size();
        if (compatible_slots < valid_slots) {
            m_palette_summary->SetLabel(wxString::Format(
                _L("已选择 %llu 种兼容耗材色（最多 4 种）\n已排除 %llu 个不兼容或超出上限的槽位"),
                static_cast<unsigned long long>(m_palette.size()),
                static_cast<unsigned long long>(valid_slots - compatible_slots)));
        } else {
            m_palette_summary->SetLabel(wxString::Format(_L("当前耗材：%llu 种颜色"),
                                                         static_cast<unsigned long long>(m_palette.size())));
        }
        m_palette_summary->SetForegroundColour(wxColour(91, 104, 107));
    }
    if (enabled && palette.size() > 1 && minimum_palette_distance(palette) < 12.0) {
        m_palette_summary->SetLabel(m_palette_summary->GetLabel() +
                                    _L("\n提示：部分耗材颜色非常接近，打印后色区可能不易区分。"));
        m_palette_summary->SetForegroundColour(wxColour(174, 112, 22));
    }
    m_palette_source->Show(enabled);
    m_palette_panel->Show(enabled);
    m_palette_roles_panel->Show(enabled && !m_palette.empty());
    m_custom_color_panel->Show(enabled && custom);
    refresh_palette_recommendation();
    m_palette_panel->Layout();
    m_palette_panel->GetParent()->Layout();
}

void ModelGenerationPanel::reset(bool remove_remote)
{
    m_poll_timer.Stop();
    m_client.cancel_current();
    const std::string old_job = m_job_id;
    ++m_sequence;
    cleanup_files();
    m_job_id.clear();
    m_job_palette.clear();
    m_job_palette_roles.clear();
    m_job_use_printable_colors = false;
    m_job_prompt.clear();
    m_job_style.clear();
    m_job_custom_style.clear();
    m_job_face_limit = 300000;
    m_job_image_path.clear();
    m_artifact_format.clear();
    m_artifact_color_encoding.clear();
    m_busy = false;
    m_awaiting_confirmation = false;
    m_awaiting_palette_confirmation = false;
    m_palette_recommendation_confirmed = false;
    m_ready = false;
    m_artifact_download_started = false;
    m_model_preview_ready = false;
    m_library_model_loaded = false;
    m_displayed_model_path.clear();
    m_displayed_model_job_id.clear();
    m_displayed_model_palette.clear();
    m_displayed_model_palette_roles.clear();
    clear_model_quality();
    if (m_model_preview != nullptr)
        m_model_preview->clear();
    if (m_preview_book != nullptr)
        m_preview_book->SetSelection(0);
    if (m_model_stats != nullptr)
        m_model_stats->SetLabel(_L("模型生成后将在这里显示"));
    if (m_model_preview_message != nullptr)
        m_model_preview_message->SetLabel(_L("生成完成后可拖动旋转模型，并使用滚轮缩放。"));
    m_style_preview_ready = false;
    m_raw_preview_available = false;
    m_strict_preview_available = false;
    m_heatmap_available = false;
    m_palette_quality_ok = true;
    m_meaningful_palette_count = 0;
    m_meaningful_subject_color_count = 0;
    m_generation_progress->SetValue(0);
    update_workflow();
    m_status->SetLabel(_L("空闲"));
    m_prepared_prompt->Clear();
    set_preview_empty(_L("请先输入描述或选择参考图。"));
    if (!m_selected_image_path.empty())
        show_selected_image_preview();
    m_result_summary->SetLabel(_L("尚未生成模型。"));
    if (remove_remote && !old_job.empty())
        m_client.remove(old_job, [] {}, [](std::string) {});
    refresh_controls();
}

void ModelGenerationPanel::cleanup_files()
{
    m_preview_path.clear();
    m_artifact_path.clear();
}

void ModelGenerationPanel::load_library_entries()
{
    const boost::filesystem::path root = generated_models_root();
    const boost::filesystem::path downloads = root / "downloads";
    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(root, ec)) {
        m_library_entries.clear();
        refresh_library();
        return;
    }

    std::map<std::string, boost::filesystem::path> models;
    for (boost::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        boost::system::error_code entry_ec;
        if (!boost::filesystem::is_directory(it->path(), entry_ec))
            continue;
        const std::string job_id = it->path().filename().string();
        if (job_id == "downloads" || job_id.rfind("attempt-", 0) == 0)
            continue;
        const boost::filesystem::path model_path = it->path() / "model-vertex-color.obj";
        if (boost::filesystem::is_regular_file(model_path, entry_ec) &&
            boost::filesystem::file_size(model_path, entry_ec) > 0 && !entry_ec)
            models.emplace(job_id, model_path);
    }
    ec.clear();
    if (boost::filesystem::is_directory(downloads, ec)) {
        for (boost::filesystem::directory_iterator it(downloads, ec), end; !ec && it != end; it.increment(ec)) {
            const boost::filesystem::path path = it->path();
            boost::system::error_code entry_ec;
            if (!boost::filesystem::is_regular_file(path, entry_ec) || path.extension() != ".obj")
                continue;
            const std::string job_id = download_job_id(path);
            if (!job_id.empty() && boost::filesystem::file_size(path, entry_ec) > 0 && !entry_ec)
                models.emplace(job_id, path);
        }
    }

    std::vector<GeneratedModelEntry> entries;
    entries.reserve(models.size());
    for (const auto& [job_id, model_path] : models) {
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
            entry.use_printable_colors = metadata.value("use_printable_colors", false);
            const std::string prompt = metadata.value("prompt", std::string());
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

        entries.emplace_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const GeneratedModelEntry& lhs, const GeneratedModelEntry& rhs) {
        if (lhs.generated_at != rhs.generated_at)
            return lhs.generated_at > rhs.generated_at;
        return lhs.job_id < rhs.job_id;
    });
    m_library_entries = std::move(entries);
    refresh_library();
}

void ModelGenerationPanel::save_library_entry(size_t artifact_size, size_t triangle_count, double width,
                                               double depth, double height, size_t color_count)
{
    if (m_job_id.empty() || m_displayed_model_path.empty())
        return;
    const boost::filesystem::path root = generated_models_root();
    nlohmann::json metadata {
        {"schema_version", 2},
        {"job_id", m_job_id},
        {"model_path", m_displayed_model_path.lexically_relative(root).generic_string()},
        {"source", job_uses_image() ? (m_job_prompt.empty() ? "image" : "text_image") : "text"},
        {"style", m_job_style},
        {"custom_style", m_job_custom_style},
        {"face_limit", m_job_face_limit},
        {"prompt", std::string(m_job_prompt.ToUTF8().data())},
        {"palette", m_job_palette},
        {"palette_roles", m_job_palette_roles},
        {"use_printable_colors", m_job_use_printable_colors},
        {"generated_at", std::time(nullptr)},
        {"artifact_size", artifact_size},
        {"triangle_count", triangle_count},
        {"color_count", color_count},
        {"dimensions", {width, depth, height}}
    };
    if (!m_preview_path.empty())
        metadata["preview_path"] = m_preview_path.lexically_relative(root).generic_string();

    boost::filesystem::ofstream stream(library_metadata_path(m_job_id));
    if (!stream) {
        BOOST_LOG_TRIVIAL(warning) << "Unable to write generated model library metadata for " << m_job_id;
    } else {
        stream << metadata.dump(2);
        stream.close();
    }
    load_library_entries();
}

void ModelGenerationPanel::load_library_entry(const boost::filesystem::path& model_path,
                                               const std::vector<std::string>& palette,
                                               const AIModelGenerationClient::PaletteRoles& palette_roles,
                                               bool use_printable_colors,
                                               const std::string& job_id, const wxString& title)
{
    if (m_busy || m_model_preview == nullptr)
        return;
    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(model_path, ec)) {
        m_status->SetLabel(_L("历史模型文件已不存在。"));
        return;
    }

    m_status->SetLabel(_L("正在加载历史模型：") + title);
    m_model_stats->SetLabel(_L("正在解析 OBJ 模型..."));
    Update();
    wxBusyCursor busy;
    size_t triangle_count = 0;
    size_t color_count = 0;
    Vec3d dimensions = Vec3d::Zero();
    std::string error;
    if (!m_model_preview->load_model(model_path, palette, triangle_count, dimensions, color_count, error)) {
        m_model_preview_ready = false;
        m_library_model_loaded = false;
        m_displayed_model_path.clear();
        m_displayed_model_palette.clear();
        m_displayed_model_palette_roles.clear();
        m_status->SetLabel(_L("历史 OBJ 模型加载失败。"));
        m_model_stats->SetLabel(_L("模型预览不可用"));
        m_result_summary->SetLabel(from_u8(error));
        refresh_controls();
        return;
    }

    m_poll_timer.Stop();
    m_client.cancel_current();
    ++m_sequence;
    m_job_id.clear();
    m_job_palette = palette;
    m_job_palette_roles = palette_roles.empty() ? automatic_palette_roles(palette) : palette_roles;
    m_job_use_printable_colors = use_printable_colors;
    m_job_prompt.clear();
    m_job_style.clear();
    m_job_custom_style.clear();
    m_job_image_path.clear();
    m_artifact_path = model_path;
    m_artifact_format = "obj";
    m_artifact_color_encoding = "vertex_colors";
    m_busy = false;
    m_awaiting_confirmation = false;
    m_ready = true;
    m_artifact_download_started = true;
    if (m_use_printable_colors != nullptr)
        m_use_printable_colors->SetValue(use_printable_colors);
    m_displayed_model_path = model_path;
    m_displayed_model_job_id = job_id;
    m_displayed_model_palette = palette;
    m_displayed_model_palette_roles = m_job_palette_roles;
    m_model_preview_ready = true;
    m_library_model_loaded = true;
    clear_model_quality();
    update_progress(95, 4, _L("检查 3D 模型"));
    m_model_stats->SetLabel(wxString::Format(
        _L("%llu 个三角面 · %llu 种颜色\n%.1f × %.1f × %.1f mm"),
        static_cast<unsigned long long>(triangle_count), static_cast<unsigned long long>(color_count),
        dimensions.x(), dimensions.y(), dimensions.z()));
    m_model_preview_message->SetLabel(_L("已从模型库加载。确认外观后点击“导入并切片”。"));
    m_status->SetLabel(_L("已加载历史模型：") + title);
    m_result_summary->SetLabel(_L("历史模型已加载到当前 3D 预览，可继续导入准备页并切片。"));
    if (m_preview_book != nullptr)
        m_preview_book->SetSelection(1);
    m_model_preview->refresh();
    refresh_controls();
    const uint64_t sequence = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.get_status(job_id,
        [weak, sequence, job_id](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                if (!status.palette_roles.empty() && status.palette == weak->m_displayed_model_palette) {
                    weak->m_job_palette_roles = status.palette_roles;
                    weak->m_displayed_model_palette_roles = status.palette_roles;
                }
                weak->apply_model_quality(status.model_quality);
                weak->apply_visual_quality(status.visual_quality);
                weak->refresh_controls();
            });
        },
        [weak, sequence, job_id](std::string) {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                weak->refresh_model_quality_card();
                weak->refresh_controls();
            });
        });
}

void ModelGenerationPanel::refresh_library()
{
    if (m_library_sizer == nullptr || m_library_scroller == nullptr)
        return;
    m_library_sizer->Clear(true);
    m_library_empty->Show(m_library_entries.empty());
    for (const GeneratedModelEntry& entry : m_library_entries) {
        auto* card = new wxPanel(m_library_scroller, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* format = new wxStaticText(card, wxID_ANY, "OBJ");
        format->SetForegroundColour(wxColour(31, 122, 116));
        row->Add(format, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(12));
        auto* text = new wxBoxSizer(wxVERTICAL);
        auto* title = new wxStaticText(card, wxID_ANY, entry.title);
        wxFont title_font = title->GetFont();
        title_font.SetWeight(wxFONTWEIGHT_BOLD);
        title->SetFont(title_font);
        text->Add(title, 0, wxBOTTOM, FromDIP(3));
        auto* details = new wxStaticText(card, wxID_ANY, entry.details);
        details->SetForegroundColour(wxColour(91, 104, 107));
        text->Add(details, 0);
        row->Add(text, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(8));
        card->SetSizer(row);
        const auto bind_load = [this, model_path = entry.model_path, palette = entry.palette,
                                palette_roles = entry.palette_roles,
                                use_printable_colors = entry.use_printable_colors,
                                job_id = entry.job_id,
                                title_text = entry.title](wxWindow* window) {
            window->SetCursor(wxCursor(wxCURSOR_HAND));
            window->SetToolTip(_L("双击加载到 3D 模型预览"));
            window->Bind(wxEVT_LEFT_DCLICK, [this, model_path, palette, palette_roles, use_printable_colors, job_id,
                                             title_text](wxMouseEvent&) {
                load_library_entry(model_path, palette, palette_roles, use_printable_colors, job_id, title_text);
            });
        };
        bind_load(card);
        bind_load(format);
        bind_load(title);
        bind_load(details);
        m_library_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }
    m_library_scroller->FitInside();
    m_library_scroller->Layout();
}

void ModelGenerationPanel::show_selected_image_preview()
{
    if (m_selected_image_path.empty())
        return;
    wxImage image(m_selected_image_path.wstring());
    if (!image.IsOk())
        return;
    m_reference_image = image;
    m_raw_preview_image = wxImage();
    m_strict_preview_image = wxImage();
    m_clean_preview_image = wxImage();
    m_heatmap_image = wxImage();
    m_style_preview_image = wxImage();
    m_style_preview_bitmap = wxNullBitmap;
    m_preview_zoom_factor = 1.0;
    m_style_preview_placeholder = _L("等待生成 AI 处理图");
    if (m_preview_stage != nullptr)
        m_preview_stage->SetSelection(2);
    m_preview_kind->SetLabel(_L("图片对照"));
    m_preview_message->SetLabel(
        wxString::Format(_L("参考图 %d × %d px  ·  等待生成 AI 处理图"), image.GetWidth(), image.GetHeight()));
    update_preview_view(true);
}

void ModelGenerationPanel::apply_preview_stage(bool center)
{
    const int selection = m_preview_stage == nullptr ? 2 : m_preview_stage->GetSelection();
    const wxImage* selected = nullptr;
    if (selection == 0 && m_raw_preview_image.IsOk()) selected = &m_raw_preview_image;
    else if (selection == 1 && m_strict_preview_image.IsOk()) selected = &m_strict_preview_image;
    else if (selection == 2 && m_clean_preview_image.IsOk()) selected = &m_clean_preview_image;
    else if (selection == 3 && m_heatmap_image.IsOk()) selected = &m_heatmap_image;
    else if (m_clean_preview_image.IsOk()) selected = &m_clean_preview_image;
    else if (m_raw_preview_image.IsOk()) selected = &m_raw_preview_image;
    m_style_preview_image = selected == nullptr ? wxImage() : selected->Copy();
    m_style_preview_bitmap = wxNullBitmap;
    if (m_preview_stage != nullptr)
        m_preview_stage->Enable(m_raw_preview_available || m_strict_preview_available || m_heatmap_available);
    update_preview_view(center);
    if (m_preview_area != nullptr)
        m_preview_area->Update();
}

void ModelGenerationPanel::update_preview_view(bool center)
{
    if (m_preview_area == nullptr || m_updating_preview)
        return;
    m_updating_preview = true;
    if (!m_reference_image.IsOk() && !m_style_preview_image.IsOk()) {
        m_reference_bitmap = wxNullBitmap;
        m_style_preview_bitmap = wxNullBitmap;
        m_reference_preview_pane = wxRect();
        m_style_preview_pane = wxRect();
        m_preview_area->SetVirtualSize(m_preview_area->GetClientSize());
        m_preview_area->Refresh();
        m_updating_preview = false;
        return;
    }

    const wxSize client = m_preview_area->GetClientSize();
    const int padding = FromDIP(16);
    const int gap = FromDIP(16);
    const int label_height = FromDIP(32);
    const int stage = m_preview_stage == nullptr ? 2 : m_preview_stage->GetSelection();
    const wxImage* comparison_image = nullptr;
    if (m_reference_image.IsOk())
        comparison_image = &m_reference_image;
    else if (stage != 0 && m_raw_preview_image.IsOk())
        comparison_image = &m_raw_preview_image;
    const bool comparison = comparison_image != nullptr &&
                            (m_reference_image.IsOk() || m_style_preview_image.IsOk());
    const int base_pane_width = comparison
        ? std::max(1, (client.GetWidth() - 2 * padding - gap) / 2)
        : std::max(1, client.GetWidth() - 2 * padding);
    const int base_image_height = std::max(1, client.GetHeight() - 2 * padding - label_height);

    auto update_bitmap = [&](const wxImage& image, wxBitmap& bitmap) {
        if (!image.IsOk()) {
            bitmap = wxNullBitmap;
            return;
        }
        const double fit_scale = std::min({ 1.0,
            double(base_pane_width) / image.GetWidth(),
            double(base_image_height) / image.GetHeight() });
        double scale = fit_scale * m_preview_zoom_factor;
        scale = std::min(scale, double(MAX_PREVIEW_BITMAP_DIMENSION) / image.GetWidth());
        scale = std::min(scale, double(MAX_PREVIEW_BITMAP_DIMENSION) / image.GetHeight());
        const int width = std::max(1, int(std::lround(image.GetWidth() * scale)));
        const int height = std::max(1, int(std::lround(image.GetHeight() * scale)));
        if (!bitmap.IsOk() || bitmap.GetWidth() != width || bitmap.GetHeight() != height)
            bitmap = wxBitmap(image.Scale(width, height, wxIMAGE_QUALITY_HIGH));
    };

    if (comparison_image != nullptr)
        update_bitmap(*comparison_image, m_reference_bitmap);
    else
        m_reference_bitmap = wxNullBitmap;
    update_bitmap(m_style_preview_image, m_style_preview_bitmap);

    if (comparison) {
        const int reference_width = std::max(base_pane_width,
            m_reference_bitmap.IsOk() ? m_reference_bitmap.GetWidth() : 0);
        const int result_width = std::max(base_pane_width,
            m_style_preview_bitmap.IsOk() ? m_style_preview_bitmap.GetWidth() : 0);
        const int image_height = std::max({ base_image_height,
            m_reference_bitmap.IsOk() ? m_reference_bitmap.GetHeight() : 0,
            m_style_preview_bitmap.IsOk() ? m_style_preview_bitmap.GetHeight() : 0 });
        const int pane_height = label_height + image_height;
        m_reference_preview_pane = wxRect(padding, padding, reference_width, pane_height);
        m_style_preview_pane = wxRect(padding + reference_width + gap, padding, result_width, pane_height);
    } else {
        const wxBitmap& bitmap = m_style_preview_bitmap.IsOk() ? m_style_preview_bitmap : m_reference_bitmap;
        const int pane_width = std::max(base_pane_width, bitmap.IsOk() ? bitmap.GetWidth() : 0);
        const int image_height = std::max(base_image_height, bitmap.IsOk() ? bitmap.GetHeight() : 0);
        if (m_reference_image.IsOk()) {
            m_reference_preview_pane = wxRect(padding, padding, pane_width, label_height + image_height);
            m_style_preview_pane = wxRect();
        } else {
            m_reference_preview_pane = wxRect();
            m_style_preview_pane = wxRect(padding, padding, pane_width, label_height + image_height);
        }
    }

    const wxRect content = m_style_preview_pane.IsEmpty() ? m_reference_preview_pane : m_style_preview_pane;
    const int virtual_width = std::max(client.GetWidth(), content.GetRight() + padding + 1);
    const int virtual_height = std::max(client.GetHeight(), content.GetBottom() + padding + 1);
    m_preview_area->SetVirtualSize(virtual_width, virtual_height);
    if (center) {
        int pixels_per_unit_x = 1;
        int pixels_per_unit_y = 1;
        m_preview_area->GetScrollPixelsPerUnit(&pixels_per_unit_x, &pixels_per_unit_y);
        const int scroll_x = std::max(0, virtual_width - client.GetWidth()) / std::max(1, 2 * pixels_per_unit_x);
        const int scroll_y = std::max(0, virtual_height - client.GetHeight()) / std::max(1, 2 * pixels_per_unit_y);
        m_preview_area->Scroll(scroll_x, scroll_y);
    }
    m_preview_zoom->SetLabel(wxString::Format("%d%%", int(std::lround(m_preview_zoom_factor * 100.0))));
    m_preview_area->Refresh();
    m_updating_preview = false;
}

void ModelGenerationPanel::set_preview_zoom(double zoom)
{
    if (!m_reference_image.IsOk() && !m_style_preview_image.IsOk())
        return;
    m_preview_zoom_factor = std::clamp(zoom, MIN_PREVIEW_ZOOM, MAX_PREVIEW_ZOOM);
    update_preview_view(true);
    refresh_controls();
}

void ModelGenerationPanel::update_progress(int value, int step, const wxString& phase)
{
    value = std::clamp(value, 0, 100);
    step = std::clamp(step, 1, 4);
    m_generation_progress->SetValue(value);
    m_workflow_phase->SetLabel(phase);
    m_workflow_phase->SetToolTip(wxString::Format(_L("第 %d 步，共 4 步"), step));
    m_progress_percent->SetLabel(wxString::Format("%d%%", value));
    for (size_t index = 0; index < m_step_labels.size(); ++index) {
        if (m_step_labels[index] == nullptr)
            continue;
        const int label_step = int(index) + 1;
        const bool active = label_step == step;
        const bool complete = label_step < step;
        m_step_labels[index]->SetForegroundColour(active || complete ? wxColour(24, 112, 105)
                                                                     : wxColour(132, 143, 145));
        wxFont font = m_step_labels[index]->GetFont();
        font.SetWeight(active ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
        m_step_labels[index]->SetFont(font);
    }
}

void ModelGenerationPanel::update_workflow(const AIModelGenerationClient::JobStatus* status)
{
    const bool image_mode = job_uses_image() || m_job_use_printable_colors ||
                            (m_job_id.empty() && (has_image_input() || use_printable_colors()));
    wxString phase = _L("输入");
    wxString guidance = _L("输入文字、图片，或同时使用两者");
    int step = 1;
    int progress = 0;
    if (status != nullptr) {
        progress = display_progress(*status);
        if (status->state == "recommending_palette") {
            phase = _L("推荐打印配色");
            guidance = _L("AI 正在分析主体、风格和适合打印的大色区");
            step = 1;
        } else if (status->state == "awaiting_palette_confirmation") {
            phase = _L("确认目标配色");
            guidance = _L("修改或确认四个设计目标色，再生成图片预览");
            step = 1;
        } else if (status->state == "preprocessing") {
            phase = image_mode ? _L("生成可打印预览") : _L("准备提示词");
            guidance = image_mode ? _L("AI 正在生成并检查图片，请稍候") : _L("AI 正在整理 3D 提示词");
            step = 2;
        } else if (status->state == "awaiting_confirmation") {
            phase = image_mode ? _L("确认可打印预览") : _L("确认提示词");
            guidance = image_mode ? _L("确认右侧图片效果，并选择 3D 模型精度") : _L("确认提示词并选择 3D 模型精度");
            step = 2;
        } else if (status->phase == "generating") {
            phase = _L("生成模型");
            guidance = _L("正在生成 3D 模型，可在这里查看进度");
            step = 3;
        } else if (status->phase == "converting" || status->phase == "downloading_artifact") {
            phase = _L("优化模型");
            guidance = _L("正在优化并下载 3D 模型");
            step = 3;
        } else if (status->state == "ready") {
            phase = _L("检查并导入");
            guidance = _L("检查右侧 3D 模型，然后选择导入方式");
            step = 4;
        } else if (status->state == "stopping") {
            phase = _L("停止生成");
            guidance = _L("正在安全停止当前生成任务");
            step = 3;
        }
    }
    m_workflow_phase->SetLabel(phase);
    m_workflow_steps->SetLabel(guidance);
    update_progress(progress, step, phase);
}

void ModelGenerationPanel::set_preview_empty(const wxString& message)
{
    m_reference_image = wxImage();
    m_raw_preview_image = wxImage();
    m_strict_preview_image = wxImage();
    m_clean_preview_image = wxImage();
    m_heatmap_image = wxImage();
    m_style_preview_image = wxImage();
    m_reference_bitmap = wxNullBitmap;
    m_style_preview_bitmap = wxNullBitmap;
    m_reference_preview_pane = wxRect();
    m_style_preview_pane = wxRect();
    m_style_preview_placeholder.clear();
    if (m_preview_stage != nullptr)
        m_preview_stage->SetSelection(2);
    m_preview_zoom_factor = 1.0;
    if (m_preview_kind != nullptr)
        m_preview_kind->SetLabel(_L("暂无预览"));
    if (m_preview_zoom != nullptr)
        m_preview_zoom->SetLabel("100%");
    m_preview_message->SetLabel(message);
    update_preview_view();
}

} // namespace Slic3r::GUI
