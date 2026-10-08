#include "slic3r/GUI/TextureImportDialog.hpp"
#include "OrcaWorkspaceAdapter.hpp"
#include "OrcaModelPreparationPanel.hpp"
#include "OrcaFilamentSelection.hpp"
#include "ModelColorUpdate.hpp"
#include "OrcaPaletteSnapshotBuilder.hpp"
#include "OrcaPrintPaletteSnapshot.hpp"
#include "AIImportSeamRepair.hpp"
#include "OrcaSmartSlicingAdapter.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/ObjColorDialog.hpp"
#include "slic3r/GUI/ModelColorImportResult.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchProjectColor.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <wx/thread.h>
#include <cctype>
#include <limits>
#include <set>
#include <utility>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>
#include <wx/colour.h>

namespace Slic3r::GUI {
namespace {

// Wall time only: scopes that can open modal UI are named explicitly so their
// duration is not mistaken for uninterrupted main-thread computation.
class ImportStageTiming {
public:
    explicit ImportStageTiming(const char* stage) : m_stage(stage) {}
    ~ImportStageTiming()
    {
        BOOST_LOG_TRIVIAL(info) << "AI import timing: stage=" << m_stage
            << ", elapsed_ms=" << std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - m_start).count()
            << ", main_thread=" << wxIsMainThread();
    }
private:
    const char* m_stage;
    const std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

bool is_nonempty_obj(const boost::filesystem::path& path)
{
    boost::system::error_code ec;
    return boost::filesystem::is_regular_file(path, ec) && !ec && boost::filesystem::file_size(path, ec) > 0 && !ec &&
           path.extension() == ".obj";
}

ObjImportColorFn make_obj_color_mapper(const std::vector<std::string>& extruder_colours,
                                       const std::vector<size_t>& allowed_slots, bool& applied,
                                       size_t& source_colour_count, size_t& mapped_colour_count)
{
    struct FilamentColour {
        RGBA          colour;
        unsigned char filament_id;
    };
    std::vector<FilamentColour> decoded_colours;
    decoded_colours.reserve(allowed_slots.size());
    for (const size_t slot : allowed_slots) {
        if (slot >= extruder_colours.size())
            continue;
        const wxColour colour(extruder_colours[slot]);
        if (colour.IsOk() && slot < std::numeric_limits<unsigned char>::max())
            decoded_colours.push_back({convert_to_rgba(colour), static_cast<unsigned char>(slot + 1)});
    }

    return [decoded_colours = std::move(decoded_colours), &applied, &source_colour_count,
            &mapped_colour_count](ObjDialogInOut& in_out) {
        applied = false;
        source_colour_count = 0;
        mapped_colour_count = 0;
        if (in_out.model == nullptr || in_out.input_colors.empty() || decoded_colours.empty())
            return;

        source_colour_count = std::set<RGBA>(in_out.input_colors.begin(), in_out.input_colors.end()).size();

        std::vector<size_t> usage(decoded_colours.size(), 0);
        in_out.filament_ids.clear();
        in_out.filament_ids.reserve(in_out.input_colors.size());
        for (const RGBA& vertex_colour : in_out.input_colors) {
            size_t best_index = 0;
            float best_distance = std::numeric_limits<float>::max();
            for (size_t index = 0; index < decoded_colours.size(); ++index) {
                const float distance = calc_color_distance(vertex_colour, decoded_colours[index].colour);
                if (distance < best_distance) {
                    best_distance = distance;
                    best_index = index;
                }
            }
            in_out.filament_ids.emplace_back(decoded_colours[best_index].filament_id);
            ++usage[best_index];
        }
        mapped_colour_count = std::count_if(usage.begin(), usage.end(), [](size_t count) { return count != 0; });

        const auto dominant = std::max_element(usage.begin(), usage.end());
        in_out.first_extruder_id = decoded_colours[std::distance(usage.begin(), dominant)].filament_id;
        applied = in_out.deal_vertex_color
            ? Model::obj_import_vertex_color_deal(in_out.filament_ids, in_out.first_extruder_id, in_out.model)
            : Model::obj_import_face_color_deal(in_out.filament_ids, in_out.first_extruder_id, in_out.model);
        if (!applied) {
            in_out.filament_ids.clear();
            mapped_colour_count = 0;
        }
    };
}

} // namespace

OrcaWorkspaceAdapter::OrcaWorkspaceAdapter(Plater* plater, ImportSucceededFn on_import_succeeded)
    : m_plater(plater)
    , m_on_import_succeeded(std::move(on_import_succeeded))
{}

AI::PrintablePaletteSnapshot OrcaWorkspaceAdapter::printable_palette() const
{
    if (m_plater == nullptr)
        return {};

    std::vector<std::string> project_colors = m_plater->get_extruder_colors_from_plater_config();
    const PresetBundle* bundle = wxGetApp().preset_bundle;
    PrintColorNozzleRouting nozzle_routing;
    if (bundle) {
        if (const auto* plate = m_plater->get_partplate_list().get_curr_plate()) {
            nozzle_routing.mode = plate->get_real_filament_map_mode(bundle->project_config);
            nozzle_routing.filament_maps = plate->get_real_filament_maps(bundle->project_config);
        }
    }
    return OrcaPrintPaletteSnapshot::capture(bundle,std::move(project_colors),nozzle_routing);
}

TextureImportOptions model_import_color_options(const AI::ModelImportRequest& request)
{
    TextureImportOptions options;
    options.workbench_review = true;
    // Twelve editable target groups leave room for skin, lips and clothing
    // shades. This is a starting point, not a requirement for twelve filaments.
    options.initial_target_colors = 12;
    options.initial_color_smoothing = 0;
    options.physical_filament_limit = 6;
    options.preserve_existing_filaments = true;
    options.allow_unverified_mixed_filaments = false;
    options.z_up = true;
    options.source_units_in_meters = AI::model_artifact_format(request.artifact.local_path) == "glb";
    const auto to_rgb = [](const auto& color) {
        return std::array<size_t, 3> {size_t(std::lround(color[0] * 255.f)),
                                     size_t(std::lround(color[1] * 255.f)),
                                     size_t(std::lround(color[2] * 255.f))};
    };
    for (const auto& override : request.face_color_overrides)
        options.face_color_overrides.push_back({override.first, to_rgb(override.second)});
    if (request.color_trial && request.color_trial->valid()) {
        for (const auto& color : request.color_trial->mapping_colors)
            options.fixed_mapping_palette.push_back(to_rgb(color));
        for (const auto& color : request.color_trial->target_colors)
            options.fixed_palette.push_back(to_rgb(color));
    }
    if(request.matched_colors) {
        options.matched_face_slots=request.matched_colors->face_slots;
        for(const auto& channel:request.matched_colors->palette) {
            TextureFilamentEntry entry;
            entry.kind=TextureFilamentKind::ExistingPhysical;entry.project_config_index=channel.slot;
            entry.color_hex=channel.display_color;entry.type=channel.material_type;
            options.matched_filaments.push_back(std::move(entry));
        }
        for(const auto& recipe:request.matched_colors->mixed_recipes) {
            TextureFilamentEntry entry;entry.kind=TextureFilamentKind::ExistingMixed;
            entry.project_config_index=*recipe.existing_virtual_slot;entry.color_hex=recipe.target_color;
            for(const auto& c:recipe.components) {
                entry.mixed_components.push_back(unsigned(c.slot+1));entry.mixed_ratios.push_back(int(std::lround(c.ratio*100)));
            }
            options.matched_filaments.push_back(std::move(entry));
        }
    }
    return options;
}

std::vector<AI::PhysicalFilamentChannel> OrcaWorkspaceAdapter::project_filament_channels() const
{
    if (!m_plater || !wxGetApp().preset_bundle) return {};
    return workbench_project_channels(wxGetApp().preset_bundle->project_config);
}

AI::ModelImportResult OrcaWorkspaceAdapter::import_workbench_artifact(const AI::ModelImportRequest& request)
{
    if (!m_plater) return {};
    auto result = m_plater->import_workbench_model(request);
    if (result.imported() && m_on_import_succeeded) m_on_import_succeeded();
    return result;
}

std::function<bool()> OrcaWorkspaceAdapter::capture_import_guard() const
{
    if (!m_plater) return [] { return false; };
    const auto revision = OrcaSmartSlicingAdapter(m_plater).current_revision();
    return [plater = m_plater, revision] {
        return OrcaSmartSlicingAdapter(plater).current_revision() == revision;
    };
}

bool OrcaWorkspaceAdapter::set_project_filament_color(size_t slot, const std::string& color,
    const std::function<bool()>& current, std::string& error)
{
    error.clear();
    if (!m_plater || !wxGetApp().preset_bundle || !current || !current()) {
        error = "工程或打印机已变化，请重新选择耗材颜色。";
        return false;
    }
    const auto channels = project_filament_channels();
    auto& bundle = *wxGetApp().preset_bundle;
    auto config = bundle.project_config;
    bool changed = false;
    if (!prepare_workbench_project_color(config, channels, slot, color, changed, error)) return false;
    if (!changed) return true;
    if (!m_plater->apply_project_config(std::move(config), bundle.filament_presets,
            "Change project filament color", error)) return false;
    m_plater->on_config_change(bundle.full_config());
    m_plater->get_partplate_list().invalid_all_slice_result();
    m_plater->update_project_dirty_from_presets();
    bundle.export_selections(*wxGetApp().app_config);
    m_plater->update();
    return true;
}

ObjImportColorFn workbench_obj_color_mapper(Plater* plater, AI::ImportColorMode mode,
    const AI::PrintablePaletteSnapshot& palette, AI::ModelImportResult& result, bool& cancelled)
{
    if (mode == AI::ImportColorMode::AutoMap)
        return make_obj_color_mapper(palette.project_colors, palette.compatible_slots, result.colors_applied,
            result.source_color_count, result.mapped_color_count);
    return [plater, colors = palette.project_colors, &result, &cancelled](ObjDialogInOut& input) {
        input.preserve_input_colors = true;
        result.source_color_count = std::set<RGBA>(input.input_colors.begin(), input.input_colors.end()).size();
        ObjColorDialog dialog(plater, input, colors, Sidebar::should_show_SEMM_buttons());
        if (dialog.ShowModal() != wxID_OK) { input.cancelled = cancelled = true; return; }
        result.mapped_color_count = std::set<unsigned char>(input.filament_ids.begin(), input.filament_ids.end()).size();
        result.colors_applied = input.deal_vertex_color ?
            Model::obj_import_vertex_color_deal(input.filament_ids, input.first_extruder_id, input.model) :
            Model::obj_import_face_color_deal(input.filament_ids, input.first_extruder_id, input.model);
    };
}

AI::ModelImportResult OrcaWorkspaceAdapter::import_artifact(const AI::ModelImportRequest& request)
{
    const ImportStageTiming total_timing("import_wall_including_dialogs");
    AI::ModelImportResult result;
    result.color_mode = request.color_mode;
    result.subface_color_count = request.subface_color_overrides.size();
    TriangleMesh semantic_source_mesh;
    const indexed_triangle_set* semantic_source_its = nullptr;
    if (m_plater == nullptr || !AI::is_model_artifact(request.artifact.local_path)) {
        result.outcome = AI::ModelImportOutcome::InvalidArtifact;
        result.error = "The generated OBJ/GLB is missing or invalid.";
        return result;
    }

    boost::filesystem::path path = request.artifact.local_path;
    std::shared_ptr<const indexed_triangle_set> matched_source;
    std::string matched_geometry_id;
    if(request.matched_colors && request.color_mode==AI::ImportColorMode::NativeMatch) {
        const ImportStageTiming timing("matched_identity_validation");
        const auto& matched=*request.matched_colors;
        const auto current=printable_palette();
        bool palette_matches=matched.palette.size()==current.physical_channels.size();
        for(const auto& saved:matched.palette) {
            const auto found=std::find_if(current.physical_channels.begin(),current.physical_channels.end(),[&](const auto& c){
                return c.slot==saved.slot && c.display_color==saved.display_color && c.material_type==saved.material_type && c.compatible==saved.compatible;
            });
            palette_matches=palette_matches && found!=current.physical_channels.end();
        }
        for(const auto& recipe:matched.mixed_recipes)
            palette_matches=palette_matches && std::any_of(current.mixed_recipes.begin(),current.mixed_recipes.end(),
                [&](const auto& r){return AI::same_native_mixed_recipe(r,recipe);});
        TriangleMesh mesh;ObjInfo colors;
        bool source_valid = matched.valid() && palette_matches &&
            AI::model_artifact_sha256(path) == matched.source_sha256 &&
            AI::load_model_artifact(path, mesh, colors, result.error) &&
            mesh.its.indices.size() == matched.face_slots.size();
        if (source_valid) {
            matched_geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh.its);
            source_valid = matched_geometry_id == matched.geometry_id;
        }
        if (!source_valid) {
            result.outcome=AI::ModelImportOutcome::InvalidArtifact;
            result.error="模型或准备页耗材已变化，请回美颜工作台重新匹配并保存后导入。";return result;
        }
        matched_source=std::make_shared<indexed_triangle_set>(std::move(mesh.its));
        semantic_source_its = matched_source.get();
    }
    if (request.color_mode == AI::ImportColorMode::NativeMatch &&
        (!request.face_color_overrides.empty() || !request.subface_color_overrides.empty())) {
        const ImportStageTiming timing("edited_geometry_validation");
        if (!semantic_source_its) {
            ObjInfo source_colors;
            if (!AI::load_model_artifact(path, semantic_source_mesh, source_colors, result.error)) {
                result.outcome = AI::ModelImportOutcome::InvalidArtifact;
                return result;
            }
            semantic_source_its = &semantic_source_mesh.its;
        }
        // Native matching already decoded and verified this exact source for
        // its face slots. Reuse that mesh for local-edit identity checks.
        const auto identity = matched_source ? matched_geometry_id :
            AI::SurfaceSelectionPersistence::geometry_fingerprint(*semantic_source_its);
        if (identity.empty() || identity != request.face_color_geometry_id) {
            result.outcome = AI::ModelImportOutcome::InvalidArtifact;
            result.error = "The locally edited surface belongs to another model version. Reload the model before importing.";
            return result;
        }
        for (const auto& override : request.face_color_overrides) {
            if (override.first >= semantic_source_its->indices.size() ||
                std::any_of(override.second.begin(), override.second.end(), [](float channel) {
                    return !std::isfinite(channel) || channel < 0.f || channel > 1.f;
                })) {
                result.outcome = AI::ModelImportOutcome::InvalidArtifact;
                result.error = "The locally edited surface contains an invalid face or color. Reload the model before importing.";
                return result;
            }
        }
        for (const auto& override : request.subface_color_overrides) {
            if (override.face_id >= semantic_source_its->indices.size() || override.depth == 0 ||
                override.depth > 2 || unsigned(override.path) >= (1u << (2u * override.depth)) ||
                std::any_of(override.color.begin(), override.color.end(), [](float channel) {
                    return !std::isfinite(channel) || channel < 0.f || channel > 1.f;
                })) {
                result.outcome = AI::ModelImportOutcome::InvalidArtifact;
                result.error = "The locally edited surface contains an invalid subface or color. Reload the model before importing.";
                return result;
            }
        }
    }
    if (AI::model_artifact_format(path) == "glb" && request.color_mode != AI::ImportColorMode::NativeMatch) {
        const ImportStageTiming timing("glb_to_obj_preparation");
        // Feed the same Z-up millimetres and sampled sRGB colors as the AI
        // preview into Orca's existing color matching and undo transaction.
        // Keep the textured GLB and an immutable, local import copy separately.
        const auto original = path;
        const boost::filesystem::path cache_root = Slic3r::temporary_dir();
        bool reused = false;
        if (!AI::prepare_glb_obj_import(original, cache_root, m_verified_glb_import,
                                        path, result.error, reused)) {
            result.outcome = AI::ModelImportOutcome::InvalidArtifact;
            return result;
        }
        BOOST_LOG_TRIVIAL(info) << "AI import preparation: verified_obj_reused=" << reused;
    }
    // Reject geometrically flat input before the native loader starts an undo
    // transaction or changes the active workspace. This is only a zero-extent
    // guard, not a general printability or watertightness decision. Keep the
    // artifact available for preview and editing in the existing GL host.
    {
        TriangleMesh import_geometry;
        ObjInfo import_colors;
        const indexed_triangle_set* geometry = semantic_source_its;
        if (!geometry) {
            if (!AI::load_model_artifact(path, import_geometry, import_colors, result.error)) {
                result.outcome = AI::ModelImportOutcome::InvalidArtifact;
                return result;
            }
            geometry = &import_geometry.its;
        }
        BoundingBoxf3 bounds;
        for(const auto& vertex:geometry->vertices) bounds.merge(vertex.cast<double>());
        const Vec3d extent=bounds.size();
        if (extent.x() <= 0.0 || extent.y() <= 0.0 || extent.z() <= 0.0) {
            result.outcome = AI::ModelImportOutcome::InvalidArtifact;
            result.error = "当前模型有一个方向的厚度为零，不能直接导入切片。模型和编辑已保留，请换用有厚度的模型后重试。";
            return result;
        }
    }

