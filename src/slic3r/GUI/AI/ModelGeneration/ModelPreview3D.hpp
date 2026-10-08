#pragma once

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelection.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionRefinement.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/BeautyLeafEdits.hpp"
#include "SecondaryRegionEvidence.hpp"
#include "ReadonlyEvidenceRender.hpp"
#include "ModelColorPreviewShader.hpp"
#include "ModelLibraryModelThumbnail.hpp"
#include "ModelPreviewTexture.hpp"
#include "ModelPreviewNormals.hpp"
#include "ModelPreviewGeometry.hpp"
#include "BeautySourceSnapshot.hpp"
#include "ModelHistoryMetadata.hpp"
#include "ModelPreviewPalette.hpp"
#include "ModelPreviewColorControls.hpp"
#include "ModelSemanticColoring.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <unordered_set>
#include <functional>
#include <utility>
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/log/trivial.hpp>
#include <boost/filesystem/fstream.hpp>
#include <glad/gl.h>
#include <wx/dcclient.h>
#include <wx/dialog.h>
#include <wx/glcanvas.h>
#include <wx/panel.h>
#include <wx/button.h>
#include <wx/scrolwin.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/stdpaths.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r::GUI {
class ModelPreview3D final : public wxPanel
{
public:
    enum class SelectionGesture { Lasso, Brush, Protect, Similar, Orbit, Paint };
    using SelectionState = AI::SurfaceSelectionPersistence::SelectionState;
    using FaceColorOverrides = AI::SurfaceSelectionPersistence::FaceColorOverrides;
    using SubfaceColorOverrides = AI::SemanticColoring::SubfaceColors;
    wxWindow* workbench_overlay_parent() const { return m_preview_host; }
    explicit ModelPreview3D(wxWindow* parent, bool retain_surface_attributes = false)
        : wxPanel(parent), m_retain_surface_attributes(retain_surface_attributes)
    {
        wxString trace_value;
        const bool trace = wxGetEnv("ORCASLICER_UI_LATENCY_TRACE", &trace_value) && trace_value == "1";
        const auto started = std::chrono::steady_clock::now();
        auto trace_stage = [trace, &started](const char* stage) {
            if (trace)
                BOOST_LOG_TRIVIAL(info) << "AI canvas build: " << stage << " elapsed_ms="
                    << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        };
        SetBackgroundColour(wxGetApp().get_window_default_clr());
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            wxSP_LIVE_UPDATE | wxSP_3DSASH);
        m_splitter->SetMinimumPaneSize(FromDIP(170));
        m_saved_splitter_sash = FromDIP(360);
        m_preview_host = new wxPanel(m_splitter);
        auto* preview_sizer = new wxBoxSizer(wxVERTICAL);
        m_canvas = OpenGLManager::create_wxglcanvas(*m_preview_host);
        m_canvas->SetMinSize(wxSize(FromDIP(360), FromDIP(260)));
        m_overlay_top = preview_sizer->AddSpacer(0);
        preview_sizer->Add(m_canvas, 1, wxEXPAND);
        m_overlay_bottom = preview_sizer->AddSpacer(0);
        m_preview_host->SetSizer(preview_sizer);
        m_controls_scroll = new wxScrolledWindow(m_splitter, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            wxVSCROLL | wxBORDER_NONE);
        m_controls_scroll->SetScrollRate(0, FromDIP(12));
        auto* controls_sizer = new wxBoxSizer(wxVERTICAL);
        m_color_trial = new ModelPreviewColorControls(m_controls_scroll);
        controls_sizer->Add(m_color_trial, 0, wxEXPAND);
        m_controls_scroll->SetSizer(controls_sizer);
        m_splitter->SplitHorizontally(m_preview_host, m_controls_scroll, FromDIP(360));
        m_splitter->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED, [this](wxSplitterEvent& event) {
            m_saved_splitter_sash = event.GetSashPosition();
            event.Skip();
        });
        sizer->Add(m_splitter, 1, wxEXPAND);
        m_region_prepare_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
        sizer->Add(m_region_prepare_status, 0, wxEXPAND | wxALL, FromDIP(6));
        m_region_prepare_status->Hide();
        m_region_prepare_timer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { finish_region_preparation(); }, m_region_prepare_timer.GetId());
        m_rotation_timer.SetOwner(this);
        Bind(wxEVT_TIMER,[this](wxTimerEvent&) {
            const auto now=std::chrono::steady_clock::now();
            const double seconds=std::chrono::duration<double>(now-m_rotation_tick).count();
            m_rotation_tick=now;
            if (!IsShownOnScreen() || !m_has_model || m_dragging || m_drawing_selection) {
                set_auto_rotation(false);return;
            }
            m_yaw=std::remainder(m_yaw+std::min(seconds,0.1)*0.35,6.283185307179586);
            m_canvas->Refresh(false);
        },m_rotation_timer.GetId());
        m_surface_timer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { finish_surface_selection(); }, m_surface_timer.GetId());
        m_semantic_timer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { finish_semantic_coloring(); }, m_semantic_timer.GetId());
        m_color_trial->Hide();
        m_controls_scroll->Hide();
        m_color_trial->on_project_colors_changed = [this] { synchronize_project_colors(); };
        m_color_trial->on_changed = [this] {
            m_trial_toggle_started = std::chrono::steady_clock::now();
            m_color_trial_enabled = m_color_trial->enabled();
            m_trial_palette = m_color_trial->colors();
            if (!m_suppress_semantic_change) update_semantic_coloring();
            m_canvas->Refresh(false);
            if (m_color_trial_changed) m_color_trial_changed(m_trial_palette.size());
            BOOST_LOG_TRIVIAL(info) << "AI color trial toggled: enabled=" << m_color_trial_enabled
                << ", palette=" << m_trial_palette.size() << ", geometry_reloaded=false";
        };
        trace_stage("color_controls");
        m_color_trial->on_region_changed = [this] {
            rebuild_semantic_preview_from_cached_result();
            m_canvas->Refresh(false);
        };
        SetSizer(sizer);

        m_context = wxGetApp().init_glcontext(*m_canvas);
        trace_stage("gl_context");
        m_canvas->Bind(wxEVT_PAINT, &ModelPreview3D::on_paint, this);
        m_canvas->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            m_canvas->Refresh(false);
            event.Skip();
        });
        m_canvas->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            set_auto_rotation(false);
            if (m_selection_enabled && !selection_busy() && !event.AltDown() &&
                m_selection_gesture != SelectionGesture::Orbit &&
                m_selection_gesture != SelectionGesture::Similar)
                set_selection_preview_suppressed(false);
            m_dragging = true;
            m_drag_moved = false;
            m_drag_start = event.GetPosition();
            m_last_mouse = event.GetPosition();
            m_drawing_selection = m_selection_enabled && !selection_busy() && !event.AltDown() &&
                m_selection_gesture != SelectionGesture::Orbit && m_selection_gesture != SelectionGesture::Similar;
            if (m_drawing_selection) {
                m_stroke.clear();
                m_stroke.emplace_back(event.GetX(), event.GetY());
                m_canvas->Refresh(false);
            }
            if (!m_canvas->HasCapture())
                m_canvas->CaptureMouse();
            m_canvas->SetFocus();
        });
        m_canvas->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            if (m_drawing_selection) {
                m_stroke.emplace_back(event.GetX(), event.GetY());
                submit_surface_selection();
            } else if (m_selection_enabled && !m_drag_moved && !event.AltDown() &&
                       m_selection_gesture == SelectionGesture::Similar) {
                set_selection_preview_suppressed(false);
                select_at(event.GetPosition());
            }
            finish_drag();
        });
        m_canvas->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
            if (!wxGetMouseState().LeftIsDown())
                finish_drag();
        });
        m_canvas->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (event.RightIsDown() && m_canvas->HasCapture()) {
                const wxPoint current = event.GetPosition();
                const double radius = std::max(0.001, 0.5 * m_bounds.size().norm());
                const double scale = 2.0 * fitted_half_height(m_canvas->GetClientSize().x, m_canvas->GetClientSize().y)
                    / std::max(1, m_canvas->GetClientSize().y) / radius;
                m_pan_x += (current.x - m_last_mouse.x) * scale;
                m_pan_y -= (current.y - m_last_mouse.y) * scale;
                m_last_mouse = current; m_canvas->Refresh(false); return;
            }
            if (!m_dragging || !event.LeftIsDown())
                return;
            const wxPoint current = event.GetPosition();
            if (m_drawing_selection) {
                if ((Vec2d(current.x, current.y) - m_stroke.back()).squaredNorm() >= 4.0)
                    m_stroke.emplace_back(current.x, current.y);
                m_canvas->Refresh(false);
                return;
            }
            if (!m_drag_moved) {
                const wxPoint distance = current - m_drag_start;
                m_drag_moved = distance.x * distance.x + distance.y * distance.y > FromDIP(3) * FromDIP(3);
            }
            if (!m_drag_moved)
                return;
            m_yaw += (current.x - m_last_mouse.x) * 0.012;
            m_pitch = std::clamp(
                m_pitch + (current.y - m_last_mouse.y) * 0.012,
                -1.5707963267948966, -0.15);
            m_last_mouse = current;
            m_canvas->Refresh(false);
        });
        m_canvas->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
            if (m_drawing_selection) return;
            const int delta = event.GetWheelDelta();
            if (delta == 0)
                return;
            const double turns = double(event.GetWheelRotation()) / double(delta);
            m_zoom = std::clamp(m_zoom * std::pow(1.15, turns), 0.45, 12.0);
            m_canvas->Refresh(false);
        });
        m_canvas->Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent& event) {
            set_auto_rotation(false);
            if (m_drawing_selection) finish_drag();
            m_last_mouse = event.GetPosition();
            if (!m_canvas->HasCapture()) m_canvas->CaptureMouse();
        });
        m_canvas->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) { finish_drag(); });
        m_canvas->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
            m_dragging = false; m_drawing_selection = false; m_stroke.clear(); m_canvas->Refresh(false);
        });
        m_canvas->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (!event.ControlDown() && (event.GetKeyCode() == 'F' || event.GetKeyCode() == 'f')) {
                focus_selection(); return;
            }
            if (m_beauty_view && m_beauty_history && event.ControlDown() &&
                (event.GetKeyCode() == 'Z' || event.GetKeyCode() == 'z')) {
                m_beauty_history(event.ShiftDown()); return;
            }
            if (!m_selection_enabled) {
                event.Skip();
                return;
            }
            if (event.ControlDown() && (event.GetKeyCode() == 'Z' || event.GetKeyCode() == 'z')) {
                if (event.ShiftDown()) redo_selection(); else undo_selection();
                return;
            }
            if (event.GetKeyCode() == WXK_ESCAPE) {
                if (m_drawing_selection || selection_busy()) {
                    cancel_surface_selection(); finish_drag(); notify_selection_changed();
                } else clear_selection();
                return;
            }
            event.Skip();
        });
    }

    ~ModelPreview3D() override {
        const bool region_was_running = m_region_preparation && !m_region_preparation->done.load();
        if (m_region_preparation) m_region_preparation->canceled = true;
        m_rotation_timer.Stop();
        m_semantic_timer.Stop();
        m_semantic_controller.reset();
        cancel_surface_selection();
        m_surface_timer.Stop();
        if (m_surface_worker.joinable()) m_surface_worker.join();
        m_region_prepare_timer.Stop();
        // The worker owns only CPU data. Cooperatively cancel before joining;
        // ordinary model switches invalidate the generation and never wait.
        if (m_region_prepare_worker.joinable()) {
            const auto started = std::chrono::steady_clock::now();
            m_region_prepare_worker.join();
            BOOST_LOG_TRIVIAL(info) << "AI selection preparation shutdown: wait_ms="
                << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
                << ", running_before_cancel=" << region_was_running;
        }
        clear();
    }

    bool retains_surface_attributes() const { return m_retain_surface_attributes; }

    struct ViewState { double yaw, pitch, zoom; BoundingBoxf3 bounds; double pan_x, pan_y; };
    ViewState view_state() const { return {m_yaw, m_pitch, m_zoom, m_bounds, m_pan_x, m_pan_y}; }
    void restore_view(const ViewState& state) {
        m_yaw = state.yaw; m_pitch = state.pitch; m_zoom = state.zoom; m_bounds = state.bounds;
        m_pan_x = state.pan_x; m_pan_y = state.pan_y;
        m_canvas->Refresh(false);
    }

    struct FileStamp {
        std::filesystem::file_time_type modified;
        uintmax_t bytes {0};
        bool valid {false};
    };

    // CPU-only data: prepare on a worker, then adopt on the GUI thread. Neither
    // OBJ parsing nor per-triangle color/normal preparation needs an OpenGL context.
    struct PreparedModel {
        GLModel::Geometry geometry;
        std::optional<GLModel::PreparedGeometry> render_geometry;

        // Freeze CPU geometry and its exact bounds before the GUI adopts it.
        // Synchronous consumers can still use the unprepared geometry.
        void prepare_render_geometry(const std::function<bool()>& canceled = {}, bool share_exact_vertices = false)
        {
            if (share_exact_vertices) {
                auto shared = ModelPreviewGeometry::share_exact_vertices(mesh, geometry, canceled);
                if (shared) {
                    auto frozen = GLModel::prepare_geometry(std::move(*shared), canceled);
                    render_geometry.emplace(std::move(frozen));
                    geometry = {}; // Release expanded storage on this worker.
                    return;
                }
            }
            render_geometry.emplace(GLModel::prepare_geometry(std::move(geometry), canceled));
        }
        const GLModel::Geometry& render_data() const
        { return render_geometry ? render_geometry->geometry() : geometry; }

        AI::ModelArtifactTextureSurface texture_surface;
        indexed_triangle_set mesh;
        std::vector<RGBA> vertex_colors;
        BoundingBoxf3 bounds;
        boost::filesystem::path path;
        FileStamp stamp;
        size_t triangles {0};
        size_t vertices {0};
        size_t colors {0};
        std::vector<PreviewPalette::Color> trial_palette;
        std::shared_ptr<const PreviewPalette::Histogram> trial_histogram;
        std::string geometry_id;
        std::string artifact_sha256;
        std::vector<Vec3f> corner_normals;
        FaceColorOverrides face_color_overrides;
        std::optional<SelectionState> selection;
        std::optional<ModelPreviewColorControls::State> color_trial;
        std::shared_ptr<const AI::SemanticColoring::MeshSnapshot> semantic_source;
        FaceColorOverrides saved_semantic_faces;
        SubfaceColorOverrides saved_semantic_subfaces;
        std::shared_ptr<const SemanticRegionEvidence> region_evidence;
        std::string region_runtime_identity, region_evidence_error;
        std::shared_ptr<const SecondaryRegionEvidence> secondary_region_evidence;
        std::string secondary_region_evidence_error;
        std::shared_ptr<const PortraitShapeDetails> shape_details;
        bool shapes_unlocked {false};
        nlohmann::json leaf_edits;
    };

    static bool prepare_model(const boost::filesystem::path& path, PreparedModel& prepared,
                              std::string& error, const FaceColorOverrides& explicit_overrides = {},
                              const boost::filesystem::path& metadata_path = {}, bool restore_saved_edits = true,
                              const std::function<bool()>& canceled = {}, ModelHistoryMetadata* history_metadata = nullptr)
    {
        if (history_metadata) history_metadata->clear();
        const auto started = std::chrono::steady_clock::now();
        const char* timing_env = std::getenv("ORCASLICER_MODEL_PREPARE_TIMING");
        const bool trace_prepare = timing_env && std::string(timing_env) == "1";
        auto trace_tick = started;
        const auto trace_stage = [&](const char* stage) {
            if (!trace_prepare) return;
            const auto now = std::chrono::steady_clock::now();
            BOOST_LOG_TRIVIAL(info) << "AI preview prepare stage: " << stage
                << " elapsed_ms=" << std::chrono::duration<double, std::milli>(now - trace_tick).count();
            trace_tick = std::chrono::steady_clock::now();
        };
        prepared = PreparedModel {};
        auto stop_if_canceled = [&] {
            if (!canceled || !canceled()) return false;
            // No partial CPU result can be installed or cached by a caller.
            prepared = PreparedModel {};
            if (history_metadata) history_metadata->clear();
            error = "Model preview loading canceled.";
            return true;
        };
        if (stop_if_canceled()) return false;
        const FileStamp initial_stamp = file_stamp(path);
        TriangleMesh mesh;
        ObjInfo obj_info;
        if (!AI::load_model_artifact(path, mesh, obj_info, error, canceled, &prepared.texture_surface) || mesh.empty()) {
            if (error == "Model loading canceled.") error = "Model preview loading canceled.";
            else stop_if_canceled();
            return false;
        }
        // Assimp's ReadFile remains indivisible. Stop before the remaining
        // fingerprints, normals and large render arrays once it returns.
        if (stop_if_canceled()) return false;
        trace_stage("decode");
        const indexed_triangle_set& its = mesh.its;
        prepared.geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(its);
        if (stop_if_canceled()) return false;
        trace_stage("geometry_fingerprint");
        nlohmann::json region_reference, secondary_region_reference, shape_reference;
        prepared.face_color_overrides = explicit_overrides;
        auto record = metadata_path;
        if (record.empty()) { record = path; record.replace_extension(".json"); }
        boost::system::error_code record_error;
        const auto record_bytes = boost::filesystem::file_size(record, record_error);
        boost::filesystem::ifstream record_stream(record);
        // Bound auxiliary state independently of the mesh, before JSON allocates.
        if (restore_saved_edits && record_stream && !record_error && record_bytes <= 128ULL * 1024 * 1024) {
            const bool frozen_history = history_metadata && history_metadata->read(record, canceled);
            if (stop_if_canceled()) return false;
            const auto fallback = frozen_history ? nlohmann::json() : nlohmann::json::parse(record_stream, nullptr, false);
            const auto& metadata = frozen_history ? history_metadata->restoration_fields() : fallback;
            std::string state_error;
            if (metadata.is_object()) {
                if (metadata.contains("semantic_region_evidence")) region_reference = metadata["semantic_region_evidence"];
                if (metadata.contains("secondary_region_evidence")) secondary_region_reference = metadata["secondary_region_evidence"];
                if (metadata.contains("portrait_shape_reference")) shape_reference = metadata["portrait_shape_reference"];
                if (metadata.contains("beauty_leaf_edit")) prepared.leaf_edits=metadata["beauty_leaf_edit"];
                prepared.shapes_unlocked = metadata.value("portrait_shapes_unlocked", false);
                if (metadata.contains("color_trial")) {
                    ModelPreviewColorControls::State trial;
                    if (AI::ColorTrialPersistence::decode(metadata["color_trial"], its.indices.size(), prepared.geometry_id, trial, state_error)) {
                        if (trial.source != 1 || trial.project_slot_identity.empty()) trial.source = 2;
                        trial.notice = _L("已恢复此版本保存的试色。");
                        prepared.color_trial = std::move(trial);
                    }
                }
                if (metadata.contains("local_selection")) {
                    SelectionState selection;
                    if (AI::SurfaceSelectionPersistence::decode(metadata["local_selection"], its.indices.size(), prepared.geometry_id, selection, state_error))
                        prepared.selection = std::move(selection);
                }
                if (explicit_overrides.empty() && metadata.contains("face_color_intent"))
                    AI::SurfaceSelectionPersistence::decode_colors(metadata["face_color_intent"], its.indices.size(), prepared.geometry_id, prepared.face_color_overrides, state_error);
                if (metadata.contains("semantic_result") && metadata["semantic_result"].is_object()) {
                    decode_semantic_result(metadata["semantic_result"],prepared.geometry_id,its.indices.size(),
                        prepared.saved_semantic_faces,prepared.saved_semantic_subfaces);
                }
            }
            if (!state_error.empty()) BOOST_LOG_TRIVIAL(warning) << "Saved local editing state ignored: " << state_error;
        }
        if (stop_if_canceled()) return false;
        trace_stage("saved_state");
        std::unordered_map<size_t, PreviewPalette::Color> locked_colors;
        for (const auto& item : prepared.face_color_overrides) {
            if (item.first >= its.indices.size()) { error = "Local face color no longer matches this model."; return false; }
            locked_colors[item.first] = item.second;
        }
        const bool has_vertex_colors = obj_info.vertex_colors.size() == its.vertices.size();
        const bool has_face_colors = obj_info.face_colors.size() == its.indices.size();
        if (has_vertex_colors || has_face_colors) {
            auto source = std::make_shared<AI::SemanticColoring::MeshSnapshot>();
            source->mesh = its;
            source->vertex_colors = obj_info.vertex_colors;
            source->face_colors = obj_info.face_colors;
            source->geometry_id = prepared.geometry_id;
            source->content_id = AI::SemanticColoring::content_fingerprint(*source);
            prepared.semantic_source = std::move(source);
        }
        if (stop_if_canceled()) return false;
        trace_stage("semantic_snapshot");
        const auto runtime = semantic_region_runtime_directory();
        const auto cache = std::filesystem::u8path(Slic3r::data_dir()) / "cache";
        if (!shape_reference.is_null()) try {
            if (shape_reference.at("model_sha256") != AI::model_artifact_sha256(path))
                throw std::invalid_argument("Portrait shape model hash changed.");
            prepared.shape_details = PortraitShapeCache::load(shape_reference, boost::filesystem::path(cache.native()),
                prepared.geometry_id, its.indices.size(), portrait_shape_runtime_fingerprint(), true);
        } catch (const std::exception& failure) {
            BOOST_LOG_TRIVIAL(warning) << "Portrait shape cache rejected: " << failure.what();
        }
        prepared.region_runtime_identity = semantic_region_runtime_identity(runtime);
        if (!region_reference.is_null()) {
            const auto expected_hash = region_reference.is_object() ? region_reference.find("model_sha256") : region_reference.end();
            if (expected_hash == region_reference.end() || !expected_hash->is_string() ||
                expected_hash->get<std::string>() != AI::model_artifact_sha256(path))
                prepared.region_evidence_error = "Region evidence model source hash mismatch";
            else prepared.region_evidence = SemanticRegionEvidenceCache::load(region_reference,
                boost::filesystem::path((cache / "beauty_regions").native()), prepared.geometry_id,
                its.indices.size(), prepared.region_runtime_identity, prepared.region_evidence_error);
        } else if (prepared.semantic_source)
            prepared.region_evidence = load_legacy_semantic_region_evidence(*prepared.semantic_source,
                runtime, cache / "portrait_semantics", prepared.region_runtime_identity, prepared.region_evidence_error);
        if (!secondary_region_reference.is_null()) {
            const auto expected_hash = secondary_region_reference.is_object() ? secondary_region_reference.find("model_sha256") : secondary_region_reference.end();
            const auto source_hash = AI::model_artifact_sha256(path);
            if (expected_hash != secondary_region_reference.end() && expected_hash->is_string() && expected_hash->get<std::string>() != source_hash)
                prepared.secondary_region_evidence_error = "Secondary evidence model source hash mismatch";
            else {
                prepared.secondary_region_evidence = SecondaryRegionEvidenceCache::load(secondary_region_reference,
                    boost::filesystem::path((cache / "beauty_secondary_regions").native()), prepared.geometry_id,
                    source_hash, its.indices.size(), prepared.region_runtime_identity, prepared.secondary_region_evidence_error);
            }
        } else if (prepared.region_evidence && prepared.semantic_source) {
            prepared.secondary_region_evidence = SecondaryRegionEvidence::from_primary(
                *prepared.region_evidence, AI::model_artifact_sha256(path), prepared.semantic_source->content_id,
                prepared.secondary_region_evidence_error, !region_reference.is_null());
        }
        const RGBA fallback {ColorRGBA::ORCA().r(), ColorRGBA::ORCA().g(), ColorRGBA::ORCA().b(), 1.0f};
        GLModel::Geometry geometry;
        geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2};
        geometry.reserve_vertices(its.indices.size() * 3);
        geometry.reserve_indices(its.indices.size() * 3);
        std::unordered_set<uint32_t> observed_colors;
        auto trial_histogram = std::make_shared<PreviewPalette::Histogram>();
        // Shared OBJ vertices often occur in six triangles. Pack/count their color
        // once while retaining all original face corners and the exact RGB8 output.
        std::vector<uint32_t> packed_vertex_colors;
        std::vector<uint8_t> counted_vertices;
        if (has_vertex_colors) {
            packed_vertex_colors.reserve(obj_info.vertex_colors.size());
            counted_vertices.assign(obj_info.vertex_colors.size(), 0);
            std::array<float, 3> previous_rgb {};
            uint32_t previous_packed = 0;
            bool previous_rgb_valid = false;
            for (const RGBA& color : obj_info.vertex_colors) {
                // Repeated face-corner colors need the RGB8 conversion once.
                // Alpha stays in the original per-vertex array used by rendering.
                if (!previous_rgb_valid || std::memcmp(color.data(), previous_rgb.data(), sizeof(previous_rgb)) != 0) {
                    previous_packed = preview_rgb8(color[0], color[1], color[2]);
                    std::copy_n(color.data(), previous_rgb.size(), previous_rgb.begin());
                    previous_rgb_valid = std::all_of(previous_rgb.begin(), previous_rgb.end(), [](float value) { return std::isfinite(value); });
                }
                packed_vertex_colors.push_back(previous_packed);
            }
        }
        if (stop_if_canceled()) return false;
        trace_stage("packed_colors");
        std::function<void(const char*)> normal_trace;
        auto normal_tick = trace_prepare ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
        if (trace_prepare) normal_trace = [&](const char* stage) {
            const auto now = std::chrono::steady_clock::now();
            BOOST_LOG_TRIVIAL(info) << "AI preview normals stage: " << stage
                << " elapsed_ms=" << std::chrono::duration<double, std::milli>(now - normal_tick).count();
            normal_tick = std::chrono::steady_clock::now();
        };
        const auto corner_normals = ModelPreviewNormals::corner_normals(its, 45.f, normal_trace);
        if (stop_if_canceled()) return false;
        trace_stage("normals");
        for (size_t face_index = 0; face_index < its.indices.size(); ++face_index) {
            if ((face_index & 4095) == 0 && stop_if_canceled()) return false;
            const auto lock = locked_colors.find(face_index);
            const auto& indices = its.indices[face_index];
            const Vec3f& a = its.vertices[indices[0]];
            const Vec3f& b = its.vertices[indices[1]];
            const Vec3f& c = its.vertices[indices[2]];
            const double area_weight = (b - a).cross(c - a).norm();
            const auto base = static_cast<unsigned int>(geometry.vertices_count());
            const RGBA uniform_color = has_face_colors ? obj_info.face_colors[face_index] : fallback;
            const uint32_t uniform_packed = has_vertex_colors ? 0 :
                preview_rgb8(uniform_color[0], uniform_color[1], uniform_color[2]);
            if (!has_vertex_colors)
                observed_colors.insert(uniform_packed);
            for (size_t corner = 0; corner < 3; ++corner) {
                const auto vertex_index = indices[corner];
                const RGBA& color = has_vertex_colors ? obj_info.vertex_colors[vertex_index] : uniform_color;
                const uint32_t packed = has_vertex_colors ? packed_vertex_colors[vertex_index] : uniform_packed;
                trial_histogram->add(packed, area_weight);
                if (has_vertex_colors && !counted_vertices[vertex_index]) {
                    observed_colors.insert(packed);
                    counted_vertices[vertex_index] = 1;
                }
                const uint32_t shown = lock == locked_colors.end() ? packed
                    : preview_rgb8(lock->second[0], lock->second[1], lock->second[2]);
                geometry.add_vertex(its.vertices[indices[corner]], corner_normals[face_index * 3 + corner],
                    Vec2f(float(shown), lock == locked_colors.end() ? color[3] : -1.0f));
            }
            geometry.add_triangle(base, base + 1, base + 2);
        }
        if (geometry.is_empty()) {
            error = "The OBJ contains no renderable triangles.";
            return false;
        }

        if (stop_if_canceled()) return false;
        trace_stage("render");
        prepared.bounds = mesh.bounding_box();
        prepared.triangles = its.indices.size();
        prepared.vertices = its.vertices.size();
        prepared.colors = observed_colors.size();
        const auto palette_started = std::chrono::steady_clock::now();
        const size_t preview_color_count = std::clamp(prepared.colors, size_t(1), PreviewPalette::max_preview_colors);
        prepared.trial_palette = trial_histogram->palette(preview_color_count, {}, true);
        prepared.trial_histogram = std::move(trial_histogram);
        if (stop_if_canceled()) return false;
        trace_stage("palette");
        BOOST_LOG_TRIVIAL(info) << "AI color trial palette: colors=" << prepared.trial_palette.size()
            << ", clustering_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - palette_started).count();
        prepared.geometry = std::move(geometry);
        if (!prepared.texture_surface.faces.empty()) prepared.corner_normals = corner_normals;
        prepared.path = path;
        try { prepared.artifact_sha256 = AI::model_artifact_sha256(path); }
        catch (...) { /* A cache failure must not reject a valid model. */ }
        prepared.stamp = file_stamp(path);
        if (!same_stamp(initial_stamp, prepared.stamp))
            prepared.stamp.valid = false;
        if (has_vertex_colors || has_face_colors || !prepared.texture_surface.faces.empty() || !restore_saved_edits) {
            prepared.mesh = std::move(mesh.its);
            prepared.vertex_colors = std::move(obj_info.vertex_colors);
        }
        trace_stage("publish");
        BOOST_LOG_TRIVIAL(info) << "AI model preview CPU prepare: triangles=" << prepared.triangles
            << ", elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
        return true;
    }

    bool load_prepared_model(PreparedModel&& prepared, const std::vector<std::string>& palette,
                             size_t& triangle_count, Vec3d& dimensions, size_t& color_count, std::string& error)
    {
        if (m_canvas == nullptr || m_context == nullptr || !m_context->IsOK() || !m_canvas->SetCurrent(*m_context)) {
            error = "OpenGL preview context is unavailable.";
            return false;
        }
        const auto& geometry = prepared.render_data();
        if (geometry.is_empty()) {
            error = "The OBJ contains no renderable triangles.";
            return false;
        }
        std::vector<float> surface_vertices;
        std::vector<Vec2f> surface_colors;
        if (m_retain_surface_attributes) {
            surface_vertices = geometry.vertices;
            surface_colors.reserve(geometry.vertices_count());
            for (size_t v = 0; v < geometry.vertices_count(); ++v)
                surface_colors.push_back(geometry.extract_tex_coord_2(v));
        }
        // Async model loading can upload textures before the first paint initializes GLAD.
        if (!wxGetApp().init_opengl()) {
            error = "OpenGL preview initialization failed.";
            return false;
        }
        std::unique_ptr<ModelPreviewTexture> texture_model;
        auto model = std::make_unique<GLModel>();
        try {
            if (!prepared.texture_surface.faces.empty())
                texture_model = std::make_unique<ModelPreviewTexture>(prepared.texture_surface,
                    prepared.mesh, prepared.corner_normals, prepared.face_color_overrides);
            if (prepared.render_geometry)
                model->init_from(std::move(*prepared.render_geometry));
            else
                model->init_from(std::move(prepared.geometry));
        } catch (const std::exception& failure) { error = failure.what(); return false; }
        cache_current_preview();
        clear_current_preview();
        m_surface_vertices = std::move(surface_vertices);
        m_original_surface_colors = std::move(surface_colors);
        m_texture_model = std::move(texture_model);
        m_models.emplace_back(std::move(model));
        m_bounds = prepared.bounds;
        m_pending_mesh = std::move(prepared.mesh);
        m_pending_vertex_colors = std::move(prepared.vertex_colors);
        m_geometry_id = std::move(prepared.geometry_id);
        m_face_color_overrides = std::move(prepared.face_color_overrides);
        m_semantic_source = std::move(prepared.semantic_source);
        m_portrait_shapes = std::move(prepared.shape_details);
        m_shapes_unlocked = prepared.shapes_unlocked;
        if (m_portrait_shapes && m_portrait_shapes->locks.leaf_domain) m_pending_leaf_edits=std::move(prepared.leaf_edits);
        m_region_runtime_identity = std::move(prepared.region_runtime_identity);
        m_region_evidence = std::move(prepared.region_evidence);
        m_region_evidence_error = std::move(prepared.region_evidence_error);
        m_secondary_region_evidence = std::move(prepared.secondary_region_evidence);
        m_secondary_region_evidence_error = std::move(prepared.secondary_region_evidence_error);
        m_pending_selection = std::move(prepared.selection);
        m_model_path = std::move(prepared.path);
        m_model_stamp = prepared.stamp;
        m_triangle_count = prepared.triangles;
        m_vertex_count = prepared.vertices;
        m_color_count = prepared.colors;
        m_trial_palette = std::move(prepared.trial_palette);
        m_trial_histogram = std::move(prepared.trial_histogram);
        triangle_count = m_triangle_count;
        dimensions = m_model_dimensions = prepared.bounds.size().cast<double>();
        color_count = m_color_count;
        m_palette = palette;
        m_has_model = true;
        m_color_trial->load(m_trial_histogram, m_trial_palette);
        if (prepared.color_trial) {
            m_suppress_semantic_change = m_beauty_view || !prepared.saved_semantic_faces.empty() || !prepared.saved_semantic_subfaces.empty();
            m_color_trial->restore(*prepared.color_trial);
            m_suppress_semantic_change = false;
        }
        if (!prepared.saved_semantic_faces.empty() || !prepared.saved_semantic_subfaces.empty())
            set_saved_semantic_result(std::move(prepared.saved_semantic_faces), std::move(prepared.saved_semantic_subfaces));
        if (prepared.color_trial) synchronize_project_bound_semantics(*prepared.color_trial, m_color_trial->state());
        m_paint_diagnostics_logged = false;
        m_render_diagnostics_logged = false;
        default_view();
        m_pending_library_thumbnail_sha = std::move(prepared.artifact_sha256);
        notify_selection_changed();
        return true;
    }

    bool try_load_cached_model(const boost::filesystem::path& path, const std::vector<std::string>& palette,
                               size_t& triangle_count, Vec3d& dimensions, size_t& color_count)
    {
        if (!m_cached_preview || m_cached_preview->path != path ||
            !same_stamp(m_cached_preview->stamp, file_stamp(path)) ||
            m_context == nullptr || !m_context->IsOK() || !m_canvas->SetCurrent(*m_context))
            return false;
        auto cached = std::move(m_cached_preview);
        cache_current_preview();
        clear_current_preview();
        m_models = std::move(cached->models);
        m_texture_model = std::move(cached->texture_model);
        m_manual_color_model = std::move(cached->manual_color_model);
        m_pending_mesh = std::move(cached->mesh);
        m_pending_vertex_colors = std::move(cached->vertex_colors);
        m_geometry_id = std::move(cached->geometry_id);
        m_face_color_overrides = std::move(cached->face_color_overrides);
        m_semantic_source = std::move(cached->semantic_source);
        m_portrait_shapes = std::move(cached->shape_details);
        m_shapes_unlocked = cached->shapes_unlocked;
        if (m_portrait_shapes && m_portrait_shapes->locks.leaf_domain) m_pending_leaf_edits=std::move(cached->leaf_edits);
        m_region_runtime_identity = semantic_region_runtime_identity(semantic_region_runtime_directory());
        m_region_evidence_error = std::move(cached->region_evidence_error);
        m_region_evidence = std::move(cached->region_evidence);
        m_secondary_region_evidence_error = std::move(cached->secondary_region_evidence_error);
        m_secondary_region_evidence = std::move(cached->secondary_region_evidence);
        const auto saved_faces = std::move(cached->saved_semantic_faces);
        const auto saved_subfaces = std::move(cached->saved_semantic_subfaces);
        m_pending_selection = std::move(cached->selection);
        m_model_path = std::move(cached->path);
        m_model_stamp = cached->stamp;
        m_bounds = cached->bounds;
        m_triangle_count = triangle_count = cached->triangles;
        m_vertex_count = cached->vertices;
        if (m_region_evidence) restore_semantic_region_evidence(m_region_evidence);
        m_color_count = color_count = cached->colors;
        m_trial_palette = std::move(cached->trial_palette);
        m_trial_histogram = std::move(cached->trial_histogram);
        dimensions = m_model_dimensions = cached->dimensions;
        m_palette = palette;
        m_has_model = true;
        m_color_trial->load(m_trial_histogram, m_trial_palette);
        if (cached->color_trial) {
            if (m_beauty_view || !saved_faces.empty() || !saved_subfaces.empty())
                restore_color_trial_without_recognition(*cached->color_trial);
            else m_color_trial->restore(*cached->color_trial);
        }
        if (!saved_faces.empty() || !saved_subfaces.empty())
            set_saved_semantic_result(saved_faces, saved_subfaces);
        if (cached->color_trial) synchronize_project_bound_semantics(*cached->color_trial, m_color_trial->state());
        m_paint_diagnostics_logged = false;
        m_render_diagnostics_logged = false;
        default_view();
        notify_selection_changed();
        BOOST_LOG_TRIVIAL(info) << "AI model preview cache hit: triangles=" << triangle_count;
        return true;
    }

    bool try_use_current_model(const boost::filesystem::path& path, const std::vector<std::string>& palette,
                               size_t& triangle_count, Vec3d& dimensions, size_t& color_count) const
    {
        if (!m_has_model || path != m_model_path || palette != m_palette ||
            !same_stamp(m_model_stamp, file_stamp(path))) return false;
        triangle_count = m_triangle_count; color_count = m_color_count;
        dimensions = m_model_dimensions;
        return true;
    }

    bool load_model(const boost::filesystem::path& path, const std::vector<std::string>& palette,
                    size_t& triangle_count, Vec3d& dimensions, size_t& color_count, std::string& error,
                    const FaceColorOverrides& explicit_overrides = {},
                    const boost::filesystem::path& metadata_path = {})
    {
        // Accepting an already displayed preview should not reparse the OBJ or
        // rebuild GPU buffers. The file stamp still invalidates external edits.
        if (try_use_current_model(path, palette, triangle_count, dimensions, color_count))
            return true;
        if (try_load_cached_model(path, palette, triangle_count, dimensions, color_count))
            return true;
        PreparedModel prepared;
        return prepare_model(path, prepared, error, explicit_overrides, metadata_path) && load_prepared_model(std::move(prepared), palette,
            triangle_count, dimensions, color_count, error);
    }

    void clear()
    {
        set_auto_rotation(false);
        clear_current_preview();
        m_cached_preview.reset();
    }

