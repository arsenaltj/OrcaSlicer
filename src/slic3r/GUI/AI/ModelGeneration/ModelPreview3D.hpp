#pragma once

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/log/trivial.hpp>
#include <glad/gl.h>
#include <wx/dcclient.h>
#include <wx/glcanvas.h>
#include <wx/panel.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {
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

    bool selection_matches_face_evidence(const std::vector<size_t>& face_indices) const
    {
        if (!m_region_editor.ready() || face_indices.empty())
            return false;
        std::vector<uint8_t> expected(m_region_editor.selected_faces().size(), 0);
        bool has_valid_face = false;
        for (size_t face_index : face_indices) {
            if (face_index >= expected.size())
                continue;
            expected[face_index] = 1;
            has_valid_face = true;
        }
        return has_valid_face && expected == m_region_editor.selected_faces();
    }

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

    size_t select_face_evidence(const std::vector<size_t>& face_indices)
    {
        const std::vector<uint8_t> previous = m_region_editor.selected_faces();
        const size_t localized = m_region_editor.select_faces(face_indices);
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

} // namespace Slic3r::GUI