    std::optional<size_t> single_color_filament;
    if (request.color_mode == AI::ImportColorMode::SingleColor) {
        single_color_filament = choose_single_color_filament(m_plater, *m_plater);
        if (!single_color_filament) {
            result.outcome = AI::ModelImportOutcome::Cancelled;
            return result;
        }
    }
    Sidebar& workflow = m_plater->sidebar();
    workflow.start_ai_workflow(_L("正在导入 AI 生成模型"));
    workflow.update_ai_workflow_step(Sidebar::AIImportModel, Sidebar::AIWorkflowStatus::Running, _L("读取模型"));

    bool import_cancelled = false;
    AIImportSeamResult preloaded_seams;
    bool preloaded_seams_checked = false;
    auto load_model = [this, &path, &import_cancelled, &request, &matched_source, &result,
                       &preloaded_seams, &preloaded_seams_checked](const char* snapshot_name, AI::ImportColorMode color_mode, bool& colors_applied,
                                    size_t& source_color_count, size_t& mapped_color_count) {
        const ImportStageTiming timing("load_and_color_including_dialogs");
        import_cancelled = false;
        if (color_mode == AI::ImportColorMode::NativeMatch) {
            // Pass the actual selected GLB version through with its UVs and
            // embedded textures. An OBJ conversion would discard that detail.
            Plater::TakeSnapshot snapshot(m_plater, snapshot_name);
            ModelColorImportResult color_result;
            TextureImportOptions options = model_import_color_options(request);
            options.matched_source=matched_source;
            auto loaded = m_plater->load_files({path}, LoadStrategy::LoadModel, false, nullptr, &color_result, &options);
            import_cancelled = color_result.cancelled;
            if (!color_result.error.empty()) result.error = color_result.error;
            if (color_result.single_color_filament) result.color_mode = AI::ImportColorMode::SingleColor;
            colors_applied = color_result.colors_applied;
            source_color_count = color_result.source_color_count;
            mapped_color_count = color_result.mapped_color_count;
            return loaded;
        }
        ObjImportColorFn color_mapper;
        const AI::PrintablePaletteSnapshot palette = printable_palette();
        if (color_mode == AI::ImportColorMode::AutoMap) {
            color_mapper = make_obj_color_mapper(palette.project_colors, palette.compatible_slots, colors_applied,
                                                 source_color_count, mapped_color_count);
        } else if (color_mode == AI::ImportColorMode::ManualMatch) {
            color_mapper = [extruder_colors = palette.project_colors, &colors_applied, &source_color_count,
                            &mapped_color_count, &import_cancelled](ObjDialogInOut& in_out) {
                colors_applied = false;
                mapped_color_count = 0;
                source_color_count = std::set<RGBA>(in_out.input_colors.begin(), in_out.input_colors.end()).size();
                in_out.preserve_input_colors = true;

                ObjColorDialog color_dialog(nullptr, in_out, extruder_colors, Sidebar::should_show_SEMM_buttons());
                if (color_dialog.ShowModal() != wxID_OK) {
                    in_out.filament_ids.clear();
                    in_out.cancelled = true;
                    import_cancelled = true;
                    return;
                }
                std::vector<unsigned char> used_filaments;
                for (const unsigned char filament_id : in_out.filament_ids) {
                    if (filament_id != 0 &&
                        std::find(used_filaments.begin(), used_filaments.end(), filament_id) == used_filaments.end())
                        used_filaments.emplace_back(filament_id);
                }
                mapped_color_count = used_filaments.size();
                colors_applied = in_out.deal_vertex_color
                    ? Model::obj_import_vertex_color_deal(in_out.filament_ids, in_out.first_extruder_id, in_out.model)
                    : Model::obj_import_face_color_deal(in_out.filament_ids, in_out.first_extruder_id, in_out.model);
                if (!colors_applied)
                    mapped_color_count = 0;
            };
        } else if (color_mode == AI::ImportColorMode::SingleColor && request.face_color_overrides.empty() &&
                   request.subface_color_overrides.empty()) {
            color_mapper = [&preloaded_seams, &preloaded_seams_checked](ObjDialogInOut& in_out) {
                // For this unpainted OBJ, close exact seams before Plater creates
                // the render mesh and raycaster. A cancelled or absent callback
                // still uses the ordinary post-load check below.
                if (!in_out.model || in_out.model->objects.size() != 1 || !in_out.model->objects.front()) return;
                preloaded_seams = stitch_ai_import_seams(*in_out.model->objects.front());
                preloaded_seams_checked = true;
            };
        } else {
            color_mapper = [](ObjDialogInOut&) {};
        }
        Plater::TakeSnapshot snapshot(m_plater, snapshot_name);
        return m_plater->load_files({path}, LoadStrategy::LoadModel, false, std::move(color_mapper));
    };