private:
    void clear_current_preview()
    {
        cancel_surface_selection();
        if (m_semantic_controller) m_semantic_controller->cancel();
        m_semantic_source.reset(); m_automatic_face_colors.clear(); m_automatic_subface_colors.clear();
        m_portrait_shapes.reset(); m_shapes_unlocked = false; m_leaf_editing.reset();
        m_manual_leaf_colors.clear(); m_pending_leaf_edits=nullptr;
        m_saved_semantic_faces.clear(); m_saved_semantic_subfaces.clear();
        m_semantic_analysis.reset(); m_semantic_ready = false;
        m_semantic_error.clear();
        m_region_evidence.reset(); m_region_runtime_identity.clear(); m_region_evidence_error.clear();
        m_secondary_region_evidence.reset(); m_secondary_region_evidence_error.clear();
        m_protected_faces.clear();
        m_foreground_faces.clear(); m_selection_domain.clear();
        m_pending_selection.reset(); m_geometry_id.clear(); m_face_color_overrides.clear();
        m_selection_redo.clear();
        m_stroke.clear();
        m_drawing_selection = false;
        ++m_region_generation;
        if (m_region_preparation) m_region_preparation->canceled = true;
        m_deferred_selection = {};
        m_region_prepare_failed = false;
        m_region_prepare_status->Hide();
        if (m_context != nullptr && m_canvas != nullptr)
            m_canvas->SetCurrent(*m_context);
        m_models.clear();
        m_texture_model.reset();
        m_manual_color_model.reset(); m_manual_corner_normals.clear();
        m_surface_vertices.clear();
        m_original_surface_colors.clear();
        m_exact_surface_display = false;
        m_workbench_grid.reset();
        m_workbench_grid_geometry.clear();
        m_semantic_model.reset();
        m_partition_model.reset();
        m_beauty_pick = {};
        m_selection_model.reset();
        m_protection_model.reset();
        m_region_editor = std::make_shared<AI::VertexColorRegionEditor>();
        m_pending_mesh = indexed_triangle_set {};
        m_pending_vertex_colors = std::vector<RGBA> {};
        m_selection_history.clear();
        m_palette.clear();
        m_has_model = false;
        m_vertex_count = 0;
        m_color_trial_enabled = false;
        m_trial_palette.clear();
        m_trial_histogram.reset();
        m_trial_toggle_started.reset();
        if (m_color_trial) m_color_trial->clear();
        m_selection_enabled = false;
        m_model_path.clear();
        m_pending_library_thumbnail_sha.clear();
        m_model_stamp = FileStamp {};
        if (m_canvas != nullptr)
            m_canvas->SetCursor(wxCursor(wxCURSOR_ARROW));
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
    }