    size_t update_existing = size_t(-1);
    bool arrange_copy = false;
    if (request.color_mode == AI::ImportColorMode::NativeMatch) {
        const auto& objects = m_plater->model().objects;
        for (size_t i = 0; i < objects.size(); ++i) {
            if (!objects[i]) continue;
            boost::system::error_code error;
            bool same_source = !objects[i]->input_file.empty() &&
                boost::filesystem::equivalent(objects[i]->input_file, path, error) && !error;
            if (!same_source && objects[i]->volumes.size() == 1)
                same_source = same_generated_artifact_name(objects[i]->volumes.front()->source.input_file, path.string());
            if (!same_source) continue;
            RichMessageDialog repeat(m_plater,
                _L("工程中已有这份模型。更新配色会替换它的耗材分配，保留位置、比例及其他设置；可撤销。\n"
                   "新增副本会使用 Orca 自动摆放。") + "\n\n" + wxString::FromUTF8(objects[i]->name),
                _L("同一模型再次导入"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
            repeat.SetButtonLabel(wxID_YES, _L("更新配色"));
            repeat.SetButtonLabel(wxID_NO, _L("新增并摆放"));
            const int answer = repeat.ShowModal();
            if (answer == wxID_CANCEL) {
                result.outcome = AI::ModelImportOutcome::Cancelled;
                workflow.finish_ai_workflow(false, _L("已取消，本次未导入；已有模型保留。"), true);
                return result;
            }
            if (answer == wxID_YES) update_existing = i;
            else arrange_copy = true;
            break;
        }
    }
    const size_t before = m_plater->model().objects.size();
    const size_t before_snapshot = m_plater->undo_redo_stack_main().active_snapshot_time();
    std::vector<size_t> loaded = load_model("Import AI generated model", request.color_mode, result.colors_applied,
                                            result.source_color_count, result.mapped_color_count);
    if (loaded.empty() || m_plater->model().objects.size() <= before) {
        // The loader snapshots before opening color confirmation, even when
        // cancellation adds no object. Roll back only if this call created a
        // snapshot; otherwise undo would consume an existing user operation.
        if (m_plater->undo_redo_stack_main().active_snapshot_time() != before_snapshot)
            m_plater->undo();
        result.outcome = import_cancelled ? AI::ModelImportOutcome::Cancelled : AI::ModelImportOutcome::ImportFailed;
        if (result.error.empty()) result.error = import_cancelled ? "OBJ import cancelled." : "OBJ import failed.";
        workflow.update_ai_workflow_step(Sidebar::AIImportModel,
            import_cancelled ? Sidebar::AIWorkflowStatus::Warning : Sidebar::AIWorkflowStatus::Failed,
            import_cancelled ? _L("已取消导入。") : _L("OBJ 导入失败"));
        workflow.finish_ai_workflow(false, import_cancelled ? _L("已取消，本次未导入；已有模型保留。") : _L("模型导入失败"), import_cancelled);
        return result;
    }

    if (single_color_filament) {
        for (size_t index : loaded)
            if (index < m_plater->model().objects.size()) {
                ModelObject* object = m_plater->model().objects[index];
                object->config.set_key_value("extruder", new ConfigOptionInt(int(*single_color_filament + 1)));
                for (auto* volume : object->volumes)
                    volume->config.set_key_value("extruder", new ConfigOptionInt(int(*single_color_filament + 1)));
            }
        result.colors_applied = true;
        result.mapped_color_count = 1;
    }
    bool subface_import_incomplete = false;
    if (result.color_mode != AI::ImportColorMode::SingleColor && !request.subface_color_overrides.empty()) {
        std::string subface_error;
        ModelVolume* volume = nullptr;
        if (!semantic_source_its || loaded.size() != 1 || loaded.front() >= m_plater->model().objects.size()) {
            subface_error = "Semantic subfaces require one unchanged imported model.";
        } else if (ModelObject* object = m_plater->model().objects[loaded.front()]) {
            for (ModelVolume* candidate : object->volumes) {
                if (!candidate || !candidate->is_model_part()) continue;
                if (volume != nullptr) { volume = nullptr; break; }
                volume = candidate;
            }
            if (volume == nullptr) subface_error = "Semantic subfaces require one unchanged model-part volume.";
        } else {
            subface_error = "The imported model is unavailable for semantic subfaces.";
        }
        if (volume != nullptr) {
            auto painting = volume->mmu_segmentation_facets.get_data();
            if (apply_subface_color_overrides(*semantic_source_its, volume->mesh().its, painting,
                    request.face_color_overrides, request.subface_color_overrides, subface_error)) {
                volume->mmu_segmentation_facets.set_data(std::move(painting));
                result.subface_colors_applied = true;
                result.colors_applied = true;
                m_plater->changed_mesh(int(loaded.front()));
            }
        }
        subface_import_incomplete = !result.subface_colors_applied;
        if (subface_import_incomplete)
            BOOST_LOG_TRIVIAL(warning) << "Semantic subfaces retained safe whole-face colors: " << subface_error;
    }

    workflow.update_ai_workflow_step(Sidebar::AIImportModel, Sidebar::AIWorkflowStatus::Success);
    workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Running);

    auto update_color_status = [&]() {
        result.color_mapping_collapsed = result.color_mode != AI::ImportColorMode::SingleColor && result.colors_applied &&
                                         result.source_color_count > 1 && result.mapped_color_count < 2;
        result.manual_coloring_required = result.color_mode != AI::ImportColorMode::SingleColor &&
                                          (!result.colors_applied || result.color_mapping_collapsed ||
                                           subface_import_incomplete);
        BOOST_LOG_TRIVIAL(info) << "AI OBJ color import: mode=" << static_cast<int>(result.color_mode)
                                << ", source_colours=" << result.source_color_count
                                << ", mapped_colours=" << result.mapped_color_count
                                << ", applied=" << result.colors_applied
                                << ", collapsed=" << result.color_mapping_collapsed;

        if (result.color_mode == AI::ImportColorMode::ManualMatch ||
            result.color_mode == AI::ImportColorMode::NativeMatch) {
            workflow.update_ai_workflow_step(
                Sidebar::AIProcessColors,
                result.manual_coloring_required ? Sidebar::AIWorkflowStatus::Warning : Sidebar::AIWorkflowStatus::Success,
                result.manual_coloring_required ? _L("颜色匹配未完成") : _L("已确认模型颜色与耗材槽"));
        } else if (result.color_mode == AI::ImportColorMode::SingleColor) {
            workflow.update_ai_workflow_step(Sidebar::AIProcessColors, Sidebar::AIWorkflowStatus::Success,
                                             _L("单色导入"));
        } else if (result.colors_applied) {
            workflow.update_ai_workflow_step(Sidebar::AIProcessColors, Sidebar::AIWorkflowStatus::Success,
                                             _L("已映射耗材颜色"));
        } else {
            workflow.update_ai_workflow_step(Sidebar::AIProcessColors, Sidebar::AIWorkflowStatus::Warning,
                                             _L("需要手动上色"));
        }
    };
    update_color_status();

    const bool prechecked_single_object = preloaded_seams_checked && loaded.size() == 1;
    bool requires_repair = prechecked_single_object && preloaded_seams.has_open_edges;
    size_t stitched_volumes = prechecked_single_object ? preloaded_seams.stitched_volumes : 0;
    if (!prechecked_single_object) {
        for (size_t object_index : loaded) {
            if (object_index >= m_plater->model().objects.size()) continue;
            ModelObject* object = m_plater->model().objects[object_index];
            if (!object) continue;
            const ImportStageTiming timing("mesh_check_and_seam_repair_per_object");
            const auto seam_result = stitch_ai_import_seams(*object);
            const size_t stitched = seam_result.stitched_volumes;
            if (stitched) {
                stitched_volumes += stitched;
                m_plater->changed_mesh(int(object_index));
            }
            requires_repair |= seam_result.has_open_edges;
        }
    }
    if (requires_repair) {
        // CGAL repair has no interruptible per-mesh operation. Do not make it
        // an unavoidable modal import step or rebuild confirmed color surfaces.
        // Keep the import transaction intact; repair is an explicit next action.
        result.manual_repair_required = true;
        workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Warning,
            _L("模型存在开放边；已保留几何与颜色，请在准备页检查并按需修复"));
    } else {
        workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Success,
            stitched_volumes ? _L("已闭合纹理接缝，保留已选颜色") : _L("封闭网格"));
    }

    if (update_existing != size_t(-1) && (loaded.size() != 1 || result.manual_repair_required)) {
        wxMessageBox(_L("本次结果需要分部件或修复处理，无法只更新原模型配色。已保留原模型，并将新结果作为副本摆放。"),
            _L("保留原模型"), wxOK | wxICON_INFORMATION, m_plater);
        arrange_copy = true;
    }
    if (update_existing != size_t(-1) && loaded.size() == 1 && !result.manual_repair_required) {
        auto* previous = m_plater->model().objects[update_existing];
        auto* imported = m_plater->model().objects[loaded.front()];
        if (update_compatible_model_colors(*previous, *imported)) {
            Plater::SuppressSnapshots suppress(m_plater);
            wxGetApp().obj_list()->delete_from_model_and_list(itObject, int(loaded.front()), -1);
            m_plater->changed_mesh(int(update_existing));
            loaded = {update_existing};
        } else {
            wxMessageBox(_L("模型网格或部件已改变，无法安全地只替换配色。已保留原模型，并将本次结果作为副本摆放。"),
                _L("保留原模型"), wxOK | wxICON_INFORMATION, m_plater);
            arrange_copy = true;
        }
    }

    workflow.update_ai_workflow_step(Sidebar::AIArrange, Sidebar::AIWorkflowStatus::Running,
                                     _L("正在放置到打印板"));
    result.outcome = AI::ModelImportOutcome::Imported;
    if (!m_on_import_succeeded) {
        result.error = "The Orca workspace navigation callback is unavailable.";
        workflow.update_ai_workflow_step(Sidebar::AIArrange, Sidebar::AIWorkflowStatus::Failed,
                                          _L("无法切换工作区"));
        workflow.finish_ai_workflow(false, _L("模型已导入，但无法切换到准备页"));
        return result;
    }

    m_on_import_succeeded();
    if (request.suggest_base_preparation) {
        auto* preparation = dynamic_cast<OrcaModelPreparationPanel*>(
            wxWindow::FindWindowByName("ai_model_preparation", m_plater));
        if (preparation && loaded.size() == 1 && loaded.front() < m_plater->model().objects.size())
            preparation->suggest_base_for_object(m_plater->model().objects[loaded.front()]->id().id);
        else
            wxMessageBox(_L("模型已加入工程，尚未添加底座。请在准备页完整选中一个模型，再展开尺寸与底座并确认应用。"),
                _L("确认底座准备"), wxOK | wxICON_INFORMATION, m_plater);
    }
    if (arrange_copy) {
        // ArrangeJob runs on the UI worker. Keep the workflow active until its
        // finalize() callback applies the transforms, otherwise the panel
        // reports success while the imported copy can still overlap the
        // existing model.
        if (!m_plater->arrange_for_ai_workflow())
            return result;
        return result;
    }
    workflow.update_ai_workflow_step(Sidebar::AIArrange, Sidebar::AIWorkflowStatus::Success,
                                     _L("已放置到打印板"));
    // Keep the workflow active while the user performs the manual repair,
    // colour assignment and slice. Plater's process-completed event owns the
    // final Slice/G-code states; finishing here would make those later events
    // invisible to the workflow panel.
    workflow.update_ai_workflow_step(Sidebar::AISlice, Sidebar::AIWorkflowStatus::Waiting,
                                     result.manual_repair_required
                                         ? _L("修复模型后手动切片")
                                         : result.manual_coloring_required ? _L("完成上色后手动切片")
                                                                           : _L("等待手动切片"));
    workflow.update_ai_workflow_step(Sidebar::AIGCode, Sidebar::AIWorkflowStatus::Waiting,
                                     _L("手动切片后生成"));
    return result;
}

} // namespace Slic3r::GUI