public:
    void set_library_thumbnail_root(const boost::filesystem::path& root) { m_library_thumbnail_root = root; }
    void set_preview_background(const wxColour& color)
    {
        m_preview_background = color;
        SetBackgroundColour(color);
        if (m_canvas != nullptr) m_canvas->Refresh(false);
    }

    // Native GL dimensions, rendering and hit testing share this safe area.
    // Reserving sibling controls changes layout, never the stored view/edit state.
    void set_overlay_insets(int top, int bottom)
    {
        const wxSize top_size(0, std::max(0, top));
        const wxSize bottom_size(0, std::max(0, bottom));
        if (m_overlay_top->GetMinSize() == top_size && m_overlay_bottom->GetMinSize() == bottom_size) return;
        m_overlay_top->SetMinSize(top_size);
        m_overlay_bottom->SetMinSize(bottom_size);
        Layout();
        m_canvas->Refresh(false);
    }

    void reset_view()
    {
        set_auto_rotation(false);
        m_pan_x = m_pan_y = 0.0;
        m_yaw = -0.65;
        m_pitch = -1.05;
        m_zoom = 1.0;
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
    }

    // The portrait front camera sees the Y-Z projection. Thin X-Z / X-Y
    // assets would collapse to a line, so their initial view uses the existing
    // Z-up three-dimensional angle. Explicit front-view actions stay literal.
    static std::pair<double, double> initial_view_angles(const Vec3d& dimensions)
    {
        const double extent = dimensions.maxCoeff();
        if (extent > 0.0 &&
            std::min(dimensions.y(), dimensions.z()) < extent * 0.02)
            return {-0.65, -1.05};
        return {-1.5707963267948966, -1.5707963267948966};
    }

    void default_view()
    {
        set_auto_rotation(false);
        m_pan_x = m_pan_y = 0.0;
        const auto angles = initial_view_angles(m_bounds.size().cast<double>());
        m_yaw = angles.first;
        m_pitch = angles.second;
        m_zoom = 1.0;
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
    }

    void front_view()
    {
        set_auto_rotation(false);
        m_pan_x = m_pan_y = 0.0;
        // Generated portrait OBJ files are Z-up and face +X. Rotate +X toward
        // the orthographic camera so identity can be compared without asking
        // the user to find a precise angle by dragging a two-million-face mesh.
        m_yaw = -1.5707963267948966;
        m_pitch = -1.5707963267948966;
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
        if (!enabled) m_deferred_selection = {};
        const bool selection_enabled = enabled && region_editing_ready();
        if (selection_enabled == m_selection_enabled)
            return;
        m_selection_enabled = selection_enabled;
        if (m_canvas != nullptr)
            m_canvas->SetCursor(wxCursor(m_selection_enabled ? wxCURSOR_CROSS : wxCURSOR_ARROW));
        if (!m_selection_enabled) {
            m_deferred_selection = {};
            cancel_surface_selection();
            if (!m_region_prepare_failed) m_region_prepare_status->Hide();
        } else ensure_region_editor();
    }

    void set_selection_gesture(SelectionGesture gesture, double radius_pixels = 18.0)
    {
        m_selection_gesture = gesture;
        m_brush_radius = std::clamp(radius_pixels, 3.0, 100.0);
    }
    SelectionState selection_state() const {
        if (!m_region_editor->ready() && m_pending_selection) return *m_pending_selection;
        return {m_region_editor->selected_faces(), m_protected_faces, m_foreground_faces, m_selection_domain};
    }
    const std::string& geometry_id() const { return m_geometry_id; }
    bool display_surface_colors(const std::vector<PreviewPalette::Color>& colors,
                                const std::string& geometry_id, std::string& error) {
        if (!m_retain_surface_attributes || m_models.size() != 1 || geometry_id != m_geometry_id ||
            m_surface_vertices.size() != m_triangle_count * 3 * 8 ||
            m_original_surface_colors.size() != m_triangle_count * 3 ||
            (!colors.empty() && colors.size() != m_triangle_count)) {
            error = "Surface preview does not match this model geometry."; return false;
        }
        for (const auto& color : colors) for (float value : color)
            if (!std::isfinite(value) || value < 0 || value > 1) {
                error = "Invalid surface preview color."; return false;
            }
        if (!m_context || !m_canvas->SetCurrent(*m_context)) {
            error = "OpenGL preview is unavailable."; return false;
        }
        for (size_t face = 0; face < m_triangle_count; ++face) for (size_t corner = 0; corner < 3; ++corner) {
            const size_t vertex = face * 3 + corner;
            m_surface_vertices[vertex * 8 + 6] = colors.empty() ? m_original_surface_colors[vertex].x() :
                float(preview_rgb8(colors[face][0], colors[face][1], colors[face][2]));
            m_surface_vertices[vertex * 8 + 7] = colors.empty() ? m_original_surface_colors[vertex].y() : -1.f;
        }
        if (!m_models.front()->update_vertex_attributes(m_surface_vertices)) {
            error = "Unable to update surface colors."; return false;
        }
        m_exact_surface_display = !colors.empty();
        m_color_trial_enabled = false;
        m_canvas->Refresh(false);
        return true;
    }
    // Beauty adapters consume the already prepared immutable surface editor;
    // they never mutate the preview mesh or trigger semantic recognition.
    std::shared_ptr<const AI::VertexColorRegionEditor> beauty_editor() const {
        return m_region_editor->ready() ? m_region_editor : nullptr;
    }
    void prepare_beauty_editor() { ensure_region_editor(); }
    bool leaf_editing() const { return bool(m_leaf_editing); }
    uint64_t leaf_edit_revision() const { return m_leaf_edit_revision; }
    nlohmann::json leaf_edit_metadata() const {
        if (!m_leaf_editing || !m_portrait_shapes) return m_pending_leaf_edits;
        return AI::BeautyLeafEdits::capture(*m_leaf_editing,m_portrait_shapes->locks,m_manual_leaf_colors,selection_state())
            .encode(*m_leaf_editing,m_portrait_shapes->locks);
    }
    bool restore_leaf_edits(const nlohmann::json& value) {
        ++m_leaf_edit_revision;
        if (value.is_null() || value.empty()) { m_manual_leaf_colors.clear(); m_pending_leaf_edits=nullptr; refresh_leaf_colors(); return true; }
        if (!m_leaf_editing) { m_pending_leaf_edits=value; ensure_region_editor(); return true; }
        try {
            const auto state=AI::BeautyLeafEdits::decode(value,*m_leaf_editing,m_portrait_shapes->locks);
            m_manual_leaf_colors=state.colors; m_pending_leaf_edits=nullptr;
            restore_selection_state(state.selection); refresh_leaf_colors(); return true;
        } catch (const std::exception& error) {
            BOOST_LOG_TRIVIAL(warning)<<"Leaf editing state rejected: "<<error.what();
            m_manual_leaf_colors.clear(); m_pending_leaf_edits=nullptr; refresh_leaf_colors(); return false;
        }
    }
    bool paint_selected_leaves(const RGBA& color) {
        if (!m_leaf_editing || !m_region_editor->ready()) return false;
        auto state=selection_state(); constrain_shape_selection(state);
        auto colors=m_manual_leaf_colors; size_t painted=0;
        for (size_t i=0;i<state.selected.size();++i) if (state.selected[i] &&
            (i>=state.protected_faces.size() || !state.protected_faces[i])) {
            colors[m_leaf_editing->keys[i]]={color[0],color[1],color[2]}; ++painted;
        }
        if (!painted) return false;
        AI::BeautyLeafEdits::capture(*m_leaf_editing,m_portrait_shapes->locks,colors,state);
        m_manual_leaf_colors=std::move(colors); ++m_leaf_edit_revision; refresh_leaf_colors(); return true;
    }
    void refresh_leaf_colors() {
        if (!m_leaf_editing || !m_semantic_source || !m_context || !m_canvas->SetCurrent(*m_context)) return;
        auto geometry=build_semantic_colored_geometry(*m_semantic_source,import_face_color_overrides(true),import_subface_color_overrides(true));
        if (geometry.is_empty()) return;
        auto model=std::make_unique<GLModel>(); model->init_from(std::move(geometry)); m_semantic_model=std::move(model);
        m_semantic_ready=true; m_canvas->Refresh(false);
    }
    size_t editing_face_count() const { return m_leaf_editing ? m_leaf_editing->keys.size() : m_triangle_count; }
    std::string beauty_geometry_id() const { return m_leaf_editing ? m_leaf_editing->surface->geometry_id : m_geometry_id; }
    std::shared_ptr<const AI::BeautySurface> beauty_leaf_surface() const { return m_leaf_editing ? m_leaf_editing->surface : nullptr; }
    std::vector<PortraitShapeDetails::Detail> portrait_detail_catalog(const std::string& parent = {}) const {
        return m_portrait_shapes ? m_portrait_shapes->catalog(parent,m_leaf_editing.get()) : std::vector<PortraitShapeDetails::Detail>{};
    }
    const AI::ShapeLockSet* active_editing_shape_locks() const {
        return active_shape_locks() ? (m_leaf_editing ? &m_leaf_editing->editing_locks : active_shape_locks()) : nullptr;
    }
    bool semantic_regions_ready() const {
        return m_region_evidence && m_region_evidence->compatible(
            m_geometry_id, m_triangle_count, m_region_runtime_identity);
    }
    std::shared_ptr<const SemanticRegionEvidence> semantic_region_evidence() const { return m_region_evidence; }
    const std::string& semantic_region_evidence_error() const { return m_region_evidence_error; }
    bool restore_semantic_region_evidence(std::shared_ptr<const SemanticRegionEvidence> evidence,
                                         const std::string& previous_error = {}) {
        if (!evidence) {
            m_region_evidence.reset();
            m_region_evidence_error = previous_error;
            return false;
        }
        if (!evidence->compatible(m_geometry_id, m_triangle_count, m_region_runtime_identity)) {
            m_region_evidence.reset();
            m_region_evidence_error = "Region evidence geometry, face order or runtime changed";
            return false;
        }
        m_region_evidence = std::move(evidence);
        m_region_evidence_error.clear();
        return true;
    }
    wxString semantic_region_status() const {
        if (m_semantic_controller && m_semantic_controller->busy()) return _L("正在识别人像区域，请等待完成或取消。");
        if (semantic_regions_ready()) return _L("区域识别缓存可用；自动匹配不会重新识别。");
        if (!m_region_evidence_error.empty()) return _L("区域证据失效或恢复失败，请点击“重新优化人像区域”：") +
            wxString::FromUTF8(m_region_evidence_error);
        if (!m_semantic_error.empty()) return _L("人像识别未完成，请点击“重新优化人像区域”重试：") + m_semantic_error;
        return _L("当前模型尚无可靠区域标签，请先点击“重新优化人像区域”完成识别。");
    }
    size_t semantic_selection_protected_count() const { return m_semantic_selection_protected; }
    size_t semantic_selection_low_confidence_count() const { return m_semantic_selection_low_confidence; }
    bool secondary_regions_ready() const {
        return m_secondary_region_evidence && m_secondary_region_evidence->compatible(
            m_geometry_id, AI::model_artifact_sha256(m_model_path), m_triangle_count, m_region_runtime_identity);
    }
    std::vector<std::string> secondary_region_details(const std::string& parent = {}) const {
        auto result = secondary_regions_ready() ? m_secondary_region_evidence->detail_ids(parent) : std::vector<std::string> {};
        result.erase(std::remove_if(result.begin(), result.end(), [](const auto& id) {
            return id == "iris" || id == "imouth" || AI::ShapeLockSet::supported_label(id);
        }), result.end());
        for (const auto& detail : portrait_detail_catalog(parent)) result.push_back(detail.key);
        return result;
    }
    std::shared_ptr<const PortraitShapeDetails> portrait_shape_details() const { return m_portrait_shapes; }
    bool portrait_shapes_unlocked() const { return m_shapes_unlocked; }
    void restore_portrait_shapes(std::shared_ptr<const PortraitShapeDetails> details, bool unlocked = false) {
        const bool changed_domain = m_portrait_shapes && details &&
            (m_portrait_shapes->locks.encode() != details->locks.encode() ||
             (m_portrait_shapes->surface_ownership ? m_portrait_shapes->surface_ownership->fingerprint() : std::string()) !=
             (details->surface_ownership ? details->surface_ownership->fingerprint() : std::string()));
        if (m_leaf_editing && (!details || changed_domain)) {
            cancel_surface_selection();
            m_region_editor = m_leaf_editing->canonical_editor;
            m_leaf_editing.reset();
            m_protected_faces.clear(); m_foreground_faces.clear(); m_selection_domain.clear();
            m_selection_history.clear(); m_selection_redo.clear();
            m_manual_leaf_colors.clear(); m_pending_leaf_edits=nullptr;
        }
        m_portrait_shapes = details && details->compatible(m_geometry_id, m_triangle_count) ? std::move(details) : nullptr;
        m_shapes_unlocked = m_portrait_shapes && unlocked;
        if (m_portrait_shapes && m_portrait_shapes->locks.leaf_domain && !m_leaf_editing && m_region_editor->ready()) {
            cancel_surface_selection();
            m_pending_mesh = m_region_editor->mesh(); m_pending_vertex_colors = m_region_editor->vertex_colors();
            m_pending_selection = selection_state();
            m_region_editor = std::make_shared<AI::VertexColorRegionEditor>();
            ensure_region_editor();
        }
    }
    const AI::ShapeLockSet* active_shape_locks() const {
        return m_portrait_shapes && !m_shapes_unlocked && !m_portrait_shapes->locks.empty() ? &m_portrait_shapes->locks : nullptr;
    }
    nlohmann::json portrait_shape_metadata(const boost::filesystem::path& model = {}) const {
        if (!m_portrait_shapes || !m_portrait_shapes->compatible(m_geometry_id, m_triangle_count)) return nlohmann::json();
        auto reference = PortraitShapeCache::save(*m_portrait_shapes, boost::filesystem::path(Slic3r::data_dir()) / "cache");
        reference["model_sha256"] = AI::model_artifact_sha256(model.empty() ? m_model_path : model);
        return reference;
    }
    void constrain_shape_selection(SelectionState& state, bool parent_selection = false) const {
        if (m_leaf_editing && active_shape_locks()) { m_leaf_editing->constrain(state,parent_selection); return; }
        if (!active_shape_locks() || state.selected.size() != m_triangle_count) return;
        const auto owned = m_portrait_shapes->ownership();
        bool has_locked = false;
        if (!parent_selection) for (size_t f = 0; f < owned.size(); ++f) if (state.selected[f] && owned[f]) { has_locked = true; break; }
        for (size_t f = 0; f < owned.size(); ++f)
            if ((parent_selection && owned[f]) || (has_locked && !owned[f])) {
                state.selected[f] = 0;
                if (f < state.foreground.size()) state.foreground[f] = 0;
                if (f < state.domain.size()) state.domain[f] = 0;
            }
    }
    std::shared_ptr<const SecondaryRegionEvidence> secondary_region_evidence() const { return m_secondary_region_evidence; }
    const std::string& secondary_region_evidence_error() const { return m_secondary_region_evidence_error; }
    bool restore_secondary_region_evidence(std::shared_ptr<const SecondaryRegionEvidence> evidence,
                                           const std::string& previous_error = {}) {
        if (!evidence) {
            m_secondary_region_evidence.reset();
            m_secondary_region_evidence_error = previous_error;
            return false;
        }
        if (!evidence->valid() || !evidence->compatible(m_geometry_id,
                AI::model_artifact_sha256(m_model_path), m_triangle_count, m_region_runtime_identity)) {
            m_secondary_region_evidence.reset();
            m_secondary_region_evidence_error = "Secondary evidence identity mismatch";
            return false;
        }
        m_secondary_region_evidence = std::move(evidence);
        m_secondary_region_evidence_error.clear();
        return true;
    }
    bool transfer_secondary_region_evidence(std::shared_ptr<const SecondaryRegionEvidence> original,
                                            const std::string& original_source_sha256) {
        std::string error;
        auto transferred = original ? SecondaryRegionEvidence::for_derived_model(*original,
            original_source_sha256, m_geometry_id, m_triangle_count, m_region_runtime_identity,
            AI::model_artifact_sha256(m_model_path), error) : nullptr;
        return restore_secondary_region_evidence(std::move(transferred), error);
    }
    bool import_secondary_region_evidence(const boost::filesystem::path& evidence_path, std::string& error) {
        error.clear();
        if (!m_has_model || m_model_path.empty()) { error = "No model is loaded"; return false; }
        boost::system::error_code file_error;
        const auto bytes = boost::filesystem::file_size(evidence_path, file_error);
        if (file_error || !boost::filesystem::is_regular_file(evidence_path, file_error) ||
            file_error || bytes == 0 || bytes > SecondaryRegionEvidenceCache::maximum_bytes) {
            error = "Secondary evidence file is missing or too large"; return false;
        }
        const auto source_hash = AI::model_artifact_sha256(m_model_path);
        if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(source_hash)) {
            error = "Current model source hash is unavailable"; return false;
        }
        try {
            boost::filesystem::ifstream input(evidence_path, std::ios::binary);
            const auto document = nlohmann::json::parse(input, nullptr, false);
            if (document.is_discarded()) { error = "Secondary evidence JSON is invalid"; return false; }
            auto loaded = SecondaryRegionEvidence::decode(document, m_geometry_id, source_hash,
                m_triangle_count, m_region_runtime_identity, error);
            if (!loaded) return false;
            std::string cache_error;
            if (SecondaryRegionEvidenceCache::save(*loaded,
                    boost::filesystem::path(Slic3r::data_dir()) / "cache" / "beauty_secondary_regions",
                    cache_error).empty()) {
                error = cache_error.empty() ? "Secondary evidence cache write failed" : cache_error;
                return false;
            }
            m_secondary_region_evidence = std::move(loaded);
            m_secondary_region_evidence_error.clear();
            return true;
        } catch (const std::exception& exception) {
            error = exception.what(); return false;
        }
    }
    bool import_current_semantic_details(boost::filesystem::path& output, std::string& error) {
        output.clear();
        error.clear();
        if (!semantic_regions_ready() || !m_semantic_source ||
            m_region_evidence->source_content_id != m_semantic_source->content_id) {
            error = "当前模型没有同源的可靠语义缓存";
            return false;
        }
        const auto source_hash = AI::model_artifact_sha256(m_model_path);
        auto evidence = SecondaryRegionEvidence::from_primary(*m_region_evidence, source_hash,
            m_semantic_source->content_id, error);
        if (!evidence) return false;
        const auto cache = boost::filesystem::path(Slic3r::data_dir()) / "cache" / "beauty_secondary_regions";
        const auto reference = SecondaryRegionEvidenceCache::save(*evidence, cache, error);
        if (reference.empty()) return false;
        auto loaded = SecondaryRegionEvidenceCache::load(reference, cache, m_geometry_id,
            source_hash, m_triangle_count, m_region_runtime_identity, error);
        if (!loaded) return false;
        output = cache / (reference.at("sha256").get<std::string>() + ".json");
        m_secondary_region_evidence = std::move(loaded);
        m_secondary_region_evidence_error.clear();
        return true;
    }
    bool regenerate_readonly_evidence(boost::filesystem::path& output, std::string& error) const {
        output.clear();
        error.clear();
        if (!m_has_model || m_model_path.empty() || !m_semantic_source) {
            error = "当前模型没有可用的原始面颜色输入";
            return false;
        }
        const auto source_hash = AI::model_artifact_sha256(m_model_path);
        if (source_hash.size() != 64) {
            error = "当前模型 source hash 不可用";
            return false;
        }
        const auto root = boost::filesystem::path(Slic3r::data_dir()) /
            "cache" / "beauty_evidence_runs";
        boost::system::error_code fs_error;
        boost::filesystem::create_directories(root, fs_error);
        if (fs_error) { error = "无法创建证据缓存目录"; return false; }
        const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string base_name = "run-" + std::to_string(stamp);
        // The writer creates the directory atomically. A second click or a
        // partial prior run only moves to a new suffix; no run is overwritten.
        for (size_t suffix = 0; suffix < 32; ++suffix) {
            const std::string name = suffix == 0 ? base_name :
                base_name + "-" + std::to_string(suffix);
            output = root / name;
            ReadonlyEvidenceRenderResult result;
            if (write_readonly_evidence_render_package(*m_semantic_source, output, source_hash,
                    1024, result, error)) return true;
            if (error != "证据输出目录已存在，未覆盖已有运行") return false;
        }
        output.clear();
        error = "无法分配新的证据运行目录";
        return false;
    }
    size_t select_semantic_detail(const std::string& detail_id, bool record_history = true,
                                  bool include_protected_preview = false) {
        m_semantic_selection_protected = m_semantic_selection_low_confidence = 0;
        if ((!secondary_regions_ready() && !m_portrait_shapes) || !region_editing_ready()) return 0;
        if (!m_region_editor->ready()) {
            m_deferred_selection = [this, detail_id, record_history, include_protected_preview] {
                select_semantic_detail(detail_id, record_history, include_protected_preview);
            };
            ensure_region_editor(); return 0;
        }
        const auto before = selection_state();
        for (const auto& detail : portrait_detail_catalog()) if (detail.key == detail_id) {
            auto next = before;
            next.selected.assign(editing_face_count(), 0);
            next.foreground.assign(editing_face_count(), 0);
            next.domain.assign(editing_face_count(), 0);
            size_t count = 0;
            for (const auto face : detail.faces) {
                if (face < next.protected_faces.size() && next.protected_faces[face]) { ++m_semantic_selection_protected; continue; }
                next.selected[face] = next.foreground[face] = next.domain[face] = 1; ++count;
            }
            if (!count) return 0;
            if (record_history) push_selection_history(before.selected);
            m_selection_preview_suppressed = false; m_selection_overlay_visible = true; set_selection_enabled(true);
            restore_selection_state(std::move(next));
            return count;
        }
        if (!secondary_regions_ready() || detail_id == "iris" || detail_id == "imouth" || AI::ShapeLockSet::supported_label(detail_id)) return 0;
        auto match = m_secondary_region_evidence->select(detail_id,
            m_leaf_editing ? m_leaf_editing->collapse(before) : before, include_protected_preview);
        if (m_leaf_editing && match.selected) {
            match.selection = m_leaf_editing->expand(match.selection);
            match.selection.protected_faces = before.protected_faces;
            for (size_t i = 0; i < match.selection.selected.size(); ++i)
                if (i < before.protected_faces.size() && before.protected_faces[i]) match.selection.selected[i] = 0;
            constrain_shape_selection(match.selection,true);
            match.selected = size_t(std::count(match.selection.selected.begin(),match.selection.selected.end(),uint8_t(1)));
        }
        m_semantic_selection_protected = match.protected_count;
        m_semantic_selection_low_confidence = match.low_confidence;
        if (!match.selected) return 0;
        if (record_history) push_selection_history(before.selected);
        m_selection_preview_suppressed = false; m_selection_overlay_visible = true; set_selection_enabled(true);
        restore_selection_state(std::move(match.selection));
        return match.selected;
    }
    std::vector<std::string> beauty_semantic_names() const {
        std::vector<std::string> names = {"unknown", "background", "hair", "face", "body", "cloth", "accessories",
            "lips", "imouth", "eyes", "iris", "eyebrow"};
        if (secondary_regions_ready()) for (const auto& detail : m_secondary_region_evidence->regions) names.push_back(detail.detail_id);
        if (m_portrait_shapes) for (const auto& detail : m_portrait_shapes->catalog()) names.push_back(detail.label);
        return names;
    }
    std::vector<std::string> beauty_semantic_guidance() const { return beauty_semantic_names(); }
    nlohmann::json semantic_region_evidence_metadata() const {
        if (!semantic_regions_ready()) return nlohmann::json::object();
        std::string error;
        auto reference = SemanticRegionEvidenceCache::save(*m_region_evidence,
            boost::filesystem::path(Slic3r::data_dir()) / "cache" / "beauty_regions", error);
        if (!error.empty()) BOOST_LOG_TRIVIAL(warning) << "Region evidence cache save failed: " << error;
        if (!reference.empty()) {
            const auto hash = AI::model_artifact_sha256(m_model_path);
            if (hash.empty()) return nlohmann::json::object();
            reference["model_sha256"] = hash;
        }
        return reference;
    }
    nlohmann::json secondary_region_evidence_metadata() const {
        if (!secondary_regions_ready()) return nlohmann::json::object();
        std::string error;
        auto reference = SecondaryRegionEvidenceCache::save(*m_secondary_region_evidence,
            boost::filesystem::path(Slic3r::data_dir()) / "cache" / "beauty_secondary_regions", error);
        if (!error.empty()) BOOST_LOG_TRIVIAL(warning) << "Secondary region evidence cache save failed: " << error;
        if (!reference.empty()) reference["model_sha256"] = AI::model_artifact_sha256(m_model_path);
        return reference;
    }
    bool semantic_result_active() const {
        return m_semantic_ready && (bool(m_semantic_analysis) ||
            !m_saved_semantic_faces.empty() || !m_saved_semantic_subfaces.empty());
    }
    bool semantic_optimization_enabled() const {
        return m_color_trial_enabled && m_color_trial && m_color_trial->semantic_optimization();
    }
    bool semantic_processing() const { return m_semantic_controller && m_semantic_controller->busy(); }
    bool semantic_reoptimization_available() const {
        return !semantic_processing() && m_has_model && bool(m_semantic_source) && m_color_trial &&
            !m_color_trial->colors().empty() && m_color_trial->colors().size() <= 6;
    }
    wxString semantic_reoptimization_reason() const {
        if (semantic_processing()) return _L("正在识别人像区域，请等待完成或取消。");
        if (!m_has_model) return _L("当前没有已加载模型。");
        if (!m_semantic_source) return _L("当前模型缺少可用于人像识别的面颜色输入。");
        if (!m_color_trial || m_color_trial->colors().empty()) return _L("当前没有有效色卡。");
        if (m_color_trial->colors().size() > 6) return _L("人像区域优化只支持 1 至 6 个有效槽位。");
        return m_semantic_error;
    }
    const wxString& semantic_error() const { return m_semantic_error; }
    std::vector<int32_t> beauty_semantic_labels() const {
        const bool parents=m_leaf_editing && m_portrait_shapes && m_portrait_shapes->surface_ownership;
        if (!semantic_regions_ready() && !parents) return {};
        std::vector<int32_t> labels;
        labels.reserve(m_triangle_count);
        for (size_t face = 0; face < m_triangle_count; ++face) {
            if (!semantic_regions_ready()) { labels.push_back(-1); continue; }
            const auto label = m_region_evidence->labels[face];
            labels.push_back(m_region_evidence->confidence[face] >= AI::SemanticColoring::minimum_confidence &&
                label != AI::SemanticColoring::Label::Unknown && label != AI::SemanticColoring::Label::Background
                    ? int32_t(native_facial_detail(label) ? AI::SemanticColoring::Label::FaceSkin : label) : -1);
        }
        if (secondary_regions_ready()) {
            std::set<size_t> assigned;
            for (size_t index = 0; index < m_secondary_region_evidence->regions.size(); ++index) {
                const auto& detail = m_secondary_region_evidence->regions[index];
                if (detail.detail_id == "iris" || detail.detail_id == "imouth" || AI::ShapeLockSet::supported_label(detail.detail_id)) continue;
                if (!detail.selectable()) continue;
                const int32_t semantic_id = int32_t(AI::SemanticColoring::label_count + index);
                for (size_t face : detail.accepted_faces)
                    if (face < labels.size() && labels[face] >= 0 && assigned.insert(face).second)
                        labels[face] = semantic_id;
            }
        }
        if (m_leaf_editing) labels = m_leaf_editing->expand(labels,int32_t(-1));
        if (parents) m_portrait_shapes->surface_ownership->overlay_labels(m_leaf_editing->keys,labels);
        if (m_portrait_shapes) {
            const auto offset = int32_t(AI::SemanticColoring::label_count + (secondary_regions_ready() ? m_secondary_region_evidence->regions.size() : 0));
            const auto details = portrait_detail_catalog();
            for (size_t index = 0; index < details.size(); ++index)
                for (const auto f : details[index].faces) labels[f] = offset + int32_t(index);
        }
        return labels;
    }
    void set_beauty_pick_callback(std::function<void(size_t)> callback) { m_beauty_pick = std::move(callback); }
    bool beauty_partition_visible() const { return bool(m_partition_model); }
    void set_beauty_partition(const std::vector<uint32_t>& pieces,
                              const std::vector<std::vector<int32_t>>& neighbors) {
        m_partition_model.reset();
        if (!m_context || !m_canvas->SetCurrent(*m_context)) return;
        if (!m_region_editor->ready() || pieces.size() != m_region_editor->mesh().indices.size() ||
            neighbors.size() != pieces.size()) return;
        const auto& mesh = m_region_editor->mesh();
        GLModel::Geometry lines;
        lines.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3N3};
        const float offset = std::max(1e-5f, float(m_bounds.size().norm()) * 0.0001f);
        if (m_leaf_editing) for (const auto& segment : m_leaf_editing->boundary_segments(pieces)) {
            const auto base = unsigned(lines.vertices_count());
            lines.add_vertex(segment.a+offset*segment.normal,segment.normal);
            lines.add_vertex(segment.b+offset*segment.normal,segment.normal);
            lines.add_line(base,base+1);
        }
        else for (size_t face = 0; face < pieces.size(); ++face) {
            const auto& triangle = mesh.indices[face];
            Vec3f normal = (mesh.vertices[triangle[1]] - mesh.vertices[triangle[0]])
                .cross(mesh.vertices[triangle[2]] - mesh.vertices[triangle[0]]);
            if (normal.squaredNorm() < 1e-12f) continue;
            normal.normalize();
            for (size_t edge = 0; edge < 3; ++edge) {
                const int32_t neighbor = edge < neighbors[face].size() ? neighbors[face][edge] : -1;
                if (neighbor < 0 || size_t(neighbor) <= face || pieces[face] == pieces[size_t(neighbor)]) continue;
                const unsigned int base = static_cast<unsigned int>(lines.vertices_count());
                lines.add_vertex(mesh.vertices[triangle[edge]] + offset * normal, normal);
                lines.add_vertex(mesh.vertices[triangle[(edge + 1) % 3]] + offset * normal, normal);
                lines.add_line(base, base + 1);
            }
        }
        if (!lines.is_empty()) {
            m_partition_model = std::make_unique<GLModel>();
            m_partition_model->init_from(std::move(lines));
            m_partition_model->set_color(ColorRGBA(0.14f, 0.85f, 0.38f, 1.f));
        }
        m_canvas->Refresh(false);
    }
    void set_beauty_lighting(bool enabled) {
        m_beauty_lighting = enabled;
        if (m_canvas) m_canvas->Refresh(false);
    }
    bool beauty_lighting() const { return m_beauty_lighting; }
    bool beauty_original_view() const { return m_beauty_original_view; }
    void set_beauty_original_view(bool enabled) {
        m_beauty_original_view = enabled;
        if (m_canvas) m_canvas->Refresh(false);
    }
    void set_beauty_view(bool enabled) {
        m_beauty_view = enabled;
        const wxColour background = enabled ? wxColour(49, 49, 54) : wxGetApp().get_window_default_clr();
        SetBackgroundColour(background);
        m_splitter->SetBackgroundColour(background);
        m_preview_host->SetBackgroundColour(background);
        m_region_prepare_status->SetBackgroundColour(background);
        m_region_prepare_status->SetForegroundColour(enabled ? wxColour(220, 220, 222) : *wxBLACK);
        Refresh(false);
        m_splitter->Refresh(false);
        m_preview_host->Refresh(false);
        if (m_canvas) m_canvas->Refresh(false);
    }
    void set_workbench_grid_visible(bool visible) {
        m_workbench_grid_visible = visible;
        if (m_canvas) m_canvas->Refresh(false);
    }
    size_t triangle_count() const { return m_triangle_count; }
    size_t vertex_count() const { return m_vertex_count; }
    const Vec3d& model_dimensions() const { return m_model_dimensions; }
    // Select an already recognized material region for Beauty editing. This
    // only consumes the cached analysis; it never starts recognition. Existing
    // protected faces remain protected so the user can add or subtract detail
    // with the normal brush tools afterward.
    size_t select_semantic_region(const std::string& region, bool record_history = true)
    {
        m_semantic_selection_protected = m_semantic_selection_low_confidence = 0;
        const bool parents=m_leaf_editing && m_portrait_shapes && m_portrait_shapes->surface_ownership;
        if ((!semantic_regions_ready() && !parents) || !region_editing_ready()) return 0;
        if (!m_region_editor->ready()) {
            m_deferred_selection = [this, region, record_history] { select_semantic_region(region, record_history); };
            ensure_region_editor();
            return 0;
        }
        auto state = selection_state();
        SemanticRegionEvidence::Match match;
        if (semantic_regions_ready()) match = m_region_evidence->select(region,m_leaf_editing ? m_leaf_editing->collapse(state) : state);
        else { match.selection=state; match.selection.selected.assign(state.selected.size(),0); }
        if (!match.selected) {
            if (!parents || (region!="skin" && region!="clothes")) return 0;
            match.selection=state;
            match.selection.selected.assign(state.selected.size(),0);
            match.selection.foreground.assign(state.selected.size(),0);
            match.selection.domain.assign(state.selected.size(),0);
        }
        if (m_leaf_editing && match.selected) {
            match.selection = m_leaf_editing->expand(match.selection);
            match.selection.protected_faces = state.protected_faces;
            for (size_t i = 0; i < match.selection.selected.size(); ++i)
                if (i < state.protected_faces.size() && state.protected_faces[i]) match.selection.selected[i] = 0;
        }
        if (parents) {
            if (match.selection.selected.size()!=m_leaf_editing->keys.size()) {
                match.selection=state;match.selection.selected.assign(state.selected.size(),0);
            }
            m_portrait_shapes->surface_ownership->overlay_selection(region,m_leaf_editing->keys,match.selection);
        }
        m_semantic_selection_protected = match.protected_count;
        m_semantic_selection_low_confidence = match.low_confidence;
        constrain_shape_selection(match.selection, true);
        match.selected = size_t(std::count(match.selection.selected.begin(), match.selection.selected.end(), uint8_t(1)));
        if (!match.selected) return 0;
        if (record_history) push_selection_history(state.selected);
        m_selection_preview_suppressed = false;
        m_selection_overlay_visible = true;
        set_selection_enabled(true);
        restore_selection_state(std::move(match.selection));
        return match.selected;
    }
    // Re-run the existing semantic request only after an explicit user action.
    bool request_semantic_reoptimization() {
        if (!semantic_reoptimization_available()) {
            m_semantic_error = semantic_reoptimization_reason();
            return false;
        }
        m_semantic_error.clear();
        m_color_trial->activate_for_beauty_semantics();
        m_color_trial_enabled = m_color_trial->enabled();
        m_trial_palette = m_color_trial->colors();
        // A completed request is normally de-duplicated by the worker. An
        // explicit Beauty action must still create a fresh candidate, while
        // allowing the worker to reuse its analysis cache.
        if (m_semantic_controller) m_semantic_controller->cancel();
        update_semantic_coloring();
        return m_semantic_controller && m_semantic_controller->busy();
    }
    void cancel_semantic_request() {
        m_semantic_error = _L("用户已取消人像区域优化。");
        if (m_semantic_controller) m_semantic_controller->cancel();
        m_semantic_timer.Stop();
        m_color_trial->set_semantic_status(m_semantic_error, false);
        if (m_semantic_completion) {
            auto callback = std::move(m_semantic_completion);
            callback(false);
        }
    }
    void set_semantic_completion_callback(std::function<void(bool)> callback) {
        m_semantic_completion = std::move(callback);
    }
    const FaceColorOverrides& face_color_overrides() const { return m_face_color_overrides; }
    size_t paint_beauty_faces(const std::vector<size_t>& faces, const RGBA& color);
    bool restore_beauty_face_colors(const FaceColorOverrides& colors);
    void set_paint_commit_callback(std::function<void(const std::vector<size_t>&)> callback) { m_paint_commit = std::move(callback); }
    void set_beauty_history_callback(std::function<void(bool)> callback) { m_beauty_history = std::move(callback); }
    FaceColorOverrides import_face_color_overrides(bool use_current_trial = true) const {
        if (use_current_trial && !m_semantic_analysis && !m_saved_semantic_faces.empty())
            { auto result=AI::SemanticColoring::compose(m_saved_semantic_faces,m_face_color_overrides,true);
              auto children=SubfaceColorOverrides{}; AI::compose_leaf_colors(result,children,m_manual_leaf_colors); return result; }
        const bool semantic = use_current_trial && m_color_trial_enabled &&
            m_color_trial->semantic_optimization() && m_semantic_ready && m_semantic_analysis;
        auto automatic = semantic
            ? AI::SemanticColoring::apply_semantic_region_slot_overrides(m_automatic_face_colors, *m_semantic_analysis,
                m_color_trial->semantic_region_slots(), m_color_trial->semantic_palette())
            : m_automatic_face_colors;
        if (semantic && !(m_portrait_shapes && m_portrait_shapes->locks.leaf_domain)) {
            FaceColorOverrides locked;
            if (m_portrait_shapes) for (const auto& item : m_automatic_face_colors)
                if (m_portrait_shapes->locks.face_locked(item.first)) locked.push_back(item);
            auto children = m_automatic_subface_colors;
            compose_portrait_shapes(*m_semantic_analysis, m_portrait_shapes.get(), automatic, children, locked);
        }
        auto result=AI::SemanticColoring::compose(automatic,m_face_color_overrides,semantic);
        auto children=SubfaceColorOverrides{};
        if (semantic && m_portrait_shapes && m_portrait_shapes->locks.leaf_domain)
            AI::preserve_locked_leaf_colors(m_portrait_shapes->locks,m_automatic_face_colors,m_automatic_subface_colors,result,children);
        AI::compose_leaf_colors(result,children,m_manual_leaf_colors); return result;
    }
    SubfaceColorOverrides import_subface_color_overrides(bool use_current_trial = true) const {
        if (use_current_trial && !m_semantic_analysis && !m_saved_semantic_subfaces.empty())
            { auto result=AI::SemanticColoring::compose_subfaces(m_saved_semantic_subfaces,m_face_color_overrides,true);
              auto roots=FaceColorOverrides{}; AI::compose_leaf_colors(roots,result,m_manual_leaf_colors); return result; }
        const bool semantic = use_current_trial && m_color_trial_enabled &&
            m_color_trial->semantic_optimization() && m_semantic_ready && m_semantic_analysis;
        auto automatic = semantic
            ? AI::SemanticColoring::apply_semantic_region_slot_overrides(m_automatic_subface_colors, *m_semantic_analysis,
                m_color_trial->semantic_region_slots(), m_color_trial->semantic_palette())
            : m_automatic_subface_colors;
        if (semantic && !(m_portrait_shapes && m_portrait_shapes->locks.leaf_domain)) {
            auto faces = m_automatic_face_colors;
            compose_portrait_shapes(*m_semantic_analysis, m_portrait_shapes.get(), faces, automatic);
        }
        auto result=AI::SemanticColoring::compose_subfaces(automatic,m_face_color_overrides,semantic);
        auto roots=semantic ? AI::SemanticColoring::apply_semantic_region_slot_overrides(m_automatic_face_colors,*m_semantic_analysis,
            m_color_trial->semantic_region_slots(),m_color_trial->semantic_palette()) : FaceColorOverrides{};
        if (semantic && m_portrait_shapes && m_portrait_shapes->locks.leaf_domain)
            AI::preserve_locked_leaf_colors(m_portrait_shapes->locks,m_automatic_face_colors,m_automatic_subface_colors,roots,result);
        AI::compose_leaf_colors(roots,result,m_manual_leaf_colors); return result;
    }
    nlohmann::json semantic_color_metadata() const {
        if (!m_semantic_analysis) return nlohmann::json::object();
        return {{"schema", "orca.semantic-color-provenance/v1"}, {"signature", m_semantic_analysis->signature},
            {"content_sha256", m_semantic_analysis->content_id}, {"body", m_semantic_analysis->body_identity},
            {"face", m_semantic_analysis->face_identity}};
    }
    nlohmann::json semantic_result_metadata() const {
        const auto faces = import_face_color_overrides(true);
        const auto subfaces = import_subface_color_overrides(true);
        if (faces.empty() && subfaces.empty()) return nlohmann::json::object();
        nlohmann::json result = {{"schema", "orca.semantic-result/v1"}, {"geometry_id", m_geometry_id},
            {"face_count", m_triangle_count}, {"faces", nlohmann::json::array()},
            {"subfaces", nlohmann::json::array()}};
        for (const auto& item : faces) result["faces"].push_back({item.first, item.second});
        for (const auto& item : subfaces)
            result["subfaces"].push_back({item.face_id, item.path.depth, item.path.value, item.color});
        return result;
    }
    BeautySourceSnapshot beauty_source() const {
        if(m_region_editor->ready())
            return BeautySourceSnapshot::share(m_region_editor,m_region_editor->mesh(),m_region_editor->vertex_colors());
        if(!m_region_prepare_failed && current_region_preparation())
            return BeautySourceSnapshot::share(m_region_preparation,m_region_preparation->mesh,m_region_preparation->colors);
        return {};
    }
    bool region_selection_failed() const { return m_region_prepare_failed; }
    bool set_saved_semantic_result(FaceColorOverrides faces, SubfaceColorOverrides subfaces) {
        if (!m_semantic_source || !m_context || !m_canvas->SetCurrent(*m_context)) return false;
        auto geometry = build_semantic_colored_geometry(*m_semantic_source, faces, subfaces);
        if (geometry.is_empty()) return false;
        auto model = std::make_unique<GLModel>();
        model->init_from(std::move(geometry));
        m_semantic_model = std::move(model);
        m_saved_semantic_faces = std::move(faces);
        m_saved_semantic_subfaces = std::move(subfaces);
        m_semantic_ready = true;
        m_canvas->Refresh(false);
        return true;
    }
    nlohmann::json selection_metadata() const {
        return AI::SurfaceSelectionPersistence::encode(m_leaf_editing ? m_leaf_editing->collapse(selection_state()) : selection_state(), m_triangle_count, m_geometry_id);
    }
    nlohmann::json face_color_metadata() const {
        return AI::SurfaceSelectionPersistence::encode_colors(m_face_color_overrides, m_triangle_count, m_geometry_id);
    }
    void restore_selection_state(SelectionState state)
    {
        if (!ensure_region_editor()) {
            m_deferred_selection = [this, state = std::move(state)]() mutable { restore_selection_state(std::move(state)); };
            return;
        }
        if (m_leaf_editing && state.selected.size() == m_triangle_count) state = m_leaf_editing->expand(state);
        if (!m_region_editor->restore_selection(state.selected)) return;
        m_protected_faces = std::move(state.protected_faces);
        if (m_protected_faces.size() != state.selected.size()) m_protected_faces.assign(state.selected.size(), 0);
        m_foreground_faces = std::move(state.foreground); m_selection_domain = std::move(state.domain);
        if (m_foreground_faces.size() != state.selected.size()) m_foreground_faces.assign(state.selected.size(), 0);
        if (m_selection_domain.size() != state.selected.size()) m_selection_domain = state.selected;
        rebuild_selection_model(); notify_selection_changed(); m_canvas->Refresh(false);
    }
    size_t protected_face_count() const { return std::count(m_protected_faces.begin(), m_protected_faces.end(), uint8_t(1)); }
    bool selection_busy() const { return m_surface_task && !m_surface_task->canceled; }
    bool cancel_selection_calculation()
    {
        if (!selection_busy()) return false;
        cancel_surface_selection();
        notify_selection_changed();
        return true;
    }
    void refine_selection_boundary()
    {
        if (!m_region_editor->ready() || selection_busy()) return;
        if (m_surface_worker.joinable()) {
            if (m_surface_task && !m_surface_task->done) return;
            finish_surface_selection();
        }
        auto task = std::make_shared<SurfaceTask>();
        task->generation = m_region_generation; task->refinement = true;
        task->editor = m_region_editor;
        task->started = std::chrono::steady_clock::now();
        m_surface_task = task;
        auto indices = [](const std::vector<uint8_t>& mask) {
            std::vector<size_t> result;
            for (size_t i=0; i<mask.size(); ++i) if (mask[i]) result.push_back(i);
            return result;
        };
        auto editor = m_region_editor;
        auto roi = indices(m_selection_domain), foreground = indices(m_foreground_faces), background = indices(m_protected_faces);
        try {
        m_surface_worker = std::thread([task, editor, roi = std::move(roi), foreground = std::move(foreground), background = std::move(background)] {
            try {
                auto result = AI::SurfaceSelectionRefinement::refine(editor->mesh(), editor->vertex_colors(), roi,
                    foreground, background, [task] { return task->canceled.load(); });
                task->result.faces = std::move(result.faces); task->result.canceled = result.canceled;
                task->error = result.error;
            } catch (const std::exception& e) { task->error = e.what(); }
            task->done = true;
        });
        } catch (const std::exception& e) {
            task->error = e.what(); task->done = true;
            finish_surface_selection(); return;
        }
        m_surface_timer.Start(40);
        show_region_preparation_status(_L("正在贴合选区边界，可按 Esc 取消……")); notify_selection_changed();
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
    void set_color_trial_changed_callback(std::function<void(size_t)> callback)
    {
        m_color_trial_changed = std::move(callback);
    }
    void set_selection_commit_callback(std::function<void(const SelectionState&, const SelectionState&)> callback)
    {
        m_selection_commit = std::move(callback);
    }

    void clear_selection(bool record_history = true)
    {
        cancel_surface_selection();
        const bool canceled_click = bool(m_deferred_selection);
        m_deferred_selection = {};
        if (canceled_click && m_selection_enabled && current_region_preparation())
            show_region_preparation_status(_L("已清空等待中的点选；正在准备局部选择，可继续旋转模型。"));
        if (record_history && (m_region_editor->selected_face_count() > 0 || protected_face_count() > 0))
            push_selection_history(m_region_editor->selected_faces());
        m_protected_faces.assign(m_region_editor->selected_faces().size(), 0);
        m_foreground_faces.assign(m_region_editor->selected_faces().size(), 0);
        m_selection_domain.assign(m_region_editor->selected_faces().size(), 0);
        m_region_editor->clear_selection();
        rebuild_selection_model();
        notify_selection_changed();
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
    }

    size_t selected_face_count() const { return selection_busy() ? 0 : m_region_editor->selected_face_count(); }
    std::vector<size_t> selected_face_indices() const {
        std::vector<size_t> result;
        const auto& selected = m_region_editor->selected_faces();
        for (size_t i = 0; i < selected.size(); ++i) if (selected[i]) result.push_back(i);
        return result;
    }
    bool gray_view() const {return m_gray_view;}
    bool wireframe_view() const {return m_wireframe_view;}
    bool auto_rotation() const {return m_rotation_timer.IsRunning();}
    void set_wireframe_view(bool enabled) {m_wireframe_view=enabled;m_canvas->Refresh(false);}
    void set_auto_rotation(bool enabled) {
        if(enabled && m_has_model && IsShownOnScreen()) {
            m_rotation_tick=std::chrono::steady_clock::now();m_rotation_timer.Start(33);
        } else m_rotation_timer.Stop();
    }
    void set_gray_view(bool enabled) { m_gray_view = enabled; m_canvas->Refresh(false); }
    void set_selection_overlay_visible(bool visible) { m_selection_overlay_visible = visible; m_canvas->Refresh(false); }
    void set_selection_preview_suppressed(bool suppressed) {
        if (m_selection_preview_suppressed == suppressed) return;
        m_selection_preview_suppressed = suppressed;
        m_canvas->Refresh(false);
    }
    bool focus_selection() {
        if (!m_region_editor->ready() || !m_region_editor->selected_face_count()) return false;
        const auto& mesh = m_region_editor->mesh();
        const auto& selected = m_region_editor->selected_faces();
        BoundingBoxf3 bounds;
        for (size_t i = 0; i < selected.size(); ++i) if (selected[i])
            for (int k = 0; k < 3; ++k) bounds.merge(mesh.vertices[mesh.indices[i][k]].cast<double>());
        const auto rotation = view_rotation();
        const Vec3d offset = rotation.linear() * (bounds.center() - m_bounds.center()).cast<double>();
        const double radius = std::max(0.001, 0.5 * m_bounds.size().norm());
        m_pan_x = -offset.x() / radius; m_pan_y = -offset.y() / radius;
        const double aspect = double(std::max(1, m_canvas->GetClientSize().x)) / std::max(1, m_canvas->GetClientSize().y);
        const Vec3d full = rotation.linear().cwiseAbs() * (0.5 * m_bounds.size().cast<double>());
        const Vec3d patch = rotation.linear().cwiseAbs() * (0.5 * bounds.size().cast<double>());
        m_zoom = std::clamp(std::max(full.y(), full.x() / aspect) /
            std::max(0.001, 1.25 * std::max(patch.y(), patch.x() / aspect)), 0.45, 12.0);
        m_canvas->Refresh(false);
        return true;
    }
    ModelPreviewColorControls::State color_trial_state() const { return m_color_trial->state(); }
    wxWindow* build_workbench_palette(wxWindow* parent) { return m_color_trial->build_workbench_palette(parent); }
    void synchronize_project_colors(bool activate = false) {
        const auto before = m_color_trial->state();
        m_suppress_semantic_change = true;
        const bool changed = m_color_trial->synchronize_project_colors(activate);
        m_suppress_semantic_change = false;
        m_color_trial_enabled = m_color_trial->enabled();
        m_trial_palette = m_color_trial->colors();
        if (changed) synchronize_project_bound_semantics(before, m_color_trial->state());
        m_canvas->Refresh(false);
    }
    void synchronize_project_bound_semantics(const ModelPreviewColorControls::State& before,
        const ModelPreviewColorControls::State& after) {
        if (before.semantic_palette == after.semantic_palette) return;
        auto recolor = [&before, &after](auto& color) {
            const auto replacement = AI::ColorTrialPersistence::project_target_replacement(before, after,
                {color[0], color[1], color[2]});
            if (replacement) for (size_t i = 0; i < 3; ++i) color[i] = (*replacement)[i];
        };
        for (auto& item : m_automatic_face_colors) recolor(item.second);
        for (auto& item : m_automatic_subface_colors) recolor(item.color);
        std::unordered_set<size_t> manual_faces;
        for (const auto& item : m_face_color_overrides) manual_faces.insert(item.first);
        // Saved semantic results include manual leaf overlays and their descendants.
        auto manual_leaf = [this](size_t face, uint8_t depth, uint8_t path) {
            for (uint8_t ancestor = 0; ancestor <= depth; ++ancestor)
                if (m_manual_leaf_colors.count({face, ancestor, uint8_t(path >> (2 * (depth - ancestor)))})) return true;
            return false;
        };
        for (auto& item : m_saved_semantic_faces)
            if (!manual_faces.count(item.first) && !manual_leaf(item.first, 0, 0)) recolor(item.second);
        for (auto& item : m_saved_semantic_subfaces)
            if (!manual_faces.count(item.face_id) && !manual_leaf(item.face_id, item.path.depth, item.path.value)) recolor(item.color);
        if (!m_semantic_analysis && (!m_saved_semantic_faces.empty() || !m_saved_semantic_subfaces.empty()))
            set_saved_semantic_result(m_saved_semantic_faces, m_saved_semantic_subfaces);
        else rebuild_semantic_preview_from_cached_result();
    }
    void set_workbench_palette_editable(bool editable) { m_color_trial->set_workbench_editable(editable); }
    void show_workbench_palette_details() {
        if (!m_has_model) return;
        set_color_controls_visible(false);
        wxDialog dialog(this, wxID_ANY, _L("色卡详情"), wxDefaultPosition, FromDIP(wxSize(620, 560)),
                        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        dialog.SetBackgroundColour(wxColour(32, 32, 35));
        dialog.SetName("ai_content_color");
        dialog.SetFont(GetFont());
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* scroll = new WorkbenchScrolledWindow(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        scroll->SetBackgroundColour(wxColour(32, 32, 35));
        scroll->SetScrollRate(0, FromDIP(12));
        auto* controls = new wxBoxSizer(wxVERTICAL);
        m_controls_scroll->GetSizer()->Detach(m_color_trial);
        m_color_trial->Reparent(scroll);
        WorkbenchAppearanceScope appearance(m_color_trial);
        m_color_trial->set_workbench_detail_choices(true);
        controls->Add(m_color_trial, 0, wxEXPAND);
        scroll->SetSizer(controls);
        m_color_trial->Show();
        root->Add(scroll, 1, wxEXPAND | wxALL, FromDIP(12));
        auto* close = workbench_button(&dialog, _L("完成"));
        close->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
        root->Add(close, 0, wxALIGN_RIGHT | wxALL, FromDIP(12));
        dialog.SetSizer(root);
        scroll->FitInside();
        dialog.CenterOnParent();
        dialog.ShowModal();
        m_color_trial->set_workbench_detail_choices(false);
        controls->Detach(m_color_trial);
        m_color_trial->Reparent(m_controls_scroll);
        m_controls_scroll->GetSizer()->Add(m_color_trial, 0, wxEXPAND);
        m_color_trial->Hide();
    }
    nlohmann::json color_trial_metadata() const {
        AI::ColorTrialPersistence::State saved = m_color_trial->state();
        if (saved.source != 1 || saved.project_slot_identity.empty()) {
            saved.source = 2;
            saved.project_color_slots.clear(); saved.project_semantic_slots.clear(); saved.project_slot_identity.clear();
        }
        return AI::ColorTrialPersistence::encode(saved, m_triangle_count, m_geometry_id);
    }
    void restore_color_trial(const ModelPreviewColorControls::State& state) { m_color_trial->restore(state); }
    void restore_color_trial_without_recognition(const ModelPreviewColorControls::State& state) {
        m_suppress_semantic_change = true;
        m_color_trial->restore(state);
        m_suppress_semantic_change = false;
        m_color_trial_enabled = m_color_trial->enabled();
        m_trial_palette = m_color_trial->colors();
        m_canvas->Refresh(false);
    }
    void set_color_controls_visible(bool visible) {
        const bool show = visible && m_has_model;
        m_color_trial->Show(show);
        m_controls_scroll->Show(show);
        if (show && !m_splitter->IsSplit())
            m_splitter->SplitHorizontally(m_preview_host, m_controls_scroll, m_saved_splitter_sash);
        else if (!show && m_splitter->IsSplit())
            m_splitter->Unsplit(m_controls_scroll);
        Layout();
    }
    // Capture on the UI thread; callers own the copy and cannot mutate preview
    // controls, project slots or the source mesh through this snapshot.
    PreviewPalette::ColorTrialMapping color_trial_mapping() const {
        return {m_color_trial_enabled, m_color_trial->mapping_colors(), m_trial_palette};
    }
    PreviewPalette::ColorTrialMapping import_color_mapping(bool use_current_trial = true) const {
        // Original-color viewing must not silently commit the six-color trial.
        // Native matching chooses its target count independently of feed slots.
        return use_current_trial && m_color_trial_enabled ? color_trial_mapping()
                                                        : PreviewPalette::ColorTrialMapping {};
    }
    bool region_selection_preparing() const { return region_editing_ready() && current_region_preparation(); }
    bool region_editing_ready() const {
        return !m_region_prepare_failed && (m_region_editor->ready() || current_region_preparation() || (!m_pending_mesh.indices.empty() &&
            m_pending_vertex_colors.size() == m_pending_mesh.vertices.size()));
    }
    bool can_undo_selection() const { return selection_busy() || bool(m_deferred_selection) || !m_selection_history.empty(); }
    bool can_redo_selection() const { return !selection_busy() && !m_selection_redo.empty(); }

    bool selection_matches_face_evidence(const std::vector<size_t>& face_indices) const
    {
        if (!m_region_editor->ready() || face_indices.empty())
            return false;
        std::vector<uint8_t> expected(m_region_editor->selected_faces().size(), 0);
        if (m_leaf_editing) {
            const std::set<size_t> roots(face_indices.begin(),face_indices.end());
            for (size_t i=0;i<m_leaf_editing->keys.size();++i)
                expected[i]=roots.count(m_leaf_editing->keys[i].source_face_id)?1:0;
            return std::any_of(expected.begin(),expected.end(),[](auto value){return value!=0;}) && expected==m_region_editor->selected_faces();
        }
        bool has_valid_face = false;
        for (size_t face_index : face_indices) {
            if (face_index >= expected.size())
                continue;
            expected[face_index] = 1;
            has_valid_face = true;
        }
        return has_valid_face && expected == m_region_editor->selected_faces();
    }

    bool undo_selection()
    {
        if (selection_busy()) { cancel_surface_selection(); notify_selection_changed(); return true; }
        if (m_deferred_selection) {
            m_deferred_selection = {};
            if (current_region_preparation())
                show_region_preparation_status(_L("已撤销等待中的点选；正在准备局部选择，可继续旋转模型。"));
            return true;
        }
        if (m_selection_history.empty())
            return false;
        m_selection_redo.push_back(selection_state());
        SelectionState previous = std::move(m_selection_history.back());
        m_selection_history.pop_back();
        if (!m_region_editor->restore_selection(previous.selected))
            return false;
        m_protected_faces = std::move(previous.protected_faces);
        m_foreground_faces = std::move(previous.foreground); m_selection_domain = std::move(previous.domain);
        rebuild_selection_model();
        notify_selection_changed();
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
        return true;
    }

    bool redo_selection()
    {
        if (selection_busy() || m_selection_redo.empty()) return false;
        m_selection_history.push_back(selection_state());
        auto state = std::move(m_selection_redo.back()); m_selection_redo.pop_back();
        restore_selection_state(std::move(state));
        return true;
    }

    bool apply_selection_color(const RGBA& color, const boost::filesystem::path& source,
                               const boost::filesystem::path& destination, std::string& error)
    {
        if (source != m_model_path || !same_stamp(m_model_stamp, file_stamp(source))) {
            error = "The source model changed while recoloring. Please reload it.";
            return false;
        }
        if (m_leaf_editing) { error = "Subface edits require the texture-safe Beauty appearance path."; return false; }
        return m_region_editor->apply_color_to_obj_copy(color, source, destination, error);
    }

    bool select_palette_material(size_t palette_index)
    {
        std::vector<ColorRGBA> decoded;
        decode_colors(m_palette, decoded);
        if (decoded.empty() || palette_index >= decoded.size())
            return false;
        if (!ensure_region_editor()) {
            if (region_editing_ready()) m_deferred_selection = [this, palette_index] { select_palette_material(palette_index); };
            return false;
        }
        std::vector<RGBA> palette;
        palette.reserve(decoded.size());
        for (const ColorRGBA& color : decoded)
            palette.push_back({color.r(), color.g(), color.b(), color.a()});

        const std::vector<uint8_t> previous = m_region_editor->selected_faces();
        m_region_editor->select_palette_material(palette, palette_index);
        if (previous != m_region_editor->selected_faces())
            push_selection_history(previous);
        rebuild_selection_model();
        notify_selection_changed();
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
        return true;
    }

    size_t select_elevated_overhang_regions()
    {
        if (!ensure_region_editor(true)) {
            if (region_editing_ready()) m_deferred_selection = [this] { select_elevated_overhang_regions(); };
            return 0;
        }
        const std::vector<uint8_t> previous = m_region_editor->selected_faces();
        const size_t localized = m_region_editor->select_elevated_overhang_regions();
        if (localized == 0)
            return 0;
        if (previous != m_region_editor->selected_faces())
            push_selection_history(previous);
        rebuild_selection_model();
        notify_selection_changed();
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
        return localized;
    }

    size_t select_face_evidence(const std::vector<size_t>& face_indices)
    {
        if (face_indices.empty())
            return 0;
        if (!ensure_region_editor()) {
            if (region_editing_ready()) m_deferred_selection = [this, face_indices] { select_face_evidence(face_indices); };
            return 0;
        }
        const std::vector<uint8_t> previous = m_region_editor->selected_faces();
        std::vector<size_t> editing_indices;
        if (m_leaf_editing) {
            std::set<size_t> roots(face_indices.begin(),face_indices.end());
            for (size_t i = 0; i < m_leaf_editing->keys.size(); ++i)
                if (roots.count(m_leaf_editing->keys[i].source_face_id)) editing_indices.push_back(i);
        }
        const size_t localized = m_region_editor->select_faces(m_leaf_editing ? editing_indices : face_indices);
        if (localized == 0)
            return 0;
        if (previous != m_region_editor->selected_faces())
            push_selection_history(previous);
        rebuild_selection_model();
        notify_selection_changed();
        if (m_canvas != nullptr)
            m_canvas->Refresh(false);
        return localized;
    }

private:
    struct CachedPreview {
        std::vector<std::unique_ptr<GLModel>> models;
        std::unique_ptr<ModelPreviewTexture> texture_model;
        std::unique_ptr<GLModel> manual_color_model;
        indexed_triangle_set mesh;
        std::vector<RGBA> vertex_colors;
        BoundingBoxf3 bounds;
        Vec3d dimensions {Vec3d::Zero()};
        boost::filesystem::path path;
        FileStamp stamp;
        size_t triangles {0};
        size_t vertices {0};
        size_t colors {0};
        std::vector<PreviewPalette::Color> trial_palette;
        std::shared_ptr<const PreviewPalette::Histogram> trial_histogram;
        std::string geometry_id;
        FaceColorOverrides face_color_overrides;
        std::optional<SelectionState> selection;
        std::optional<ModelPreviewColorControls::State> color_trial;
        std::shared_ptr<const AI::SemanticColoring::MeshSnapshot> semantic_source;
        FaceColorOverrides saved_semantic_faces;
        SubfaceColorOverrides saved_semantic_subfaces;
        std::shared_ptr<const SemanticRegionEvidence> region_evidence;
        std::string region_runtime_identity, region_evidence_error;
        std::shared_ptr<const SecondaryRegionEvidence> secondary_region_evidence;
        std::string secondary_region_evidence_error;
        std::shared_ptr<const PortraitShapeDetails> shape_details;
        bool shapes_unlocked {false};
        nlohmann::json leaf_edits;
    };

    static FileStamp file_stamp(const boost::filesystem::path& path)
    {
        FileStamp stamp;
        std::error_code error;
        const std::filesystem::path native_path(path.native());
        stamp.bytes = std::filesystem::file_size(native_path, error);
        if (error)
            return stamp;
        stamp.modified = std::filesystem::last_write_time(native_path, error);
        stamp.valid = !error;
        return stamp;
    }

    static bool same_stamp(const FileStamp& first, const FileStamp& second)
    {
        return first.valid && second.valid && first.bytes == second.bytes && first.modified == second.modified;
    }

    void cache_current_preview()
    {
        m_cached_preview.reset();
        if (m_retain_surface_attributes) return;
        // A/B review needs one prior model. Never retain the much larger selection
        // adjacency/BVH or let large artifacts accumulate through version browsing.
        if (!m_has_model || !m_model_stamp.valid)
            return;
        const auto canonical = m_leaf_editing ? m_leaf_editing->canonical_editor : m_region_editor;
        const auto& mesh = canonical->ready() ? canonical->mesh()
            : current_region_preparation() ? m_region_preparation->mesh : m_pending_mesh;
        const auto& colors = canonical->ready() ? canonical->vertex_colors()
            : current_region_preparation() ? m_region_preparation->colors : m_pending_vertex_colors;
        size_t bytes = mesh.vertices.capacity() * sizeof(Vec3f) +
                       mesh.indices.capacity() * sizeof(stl_triangle_vertex_indices) +
                       colors.capacity() * sizeof(RGBA);
        if (m_region_evidence) bytes += m_region_evidence->labels.capacity() * sizeof(AI::SemanticColoring::Label) +
            m_region_evidence->confidence.capacity() * sizeof(float);
        for (const auto& model : m_models)
            bytes += model->cpu_memory_used() + model->gpu_memory_used();
        if (m_texture_model) bytes += m_texture_model->memory_used();
        if (m_manual_color_model) bytes += m_manual_color_model->cpu_memory_used() + m_manual_color_model->gpu_memory_used();
        constexpr size_t cache_limit = size_t(384) * 1024 * 1024;
        if (bytes > cache_limit)
            return;
        m_cached_preview = std::make_unique<CachedPreview>();
        m_cached_preview->geometry_id = m_geometry_id;
        m_cached_preview->face_color_overrides = m_face_color_overrides;
        m_cached_preview->manual_color_model = std::move(m_manual_color_model);
        m_cached_preview->semantic_source = m_semantic_source;
        m_cached_preview->region_evidence = m_region_evidence;
        m_cached_preview->region_runtime_identity = m_region_runtime_identity;
        m_cached_preview->region_evidence_error = m_region_evidence_error;
        m_cached_preview->secondary_region_evidence = m_secondary_region_evidence;
        m_cached_preview->secondary_region_evidence_error = m_secondary_region_evidence_error;
        m_cached_preview->shape_details = m_portrait_shapes;
        m_cached_preview->shapes_unlocked = m_shapes_unlocked;
        m_cached_preview->leaf_edits=leaf_edit_metadata();
        m_cached_preview->saved_semantic_faces = m_saved_semantic_faces;
        m_cached_preview->saved_semantic_subfaces = m_saved_semantic_subfaces;
        m_cached_preview->selection = m_leaf_editing ? m_leaf_editing->collapse(selection_state()) : selection_state();
        m_cached_preview->color_trial = m_color_trial->state();
        m_cached_preview->models = std::move(m_models);
        m_cached_preview->texture_model = std::move(m_texture_model);
        if (canonical->ready()) {
            // Retain only compact source arrays, never the adjacency/BVH. This
            // keeps the first local before/after comparison free of OBJ parsing.
            m_cached_preview->mesh = canonical->mesh();
            m_cached_preview->vertex_colors = canonical->vertex_colors();
        } else if (current_region_preparation()) {
            m_cached_preview->mesh = m_region_preparation->mesh;
            m_cached_preview->vertex_colors = m_region_preparation->colors;
        } else {
            m_cached_preview->mesh = std::move(m_pending_mesh);
            m_cached_preview->vertex_colors = std::move(m_pending_vertex_colors);
        }
        m_cached_preview->bounds = m_bounds;
        m_cached_preview->dimensions = m_model_dimensions;
        m_cached_preview->path = m_model_path;
        m_cached_preview->stamp = m_model_stamp;
        m_cached_preview->triangles = m_triangle_count;
        m_cached_preview->vertices = m_vertex_count;
        m_cached_preview->colors = m_color_count;
        m_cached_preview->trial_histogram = m_trial_histogram;
        // New model loads reset the trial; never relabel a previous custom or
        // project palette as a fresh automatic suggestion on a cache hit.
        const size_t preview_color_count = std::clamp(m_color_count, size_t(1), PreviewPalette::max_preview_colors);
        m_cached_preview->trial_palette = m_trial_histogram
            ? m_trial_histogram->palette(preview_color_count, {}, true) : m_trial_palette;
    }

    struct RegionPreparation {
        uint64_t generation {0};
        // These compact source arrays stay immutable while the worker builds its
        // private editor, so switching models can still cache the source safely.
        indexed_triangle_set mesh;
        std::vector<RGBA> colors;
        AI::VertexColorRegionEditor editor;
        // Upgrade only the tolerant legacy graph. Read-only source ownership
        // keeps geometry alive without copying it on the UI thread.
        std::shared_ptr<const AI::VertexColorRegionEditor> source_editor;
        std::unique_ptr<AI::VertexColorRegionEditor::RegionTopology> topology;
        bool regions {true};
        std::atomic<bool> canceled {false}, done {false};
        std::shared_ptr<const PortraitShapeDetails> shapes;
        std::shared_ptr<AI::BeautyLeafEditing> leaves;
        bool success {false};
        std::string error;
        std::chrono::steady_clock::time_point started;
    };

    bool current_region_preparation() const {
        return m_region_preparation && m_region_preparation->generation == m_region_generation;
    }

    void show_region_preparation_status(const wxString& message) {
        m_region_prepare_status->SetLabel(message);
        m_region_prepare_status->SetBackgroundColour(m_beauty_view ? wxColour(49, 49, 54) : *wxWHITE);
        m_region_prepare_status->SetForegroundColour(m_beauty_view ? wxColour(220, 220, 222) : *wxBLACK);
        m_region_prepare_status->Show();
        Layout();
    }

    bool ensure_region_editor(bool require_regions = true)
    {
        // Leaf editing and native region selection share the prepared topology.
        const bool regions = require_regions;
        if (m_region_editor->ready() && (!regions || m_region_editor->region_selection_ready()))
            return true;
        if (!region_editing_ready())
            return false;
        show_region_preparation_status(_L("正在准备局部选择，可继续旋转模型；点选会在准备完成后执行。"));
        // An invalidated worker is allowed to finish without blocking navigation.
        // The timer starts the latest model only after that worker has exited.
        if (m_region_preparation) return false;
        auto task = std::make_shared<RegionPreparation>();
        task->generation = m_region_generation;
        task->regions = regions;
        if (m_region_editor->ready()) task->source_editor = m_region_editor;
        else {
            task->mesh = std::move(m_pending_mesh);
            task->colors = std::move(m_pending_vertex_colors);
        }
        task->shapes = m_portrait_shapes;
        task->started = std::chrono::steady_clock::now();
        m_region_preparation = task;
        try {
            m_region_prepare_worker = std::thread([task] {
                try {
                    const auto canceled = [task] { return task->canceled.load(); };
                    if (task->canceled.load()) task->error = "Local selection preparation canceled.";
                    else if (task->source_editor) {
                        task->topology = task->source_editor->prepare_region_topology(task->error, canceled);
                        task->success = bool(task->topology);
                    } else if (task->regions)
                        task->success = task->editor.initialize(task->mesh, task->colors, task->error, canceled);
                    else task->success = task->editor.initialize_for_picking(task->mesh, task->colors, task->error, canceled);
                    if (task->success && task->shapes && task->shapes->locks.leaf_domain) {
                        auto leaf_editor = std::make_shared<AI::VertexColorRegionEditor>();
                        if (task->source_editor)
                            task->success = leaf_editor->initialize(task->source_editor->mesh(), task->source_editor->vertex_colors(), task->error, canceled);
                        else
                            *leaf_editor = std::move(task->editor);
                        if (task->success)
                            task->leaves = AI::BeautyLeafEditing::build(task->shapes->locks, std::move(leaf_editor), task->shapes->base_colors,
                                task->shapes->surface_ownership ? &task->shapes->surface_ownership->editing_domain : nullptr);
                    }
                }
                catch (const std::exception& error) { task->error = error.what(); }
                catch (...) { task->error = "Unknown region preparation failure"; }
                task->done.store(true, std::memory_order_release);
            });
            m_region_prepare_timer.Start(50);
        } catch (const std::exception& error) {
            m_pending_mesh = std::move(task->mesh);
            m_pending_vertex_colors = std::move(task->colors);
            m_region_preparation.reset();
            m_region_prepare_failed = !task->source_editor;
            show_region_preparation_status(_L("局部选择准备失败，请重新加载模型后重试。"));
            BOOST_LOG_TRIVIAL(warning) << "AI model preview selection worker failed: " << error.what();
            if (m_region_prepare_failed) set_selection_enabled(false);
        }
        return false;
    }

    void finish_region_preparation()
    {
        if (!m_region_preparation || !m_region_preparation->done.load(std::memory_order_acquire)) return;
        if (m_region_prepare_worker.joinable()) m_region_prepare_worker.join();
        auto task = std::move(m_region_preparation);
        m_region_prepare_timer.Stop();
        if (task->generation != m_region_generation || task->shapes != m_portrait_shapes) {
            if (task->generation == m_region_generation) {
                m_pending_mesh = std::move(task->mesh); m_pending_vertex_colors = std::move(task->colors);
            }
            if (m_selection_enabled || m_deferred_selection) ensure_region_editor();
            return;
        }
        BOOST_LOG_TRIVIAL(info) << "AI model preview selection prepare: elapsed_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - task->started).count()
            << ", background=true, region_topology=" << task->regions
            << ", upgrade=" << bool(task->source_editor) << ", success=" << task->success;
        if (task->success && task->source_editor)
            task->success = task->source_editor == m_region_editor &&
                m_region_editor->install_region_topology(std::move(task->topology));
        if (!task->success) {
            m_region_prepare_failed = !task->source_editor;
            m_deferred_selection = {};
            show_region_preparation_status(_L("局部选择准备失败，请重新加载模型后重试。"));
            BOOST_LOG_TRIVIAL(warning) << "AI model preview selection initialization failed: " << task->error;
            if (m_region_prepare_failed) set_selection_enabled(false);
            return;
        }
        m_leaf_editing = std::move(task->leaves);
        if (m_leaf_editing) m_region_editor = m_leaf_editing->editor;
        else if (!task->source_editor) m_region_editor = std::make_shared<AI::VertexColorRegionEditor>(std::move(task->editor));
        if (m_pending_selection) {
            auto state = std::move(*m_pending_selection); m_pending_selection.reset();
            if (m_leaf_editing && state.selected.size() == m_triangle_count) state = m_leaf_editing->expand(state);
            m_region_editor->restore_selection(state.selected);
            m_protected_faces = std::move(state.protected_faces);
            m_foreground_faces = std::move(state.foreground); m_selection_domain = std::move(state.domain);
            rebuild_selection_model();
        }
        if (!m_pending_leaf_edits.is_null()) restore_leaf_edits(m_pending_leaf_edits);
        m_region_prepare_status->Hide();
        Layout();
        auto deferred = std::move(m_deferred_selection);
        m_deferred_selection = {};
        if (deferred) deferred();
        else if (m_selection_enabled || m_beauty_view) notify_selection_changed();
    }

    void push_selection_history(const std::vector<uint8_t>& selected_faces)
    {
        const size_t bytes = std::max(size_t(1), selected_faces.size() + m_protected_faces.size() +
            m_foreground_faces.size() + m_selection_domain.size());
        const size_t max_history = std::max(size_t(1), std::min(size_t(20), (size_t(64)*1024*1024)/bytes));
        if (m_selection_history.size() >= max_history)
            m_selection_history.erase(m_selection_history.begin());
        m_selection_redo.clear();
        m_selection_history.push_back({selected_faces, m_protected_faces, m_foreground_faces, m_selection_domain});
    }

    void finish_drag()
    {
        m_dragging = false;
        m_drawing_selection = false;
        m_stroke.clear();
        if (m_canvas) m_canvas->Refresh(false);
        if (m_canvas != nullptr && m_canvas->HasCapture())
            m_canvas->ReleaseMouse();
    }

    void notify_selection_changed()
    {
        if (m_selection_changed)
            m_selection_changed(selected_face_count());
    }

    struct SurfaceTask {
        std::atomic<bool> canceled {false}, done {false};
        uint64_t generation {0};
        std::shared_ptr<const AI::VertexColorRegionEditor> editor;
        SelectionGesture gesture {SelectionGesture::Lasso};
        bool refinement {false};
        AI::SurfaceSelection::Result result;
        std::string error;
        std::chrono::steady_clock::time_point started;
    };

    void cancel_surface_selection()
    {
        if (m_surface_task) m_surface_task->canceled = true;
        m_drawing_selection = false;
        m_stroke.clear();
    }

    void submit_surface_selection()
    {
        if (m_stroke.empty()) return;
        if (m_surface_task && !m_surface_task->done) {
            show_region_preparation_status(_L("正在计算选区；可按 Esc 取消，完成后继续补选。"));
            return;
        }
        const int width = std::max(1, m_canvas->GetClientSize().x);
        const int height = std::max(1, m_canvas->GetClientSize().y);
        const double radius = std::max(0.001, 0.5 * m_bounds.size().norm());
        const double half_height = fitted_half_height(width, height);
        const Transform3d view = Geometry::translation_transform(Vec3d(m_pan_x * radius, m_pan_y * radius, -3.0 * radius)) *
            view_rotation() * Geometry::translation_transform(-m_bounds.center().cast<double>());
        auto request = [this, stroke = selection_outline(), gesture = m_selection_gesture,
                        brush_radius = m_brush_radius, view, half_height, width, height] {
            start_surface_selection(stroke, gesture, brush_radius, view, half_height, width, height);
        };
        if (!ensure_region_editor()) {
            m_deferred_selection = std::move(request);
            show_region_preparation_status(_L("已记住本次范围，模型准备好后自动选择；可按 Esc 取消。"));
        } else request();
    }

    std::vector<Vec2d> selection_outline() const
    {
        if (m_selection_gesture != SelectionGesture::Lasso || m_stroke.size() < 2) return m_stroke;
        Vec2d lo = m_stroke.front(), hi = lo;
        double area = 0;
        for (size_t i=0; i<m_stroke.size(); ++i) {
            lo = lo.cwiseMin(m_stroke[i]); hi = hi.cwiseMax(m_stroke[i]);
            const auto& a = m_stroke[i]; const auto& b = m_stroke[(i+1)%m_stroke.size()];
            area += a.x()*b.y()-a.y()*b.x();
        }
        // A diagonal drag is a rectangle; a curved path is a freehand lasso.
        // This also makes short coarse selections useful without a precision loop.
        if ((hi-lo).squaredNorm() > 36 && std::abs(area) < 0.08 * (hi-lo).squaredNorm())
            return {lo, Vec2d(hi.x(),lo.y()), hi, Vec2d(lo.x(),hi.y())};
        return m_stroke;
    }

    void start_surface_selection(const std::vector<Vec2d>& stroke, SelectionGesture gesture, double brush_radius,
                                 const Transform3d& view, double half_height, int width, int height)
    {
        if (m_surface_worker.joinable()) {
            if (m_surface_task && !m_surface_task->done) return;
            finish_surface_selection();
        }
        auto task = std::make_shared<SurfaceTask>();
        task->generation = m_region_generation; task->gesture = gesture;
        task->editor = m_region_editor;
        task->started = std::chrono::steady_clock::now();
        m_surface_task = task;
        auto editor = m_region_editor; // A model switch leaves the worker's immutable geometry alive.
        const Transform3d inverse = view.inverse();
        const double half_width = half_height * double(width) / height;
        try {
        m_surface_worker = std::thread([task, editor, stroke, brush_radius, view, inverse, half_width, half_height, width, height] {
            try {
                auto project = [=](const Vec3f& point) -> std::optional<Vec2d> {
                    const Vec3d p = view * point.cast<double>();
                    if (p.z() >= 0) return std::nullopt;
                    return Vec2d((p.x() / half_width + 1) * width / 2.0, (1 - p.y() / half_height) * height / 2.0);
                };
                auto pick = [=](const Vec2d& point) -> std::optional<size_t> {
                    const Vec3d origin = inverse * Vec3d((2 * point.x() / width - 1) * half_width,
                        (1 - 2 * point.y() / height) * half_height, 0);
                    return editor->pick_face(origin, inverse.linear() * Vec3d(0, 0, -1));
                };
                auto canceled = [task] { return task->canceled.load(); };
                task->result = task->gesture == SelectionGesture::Lasso && stroke.size() >= 3
                    ? AI::SurfaceSelection::visible_polygon_faces(editor->mesh(), stroke, project, pick, canceled)
                    : AI::SurfaceSelection::visible_brush_faces(editor->mesh(), stroke, brush_radius, project, pick, canceled);
            } catch (const std::exception& e) { task->error = e.what(); }
            task->done = true;
        });
        } catch (const std::exception& e) {
            task->error = e.what(); task->done = true;
            finish_surface_selection(); return;
        }
        m_surface_timer.Start(40);
        show_region_preparation_status(_L("正在选择可见表面，模型可继续转动；Esc 取消。"));
        notify_selection_changed();
    }

    void finish_surface_selection()
    {
        if (!m_surface_task || !m_surface_task->done) return;
        if (m_surface_worker.joinable()) m_surface_worker.join();
        m_surface_timer.Stop();
        auto task = std::move(m_surface_task);
        if (task->generation != m_region_generation || task->editor != m_region_editor) return;
        if (task->canceled || task->result.canceled) {
            m_region_prepare_status->Hide(); notify_selection_changed(); return;
        }
        if (!task->error.empty()) {
            show_region_preparation_status(task->refinement
                ? _L("请先圈选局部范围，在要修改处涂抹补选、在不要修改处涂抹保护，再贴合边界。范围过大时请缩小重试。")
                : _L("本次选区未完成，请缩小范围重试；当前模型保持原样。"));
            BOOST_LOG_TRIVIAL(warning) << "Surface selection failed: " << task->error;
            notify_selection_changed(); return;
        }
        if (task->gesture == SelectionGesture::Paint) {
            // Paint only this stroke, not a previously selected whole partition.
            if (m_paint_commit) m_paint_commit(task->result.faces);
            m_region_prepare_status->Hide();
            notify_selection_changed(); m_canvas->Refresh(false); return;
        }
        const auto before = selection_state();
        auto selected = m_region_editor->selected_faces();
        if (m_protected_faces.size() != selected.size()) m_protected_faces.assign(selected.size(), 0);
        if (m_foreground_faces.size() != selected.size()) m_foreground_faces.assign(selected.size(), 0);
        if (m_selection_domain.size() != selected.size()) m_selection_domain = selected;
        push_selection_history(selected);
        if (task->refinement) std::fill(selected.begin(), selected.end(), 0);
        for (size_t face : task->result.faces) {
            if (face >= selected.size()) continue;
            if (task->gesture == SelectionGesture::Protect) {
                m_selection_domain[face] = 1;
                m_protected_faces[face] = 1; m_foreground_faces[face] = 0; selected[face] = 0;
            } else if (task->gesture == SelectionGesture::Brush) {
                // Explicitly painting here can reverse a mistaken protection stroke.
                m_protected_faces[face] = 0; m_foreground_faces[face] = 1; m_selection_domain[face] = 1; selected[face] = 1;
            } else if (!m_protected_faces[face]) { selected[face] = 1; m_selection_domain[face] = 1; }
        }
        m_region_editor->restore_selection(selected);
        rebuild_selection_model();
        const auto after = selection_state();
        if (m_selection_commit && (before.selected != after.selected ||
            before.protected_faces != after.protected_faces || before.foreground != after.foreground ||
            before.domain != after.domain)) m_selection_commit(before, after);
        show_region_preparation_status(task->result.faces.empty()
            ? _L("范围内未选到可见表面；可放大模型或用涂抹补选。")
            : _L("范围已更新。圈选会保留保护区；涂抹补选可重新纳入误保护的地方。"));
        BOOST_LOG_TRIVIAL(info) << "Surface selection: elapsed_ms=" <<
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - task->started).count()
            << ", hit_faces=" << task->result.faces.size() << ", selected_faces=" << selected_face_count();
        notify_selection_changed(); m_canvas->Refresh(false);
    }

    void select_at(const wxPoint& point)
    {
        if (m_canvas == nullptr)
            return;
        int width = 0;
        int height = 0;
        m_canvas->GetClientSize(&width, &height);
        if (width <= 0 || height <= 0)
            return;
        const Vec3d size = m_bounds.size().cast<double>();
        const double radius = std::max(0.001, 0.5 * size.norm());
        const double half_height = fitted_half_height(width, height);
        const double half_width = half_height * double(width) / double(height);
        const double screen_x = 2.0 * double(point.x) / double(width) - 1.0;
        const double screen_y = 1.0 - 2.0 * double(point.y) / double(height);
        const Vec3d center = m_bounds.center().cast<double>();
        const Transform3d view_model =
            Geometry::translation_transform(Vec3d(m_pan_x * radius, m_pan_y * radius, -3.0 * radius)) *
            view_rotation() *
            Geometry::translation_transform(-center);
        const Transform3d inverse = view_model.inverse();
        const Vec3d origin = inverse * Vec3d(screen_x * half_width, screen_y * half_height, 0.0);
        const Vec3d direction = inverse.linear() * Vec3d(0.0, 0.0, -1.0);
        if (!ensure_region_editor()) {
            if (region_editing_ready()) {
                // Save the model-space ray, not screen coordinates: users may
                // continue orbiting while the editor is being prepared.
                m_deferred_selection = [this, origin, direction, operation = m_selection_operation, settings = m_selection_settings] {
                    select_ray(origin, direction, operation, settings);
                };
                show_region_preparation_status(_L("已记住最近一次点选；准备完成后自动选中，可继续旋转模型。"));
            }
            return;
        }
        select_ray(origin, direction, m_selection_operation, m_selection_settings);
    }

    void select_ray(const Vec3d& origin, const Vec3d& direction,
                    AI::RegionSelectionOperation operation, const AI::RegionSelectionSettings& settings)
    {
        if (selection_busy()) return;
        const auto hit = m_region_editor->pick_surface(origin,direction);
        const std::optional<size_t> face = hit ? std::optional<size_t>(hit->face) : std::nullopt;
        if (hit && m_leaf_editing && !(m_leaf_editing->hit_key(*hit) == m_leaf_editing->keys[hit->face])) return;
        if (!face)
            return;
        if (!ensure_region_editor(true)) {
            m_deferred_selection = [this, origin, direction, operation, settings] { select_ray(origin, direction, operation, settings); };
            return;
        }
        if (m_beauty_view && m_beauty_pick && m_selection_gesture == SelectionGesture::Similar) {
            m_beauty_pick(*face);
            return;
        }
        const auto before = selection_state();
        const std::vector<uint8_t> previous = m_region_editor->selected_faces();
        m_region_editor->update_selection(*face, operation, settings);
        auto mask = m_region_editor->selected_faces();
        for (size_t i = 0; i < std::min(mask.size(), m_protected_faces.size()); ++i)
            if (m_protected_faces[i]) mask[i] = 0;
        m_region_editor->restore_selection(mask);
        if (previous != m_region_editor->selected_faces())
            push_selection_history(previous);
        rebuild_selection_model();
        const auto after = selection_state();
        if (m_selection_commit && (before.selected != after.selected ||
            before.protected_faces != after.protected_faces || before.foreground != after.foreground ||
            before.domain != after.domain)) m_selection_commit(before, after);
        notify_selection_changed();
        m_canvas->Refresh(false);
    }

    void rebuild_selection_model()
    {
        m_selection_model.reset();
        m_protection_model.reset();
        if (!m_region_editor->ready())
            return;
        build_mask_model(m_region_editor->selected_faces(), m_selection_preview_color, m_selection_model);
        build_mask_model(m_protected_faces, ColorRGBA(0.3f, 0.5f, 0.95f, 1.0f), m_protection_model);
    }

    void build_mask_model(const std::vector<uint8_t>& selected, const ColorRGBA& color, std::unique_ptr<GLModel>& target)
    {
        if (selected.size() != m_region_editor->mesh().indices.size()) return;
        GLModel::Geometry geometry;
        geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};
        const indexed_triangle_set& mesh = m_region_editor->mesh();
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
            target = std::make_unique<GLModel>();
            target->init_from(std::move(geometry));
            target->set_color(color);
        }
    }

    // One derived frame per CPU-loaded source, using this existing GL context
    // and model buffers. No thumbnail canvases, geometry copies or UI camera
    // changes are created while the user scrolls the library.
    void cache_library_model_thumbnail(const std::string& sha)
    {
        if (m_library_thumbnail_root.empty() || !library_model_thumbnail_sha(sha) ||
            !same_stamp(m_model_stamp, file_stamp(m_model_path)) ||
            OpenGLManager::get_framebuffers_type() != OpenGLManager::EFramebufferType::Arb ||
            !wxGetApp().init_opengl()) return;
        if (!library_model_thumbnail_image(m_library_thumbnail_root, m_model_path).empty()) return;
        try {
            if (!m_color_shader) {
                m_color_shader = std::make_unique<GLShaderProgram>();
                if (!initialize_model_color_shader(*m_color_shader)) { m_color_shader.reset(); return; }
            }
            constexpr int width = 640, height = 480;
            GLuint fbo = 0, color = 0, depth = 0;
            GLint draw_fbo = 0, read_fbo = 0, renderbuffer = 0, viewport[4] {}, program = 0;
            GLint alignment = 0, row_length = 0, skip_rows = 0, skip_pixels = 0, pack_buffer = 0;
            GLint depth_func = 0, polygon[2] {};
            GLboolean depth_write = GL_TRUE, color_write[4] {};
            GLfloat clear_color[4] {}; GLdouble clear_depth = 1;
            const bool depth_test = ::glIsEnabled(GL_DEPTH_TEST), cull = ::glIsEnabled(GL_CULL_FACE);
            const bool blend = ::glIsEnabled(GL_BLEND), scissor = ::glIsEnabled(GL_SCISSOR_TEST);
            ::glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo); ::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
            ::glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer); ::glGetIntegerv(GL_VIEWPORT, viewport);
            ::glGetIntegerv(GL_CURRENT_PROGRAM, &program); ::glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
            ::glGetIntegerv(GL_POLYGON_MODE, polygon); ::glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_write);
            ::glGetBooleanv(GL_COLOR_WRITEMASK, color_write); ::glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);
            ::glGetDoublev(GL_DEPTH_CLEAR_VALUE, &clear_depth);
            ::glGetIntegerv(GL_PACK_ALIGNMENT, &alignment); ::glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
            ::glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows); ::glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
            ::glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
            Slic3r::ScopeGuard restore([&] {
                ::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo); ::glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
                ::glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer); ::glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
                ::glUseProgram(program); ::glDepthFunc(depth_func); ::glDepthMask(depth_write);
                ::glColorMask(color_write[0],color_write[1],color_write[2],color_write[3]);
                ::glClearColor(clear_color[0],clear_color[1],clear_color[2],clear_color[3]); ::glClearDepth(clear_depth);
                ::glPolygonMode(GL_FRONT_AND_BACK, polygon[0]);
                (depth_test ? ::glEnable : ::glDisable)(GL_DEPTH_TEST);
                (cull ? ::glEnable : ::glDisable)(GL_CULL_FACE); (blend ? ::glEnable : ::glDisable)(GL_BLEND);
                (scissor ? ::glEnable : ::glDisable)(GL_SCISSOR_TEST);
                ::glPixelStorei(GL_PACK_ALIGNMENT, alignment); ::glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
                ::glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows); ::glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
                ::glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
                if (depth) ::glDeleteRenderbuffers(1, &depth); if (color) ::glDeleteRenderbuffers(1, &color);
                if (fbo) ::glDeleteFramebuffers(1, &fbo);
            });
            (void)restore;
            ::glGenFramebuffers(1, &fbo); ::glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            ::glGenRenderbuffers(1, &color); ::glBindRenderbuffer(GL_RENDERBUFFER, color);
            ::glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
            ::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
            ::glGenRenderbuffers(1, &depth); ::glBindRenderbuffer(GL_RENDERBUFFER, depth);
            ::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
            ::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
            if (::glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return;
            ::glViewport(0, 0, width, height); ::glDisable(GL_SCISSOR_TEST); ::glDisable(GL_CULL_FACE);
            ::glDisable(GL_BLEND); ::glEnable(GL_DEPTH_TEST); ::glDepthFunc(GL_LESS); ::glDepthMask(GL_TRUE);
            ::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); ::glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            ::glClearColor(49.f/255,49.f/255,54.f/255,1); ::glClearDepth(1);
            ::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            const Vec3d size = m_bounds.size().cast<double>();
            const double radius = std::max(0.001, 0.5 * size.norm()), near_z = 0.01 * radius, far_z = 8.0 * radius;
            const auto angles = initial_view_angles(size);
            const Transform3d rotation = Geometry::rotation_transform(angles.second * Vec3d::UnitX()) *
                Geometry::rotation_transform(angles.first * Vec3d::UnitZ());
            const Vec3d extents = rotation.linear().cwiseAbs() * (0.5 * size);
            const double aspect = double(width) / height;
            const double hh = std::max(0.001, std::max(extents.y(), extents.x()/aspect)*1.08);
            Transform3d projection = Transform3d::Identity(); projection.matrix().setZero();
            projection.matrix()(0,0)=1/(hh*aspect); projection.matrix()(1,1)=1/hh;
            projection.matrix()(2,2)=-2/(far_z-near_z); projection.matrix()(2,3)=-(far_z+near_z)/(far_z-near_z);
            projection.matrix()(3,3)=1;
            const Transform3d view = Geometry::translation_transform(Vec3d(0,0,-3*radius)) * rotation *
                Geometry::translation_transform(-m_bounds.center().cast<double>());
            auto* shader = m_color_shader.get(); shader->start_using();
            shader->set_uniform("view_model_matrix", view); shader->set_uniform("projection_matrix", projection);
            shader->set_uniform("view_normal_matrix", Matrix3d(view.matrix().block(0,0,3,3).inverse().transpose()));
            shader->set_uniform("use_uniform_color", false); shader->set_uniform("preview_texture_enabled", false);
            shader->set_uniform("preview_unlit_overlay", false); shader->set_uniform("preview_boundary_stroke", false);
            shader->set_uniform("gray_view", false); shader->set_uniform("exact_surface_colors", false);
            shader->set_uniform("preview_lighting", true); shader->set_uniform("preview_lightness_weight", PreviewPalette::lightness_weight);
            shader->set_uniform("preview_color_count", 0);
            if (m_texture_model) m_texture_model->render(shader, view);
            else for (const auto& model : m_models) model->render(shader);
            shader->stop_using();
            ::glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
            ::glPixelStorei(GL_PACK_ROW_LENGTH, 0); ::glPixelStorei(GL_PACK_SKIP_ROWS, 0); ::glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
            ::glReadBuffer(GL_COLOR_ATTACHMENT0);
            std::vector<unsigned char> pixels(size_t(width)*height*3);
            ::glReadPixels(0,0,width,height,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
            if (::glGetError() != GL_NO_ERROR || !same_stamp(m_model_stamp, file_stamp(m_model_path))) return;
            publish_library_model_thumbnail(m_library_thumbnail_root, m_model_path,
                {sha, m_model_stamp.bytes, static_cast<int64_t>(m_model_stamp.modified.time_since_epoch().count())}, width, height, pixels);
        } catch (const std::exception& error) {
            BOOST_LOG_TRIVIAL(warning) << "Model library thumbnail unavailable: " << error.what();
        }
    }

    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(m_canvas);
        // Match the native Orca canvas: nested paints must not render or swap
        // the same context while a frame is already being submitted.
        if (m_in_paint) {
            m_canvas->Refresh(false);
            return;
        }
        const wxSize drawable_size = m_canvas->GetClientSize();
        if (!m_canvas->IsShownOnScreen() || drawable_size.x <= 0 || drawable_size.y <= 0)
            return;
        m_in_paint = true;
        Slic3r::ScopeGuard paint_guard([this]() { m_in_paint = false; });
        (void)paint_guard;
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
        const wxColour background = m_preview_background.IsOk() ? m_preview_background :
            m_beauty_view ? wxColour(49, 49, 54) : wxGetApp().get_window_default_clr();
        glsafe(::glClearColor(background.Red() / 255.0f, background.Green() / 255.0f, background.Blue() / 255.0f, 1.0f));
        glsafe(::glClearDepth(1.0));
        glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

        if (m_has_model) {
            if (!m_color_shader) {
                m_color_shader = std::make_unique<GLShaderProgram>();
                if (!initialize_model_color_shader(*m_color_shader)) {
                    BOOST_LOG_TRIVIAL(error) << "AI model vertex-color shader could not be initialized";
                    m_color_shader.reset();
                }
            }
            GLShaderProgram* shader = m_color_shader.get();
            if (shader != nullptr) {
                glsafe(::glEnable(GL_DEPTH_TEST));
                glsafe(::glDisable(GL_CULL_FACE));
                shader->start_using();

                const Vec3d size = m_bounds.size().cast<double>();
                const double radius = std::max(0.001, 0.5 * size.norm());
                const double aspect = double(width) / double(height);
                const double half_height = fitted_half_height(width, height);
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
                    Geometry::translation_transform(Vec3d(m_pan_x * radius, m_pan_y * radius, -3.0 * radius)) *
                    view_rotation() *
                    Geometry::translation_transform(-center);
                const Matrix3d normal_matrix = view_model.matrix().block(0, 0, 3, 3).inverse().transpose();
                shader->set_uniform("view_model_matrix", view_model);
                shader->set_uniform("projection_matrix", projection);
                shader->set_uniform("view_normal_matrix", normal_matrix);
                if (m_beauty_view && m_workbench_grid_visible) {
                    if (!m_workbench_grid || m_workbench_grid_geometry != m_geometry_id) {
                        GLModel::Geometry grid;
                        grid.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3N3};
                        const float extent = float(radius * 1.8);
                        const float z = m_bounds.min.z() - float(radius * 0.01);
                        for (unsigned int i = 0; i <= 18; ++i) {
                            const float offset = -extent + 2.f * extent * float(i) / 18.f;
                            const unsigned int base = unsigned(grid.vertices_count());
                            grid.add_vertex(Vec3f(float(center.x()) - extent, float(center.y()) + offset, z), Vec3f(0, 0, 1));
                            grid.add_vertex(Vec3f(float(center.x()) + extent, float(center.y()) + offset, z), Vec3f(0, 0, 1));
                            grid.add_vertex(Vec3f(float(center.x()) + offset, float(center.y()) - extent, z), Vec3f(0, 0, 1));
                            grid.add_vertex(Vec3f(float(center.x()) + offset, float(center.y()) + extent, z), Vec3f(0, 0, 1));
                            grid.add_line(base, base + 1); grid.add_line(base + 2, base + 3);
                        }
                        m_workbench_grid = std::make_unique<GLModel>();
                        m_workbench_grid->init_from(std::move(grid));
                        m_workbench_grid->set_color(ColorRGBA(0.30f, 0.30f, 0.32f, 1.f));
                        m_workbench_grid_geometry = m_geometry_id;
                    }
                    shader->set_uniform("use_uniform_color", true);
                    shader->set_uniform("preview_color_count", 0);
                    shader->set_uniform("preview_lighting", false);
                    shader->set_uniform("beauty_unlit", true);
                    shader->set_uniform("gray_view", false);
                    m_workbench_grid->render(shader);
                }
                shader->set_uniform("use_uniform_color", false);
                shader->set_uniform("preview_texture_enabled", false);
                shader->set_uniform("preview_unlit_overlay", false);
                shader->set_uniform("preview_boundary_stroke", false);
                shader->set_uniform("exact_surface_colors", m_exact_surface_display);
                shader->set_uniform("gray_view", m_gray_view);
                shader->set_uniform("preview_lighting", m_beauty_view ? m_beauty_lighting : m_color_trial->lighting());
                shader->set_uniform("beauty_unlit", m_beauty_view && !m_beauty_lighting);
                shader->set_uniform("preview_lightness_weight", PreviewPalette::lightness_weight);
                shader->set_uniform("preview_color_count", m_color_trial_enabled && !m_gray_view &&
                    !(m_beauty_view && m_beauty_original_view) ? int(m_trial_palette.size()) : 0);
                if (m_color_trial_enabled) for (size_t i = 0; i < m_trial_palette.size(); ++i) {
                    shader->set_uniform(("preview_rgb[" + std::to_string(i) + "]").c_str(), m_trial_palette[i]);
                    shader->set_uniform(("preview_lab[" + std::to_string(i) + "]").c_str(), PreviewPalette::to_lab(m_color_trial->mapping_colors()[i]));
                }
                // Pure separation must not introduce intermediate colors at
                // multisample edges or through framebuffer dithering. Restore
                // both states immediately after drawing the trial surface.
                const bool pure_separation = m_color_trial_enabled && !m_gray_view && !m_color_trial->lighting();
                const bool multisample = pure_separation && ::glIsEnabled(GL_MULTISAMPLE);
                const bool dither = pure_separation && ::glIsEnabled(GL_DITHER);
                if (pure_separation) {
                    glsafe(::glDisable(GL_MULTISAMPLE));
                    glsafe(::glDisable(GL_DITHER));
                }
                GLint polygon_mode[2]={GL_FILL,GL_FILL};
                ::glGetIntegerv(GL_POLYGON_MODE,polygon_mode);
                if(m_wireframe_view)glsafe(::glPolygonMode(GL_FRONT_AND_BACK,GL_LINE));
                if (m_semantic_ready && m_semantic_model && ((m_color_trial_enabled && m_color_trial->semantic_optimization()) ||
                    (m_beauty_view && (!m_saved_semantic_faces.empty() || !m_saved_semantic_subfaces.empty())) ||
                    (m_leaf_editing && !m_manual_leaf_colors.empty())) &&
                    !m_gray_view && !(m_beauty_view && m_beauty_original_view))
                    m_semantic_model->render(shader);
                else if (m_texture_model && !m_gray_view && !m_exact_surface_display)
                    m_texture_model->render(shader, view_model);
                else for (const std::unique_ptr<GLModel>& model : m_models)
                    model->render(shader);
                if(m_wireframe_view)glsafe(::glPolygonMode(GL_FRONT_AND_BACK,polygon_mode[0]));
                if (m_manual_color_model && m_beauty_view && !m_gray_view && !m_beauty_original_view) {
                    shader->set_uniform("preview_texture_enabled", false);
                    GLint depth_func = GL_LESS;
                    glsafe(::glGetIntegerv(GL_DEPTH_FUNC, &depth_func));
                    glsafe(::glDepthFunc(GL_LEQUAL));
                    m_manual_color_model->render(shader);
                    glsafe(::glDepthFunc(depth_func));
                }
                if (multisample) glsafe(::glEnable(GL_MULTISAMPLE));
                if (dither) glsafe(::glEnable(GL_DITHER));
                if ((m_selection_model || m_protection_model) && m_selection_enabled &&
                    m_selection_overlay_visible && !m_selection_preview_suppressed) {
                    shader->set_uniform("gray_view", false);
                    shader->set_uniform("use_uniform_color", true);
                    shader->set_uniform("preview_color_count", 0);
                    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
                    glsafe(::glPolygonOffset(-1.0f, -1.0f));
                    if (m_selection_model) m_selection_model->render(shader);
                    if (m_protection_model) m_protection_model->render(shader);
                    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
                }
                if (m_beauty_view && m_partition_model && !m_selection_preview_suppressed) {
                    shader->set_uniform("gray_view", false);
                    shader->set_uniform("use_uniform_color", true);
                    shader->set_uniform("preview_color_count", 0);
                    shader->set_uniform("preview_lighting", false);
                    m_partition_model->render(shader);
                }
                if (!m_render_diagnostics_logged || m_render_diagnostics_size != wxSize(width,height) ||
                    m_render_diagnostics_zoom != m_zoom) {
                    const GLenum error = ::glGetError();
                    GLint viewport[4]{};
                    ::glGetIntegerv(GL_VIEWPORT,viewport);
                    BOOST_LOG_TRIVIAL(info) << "AI model preview render: groups=" << m_models.size()
                                            << ", viewport=" << width << "x" << height
                                            << ", shader=" << shader->get_id()
                                            << ", gl_error=" << error
                                            << ", client=" << drawable_size.x << "x" << drawable_size.y
                                            << ", dpi=" << m_canvas->GetDPIScaleFactor()
                                            << ", content_scale=" << m_canvas->GetContentScaleFactor()
                                            << ", gl_viewport=" << viewport[2] << "x" << viewport[3]
                                            << ", zoom=" << m_zoom;
                    m_render_diagnostics_logged = true;
                    m_render_diagnostics_size = wxSize(width,height);
                    m_render_diagnostics_zoom = m_zoom;
                }
                shader->stop_using();
                glsafe(::glDisable(GL_DEPTH_TEST));
                if (m_drawing_selection && !m_stroke.empty()) {
                    GLModel::Geometry line;
                    line.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3N3};
                    const double cw = std::max(1, m_canvas->GetClientSize().x), ch = std::max(1, m_canvas->GetClientSize().y);
                    auto add_point = [&](const Vec2d& p) {
                        line.add_vertex(Vec3f(float(2*p.x()/cw-1), float(1-2*p.y()/ch), 0), Vec3f(0, 0, 1));
                    };
                    const auto outline = selection_outline();
                    for (const auto& p : outline) add_point(p);
                    for (unsigned int i = 1; i < outline.size(); ++i) line.add_line(i-1, i);
                    if (m_selection_gesture == SelectionGesture::Lasso && outline.size() > 2)
                        line.add_line(unsigned(outline.size()-1), 0);
                    else {
                        const unsigned int base = unsigned(line.vertices_count());
                        for (unsigned int i = 0; i < 32; ++i) {
                            const double a = i * 6.283185307179586 / 32;
                            add_point(m_stroke.back() + m_brush_radius * Vec2d(std::cos(a), std::sin(a)));
                        }
                        for (unsigned int i = 0; i < 32; ++i) line.add_line(base+i, base+(i+1)%32);
                    }
                    if (!line.is_empty()) {
                        GLModel feedback; feedback.init_from(std::move(line));
                        feedback.set_color(m_selection_gesture == SelectionGesture::Protect
                            ? ColorRGBA(0.3f,0.5f,0.95f,1) : ColorRGBA(1,0.55f,0,1));
                        shader->start_using();
                        shader->set_uniform("view_model_matrix", Transform3d::Identity());
                        shader->set_uniform("projection_matrix", Transform3d::Identity());
                        shader->set_uniform("use_uniform_color", true);
                        shader->set_uniform("preview_color_count", 0);
                        shader->set_uniform("preview_lighting", false);
                        feedback.render(shader); shader->stop_using();
                    }
                }
            } else if (!m_render_diagnostics_logged) {
                BOOST_LOG_TRIVIAL(error) << "AI model preview render: vertex-color shader is unavailable";
                m_render_diagnostics_logged = true;
            }
        }
        // Visibility can change through native window messages during a paint.
        // Do not submit a frame for a canvas hidden by navigation or layout.
        if (!m_canvas->IsShownOnScreen())
            return;
        const bool frame_submitted = m_canvas->SwapBuffers();
        if (frame_submitted && m_has_model && m_color_shader != nullptr && !m_pending_library_thumbnail_sha.empty()) {
            // The first visible frame initializes the GL drawing state. Capture once
            // afterwards, rather than before the newly loaded model has been drawn.
            auto artifact_sha = std::move(m_pending_library_thumbnail_sha);
            m_pending_library_thumbnail_sha.clear();
            cache_library_model_thumbnail(artifact_sha);
        }
        if (m_trial_toggle_started) {
            BOOST_LOG_TRIVIAL(info) << "AI color trial frame submitted: enabled=" << m_color_trial_enabled
                << ", elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - *m_trial_toggle_started).count();
            m_trial_toggle_started.reset();
        }
    }

    boost::filesystem::path m_library_thumbnail_root;
    std::string m_pending_library_thumbnail_sha;
    wxGLCanvas* m_canvas {nullptr};
    wxSizerItem* m_overlay_top {nullptr};
    wxSizerItem* m_overlay_bottom {nullptr};
    bool m_retain_surface_attributes {false};
    bool m_exact_surface_display {false};
    std::vector<float> m_surface_vertices;
    std::vector<Vec2f> m_original_surface_colors;
    wxSplitterWindow* m_splitter {nullptr};
    wxPanel* m_preview_host {nullptr};
    wxScrolledWindow* m_controls_scroll {nullptr};
    int m_saved_splitter_sash {360};
    void update_semantic_coloring();
    void finish_semantic_coloring();
    void rebuild_semantic_preview_from_cached_result();
    wxTimer m_semantic_timer;
    std::unique_ptr<ModelSemanticColoring> m_semantic_controller;
    std::function<void(bool)> m_semantic_completion;
    std::shared_ptr<const AI::SemanticColoring::MeshSnapshot> m_semantic_source;
    std::shared_ptr<const PortraitShapeDetails> m_portrait_shapes;
    std::shared_ptr<AI::BeautyLeafEditing> m_leaf_editing;
    std::map<AI::BeautyLeafKey,AI::SemanticColoring::Color> m_manual_leaf_colors;
    nlohmann::json m_pending_leaf_edits;
    uint64_t m_leaf_edit_revision{0};
    bool m_shapes_unlocked {false};
    std::shared_ptr<const AI::SemanticColoring::Analysis> m_semantic_analysis;
    std::shared_ptr<const SemanticRegionEvidence> m_region_evidence;
    std::string m_region_runtime_identity, m_region_evidence_error;
    std::shared_ptr<const SecondaryRegionEvidence> m_secondary_region_evidence;
    std::string m_secondary_region_evidence_error;
    size_t m_semantic_selection_protected {0}, m_semantic_selection_low_confidence {0};
    std::unique_ptr<GLModel> m_semantic_model;
    bool m_semantic_ready {false};
    wxString m_semantic_error;
    FaceColorOverrides m_automatic_face_colors;
    SubfaceColorOverrides m_automatic_subface_colors;
    FaceColorOverrides m_saved_semantic_faces;
    SubfaceColorOverrides m_saved_semantic_subfaces;
    bool m_suppress_semantic_change {false};
    ModelPreviewColorControls* m_color_trial {nullptr};
    std::vector<PreviewPalette::Color> m_trial_palette;
    std::shared_ptr<const PreviewPalette::Histogram> m_trial_histogram;
    bool m_color_trial_enabled {false};
    bool m_beauty_view {false};
    wxColour m_preview_background;
    bool m_workbench_grid_visible {true};
    std::unique_ptr<GLModel> m_workbench_grid;
    std::string m_workbench_grid_geometry;
    bool m_beauty_lighting {true};
    bool m_beauty_original_view {false};
    bool m_gray_view {false};
    bool m_wireframe_view {false};
    wxTimer m_rotation_timer;
    std::chrono::steady_clock::time_point m_rotation_tick;
    double m_pan_x {0.0}, m_pan_y {0.0};
    std::optional<std::chrono::steady_clock::time_point> m_trial_toggle_started;
    wxGLContext* m_context {nullptr};
    std::vector<std::unique_ptr<GLModel>> m_models;
    std::unique_ptr<ModelPreviewTexture> m_texture_model;
    std::unique_ptr<GLModel> m_selection_model;
    std::unique_ptr<GLModel> m_manual_color_model;
    std::vector<Vec3f> m_manual_corner_normals;
    std::function<void(const std::vector<size_t>&)> m_paint_commit;
    std::function<void(bool)> m_beauty_history;
    std::unique_ptr<GLModel> m_protection_model;
    std::unique_ptr<GLModel> m_partition_model;
    std::function<void(size_t)> m_beauty_pick;
    std::unique_ptr<GLShaderProgram> m_color_shader;
    std::shared_ptr<AI::VertexColorRegionEditor> m_region_editor = std::make_shared<AI::VertexColorRegionEditor>();
    wxStaticText* m_region_prepare_status {nullptr};
    wxTimer m_region_prepare_timer;
    std::thread m_region_prepare_worker;
    std::shared_ptr<RegionPreparation> m_region_preparation;
    uint64_t m_region_generation {0};
    bool m_region_prepare_failed {false};
    std::function<void()> m_deferred_selection;
    indexed_triangle_set m_pending_mesh;
    std::vector<RGBA> m_pending_vertex_colors;
    std::unique_ptr<CachedPreview> m_cached_preview;
    boost::filesystem::path m_model_path;
    FileStamp m_model_stamp;
    size_t m_triangle_count {0};
    size_t m_vertex_count {0};
    size_t m_color_count {0};
    std::vector<SelectionState> m_selection_history, m_selection_redo;
    std::vector<uint8_t> m_protected_faces;
    std::vector<uint8_t> m_foreground_faces, m_selection_domain;
    std::optional<SelectionState> m_pending_selection;
    std::string m_geometry_id;
    FaceColorOverrides m_face_color_overrides;
    SelectionGesture m_selection_gesture {SelectionGesture::Lasso};
    std::vector<Vec2d> m_stroke;
    double m_brush_radius {18.0};
    bool m_drawing_selection {false};
    wxTimer m_surface_timer;
    std::thread m_surface_worker;
    std::shared_ptr<SurfaceTask> m_surface_task;
    std::vector<std::string> m_palette;
    BoundingBoxf3 m_bounds;
    Vec3d m_model_dimensions {Vec3d::Zero()};
    wxPoint m_last_mouse;
    wxPoint m_drag_start;
    std::function<void(size_t)> m_selection_changed;
    std::function<void(size_t)> m_color_trial_changed;
    std::function<void(const SelectionState&, const SelectionState&)> m_selection_commit;
    AI::RegionSelectionSettings m_selection_settings;
    AI::RegionSelectionOperation m_selection_operation {AI::RegionSelectionOperation::Replace};
    ColorRGBA m_selection_preview_color {1.0f, 0.55f, 0.0f, 1.0f};
    Transform3d view_rotation() const
    {
        // Generated and imported OBJ files use OrcaSlicer's Z-up convention.
        // Orbit around Z first, then tilt the camera so the build plate stays
        // horizontal instead of presenting portrait bases sideways.
        return Geometry::rotation_transform(m_pitch * Vec3d::UnitX()) *
               Geometry::rotation_transform(m_yaw * Vec3d::UnitZ());
    }

    double fitted_half_height(int width, int height) const
    {
        const double aspect = double(std::max(1, width)) / double(std::max(1, height));
        const Vec3d half_extents = 0.5 * m_bounds.size().cast<double>();
        const Vec3d view_extents = view_rotation().linear().cwiseAbs() * half_extents;
        constexpr double fit_margin = 1.08;
        return std::max(0.001, std::max(view_extents.y(), view_extents.x() / aspect) * fit_margin / m_zoom);
    }

    double m_yaw {-0.65};
    double m_pitch {-1.05};
    double m_zoom {1.0};
    bool m_dragging {false};
    bool m_drag_moved {false};
    bool m_selection_enabled {false};
    bool m_selection_overlay_visible {true};
    bool m_selection_preview_suppressed {false};
    bool m_has_model {false};
    bool m_in_paint {false};
    bool m_paint_diagnostics_logged {false};
    bool m_render_diagnostics_logged {false};
    wxSize m_render_diagnostics_size {0,0};
    double m_render_diagnostics_zoom {0.0};
};

} // namespace Slic3r::GUI
