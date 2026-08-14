#include "OrcaWorkspaceAdapter.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/ObjColorDialog.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Widgets/ProgressDialog.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/Utils/FixModelByCgal.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <regex>
#include <utility>

namespace Slic3r::GUI {
namespace {

bool is_nonempty_obj(const boost::filesystem::path& path)
{
    boost::system::error_code ec;
    return boost::filesystem::is_regular_file(path, ec) && !ec && boost::filesystem::file_size(path, ec) > 0 && !ec &&
           path.extension() == ".obj";
}

bool has_open_mesh_edges(const ModelObject& object)
{
    return std::any_of(object.volumes.begin(), object.volumes.end(), [](const ModelVolume* volume) {
        return volume != nullptr && its_num_open_edges(volume->mesh().its) != 0;
    });
}

bool has_mmu_painting(const ModelObject& object)
{
    return std::any_of(object.volumes.begin(), object.volumes.end(), [](const ModelVolume* volume) {
        return volume != nullptr && volume->is_mm_painted();
    });
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
        if (!in_out.deal_vertex_color || in_out.model == nullptr || in_out.input_colors.empty() || decoded_colours.empty())
            return;

        std::vector<RGBA> source_colours;
        source_colours.reserve(in_out.input_colors.size());
        for (const RGBA& vertex_colour : in_out.input_colors) {
            if (std::none_of(source_colours.begin(), source_colours.end(), [&vertex_colour](const RGBA& colour) {
                    return calc_color_distance(vertex_colour, colour) < 1.0f;
                })) {
                source_colours.emplace_back(vertex_colour);
                if (source_colours.size() > 1)
                    break;
            }
        }
        source_colour_count = source_colours.size();

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
        applied = Model::obj_import_vertex_color_deal(in_out.filament_ids, in_out.first_extruder_id, in_out.model);
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
    AI::PrintablePaletteSnapshot snapshot;
    if (m_plater == nullptr)
        return snapshot;

    snapshot.project_colors = m_plater->get_extruder_colors_from_plater_config();
    static const std::regex color_pattern(R"(^#[0-9A-Fa-f]{6}$)");
    for (size_t slot = 0; slot < snapshot.project_colors.size() && snapshot.valid_slots.size() < 16; ++slot) {
        if (std::regex_match(snapshot.project_colors[slot], color_pattern))
            snapshot.valid_slots.push_back(slot);
    }
    snapshot.compatible_slots = snapshot.valid_slots;

    if (snapshot.valid_slots.size() >= 2 && wxGetApp().preset_bundle != nullptr) {
        const PresetBundle& bundle = *wxGetApp().preset_bundle;
        struct SlotTemperature {
            std::string type;
            int         temperature;
            int         range_low;
            int         range_high;
        };
        std::vector<SlotTemperature> slot_temperatures;
        slot_temperatures.reserve(snapshot.valid_slots.size());
        bool complete = true;
        for (const size_t slot : snapshot.valid_slots) {
            if (slot >= bundle.filament_presets.size()) {
                complete = false;
                break;
            }
            const Preset* preset = bundle.filaments.find_preset(bundle.filament_presets[slot]);
            if (preset == nullptr) {
                complete = false;
                break;
            }
            const auto* types = preset->config.option<ConfigOptionStrings>("filament_type");
            const auto* temperatures = preset->config.option<ConfigOptionInts>("nozzle_temperature");
            const auto* range_lows = preset->config.option<ConfigOptionInts>("nozzle_temperature_range_low");
            const auto* range_highs = preset->config.option<ConfigOptionInts>("nozzle_temperature_range_high");
            if (types == nullptr || types->values.empty() || temperatures == nullptr || temperatures->values.empty() ||
                range_lows == nullptr || range_lows->values.empty() || range_highs == nullptr || range_highs->values.empty()) {
                complete = false;
                break;
            }
            slot_temperatures.push_back(
                {types->get_at(0), temperatures->get_at(0), range_lows->get_at(0), range_highs->get_at(0)});
        }

        if (complete) {
            std::vector<size_t> best {snapshot.valid_slots.front()};
            const uint32_t subset_count = uint32_t(1) << snapshot.valid_slots.size();
            for (uint32_t mask = 1; mask < subset_count; ++mask) {
                std::vector<size_t> slots;
                std::vector<std::string> selected_types;
                std::vector<int> selected_temperatures;
                std::vector<int> selected_lows;
                std::vector<int> selected_highs;
                for (size_t bit = 0; bit < snapshot.valid_slots.size(); ++bit) {
                    if ((mask & (uint32_t(1) << bit)) == 0)
                        continue;
                    slots.push_back(snapshot.valid_slots[bit]);
                    selected_types.push_back(slot_temperatures[bit].type);
                    selected_temperatures.push_back(slot_temperatures[bit].temperature);
                    selected_lows.push_back(slot_temperatures[bit].range_low);
                    selected_highs.push_back(slot_temperatures[bit].range_high);
                }
                if (slots.size() <= best.size())
                    continue;
                if (Print::check_multi_filaments_compatibility(
                        selected_types, selected_temperatures, selected_lows, selected_highs) ==
                    FilamentCompatibilityType::Compatible)
                    best = std::move(slots);
            }
            snapshot.compatible_slots = std::move(best);
        }
    }

    for (const size_t slot : snapshot.compatible_slots) {
        if (slot >= snapshot.project_colors.size())
            continue;
        std::string color = snapshot.project_colors[slot];
        std::transform(color.begin(), color.end(), color.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        if (std::find(snapshot.compatible_colors.begin(), snapshot.compatible_colors.end(), color) ==
            snapshot.compatible_colors.end())
            snapshot.compatible_colors.emplace_back(std::move(color));
    }
    return snapshot;
}

AI::ModelImportResult OrcaWorkspaceAdapter::import_artifact(const AI::ModelImportRequest& request)
{
    AI::ModelImportResult result;
    result.color_mode = request.color_mode;
    if (m_plater == nullptr || !is_nonempty_obj(request.artifact.local_path)) {
        result.outcome = AI::ModelImportOutcome::InvalidArtifact;
        result.error = "The generated OBJ is missing or invalid.";
        return result;
    }

    const boost::filesystem::path& path = request.artifact.local_path;
    Sidebar& workflow = m_plater->sidebar();
    workflow.start_ai_workflow(_L("正在导入 AI 生成模型"));
    workflow.update_ai_workflow_step(Sidebar::AIImportModel, Sidebar::AIWorkflowStatus::Running, _L("读取 OBJ"));

    auto load_model = [this, &path](const char* snapshot_name, AI::ImportColorMode color_mode, bool& colors_applied,
                                    size_t& source_color_count, size_t& mapped_color_count) {
        ObjImportColorFn color_mapper;
        const AI::PrintablePaletteSnapshot palette = printable_palette();
        if (color_mode == AI::ImportColorMode::AutoMap) {
            color_mapper = make_obj_color_mapper(palette.project_colors, palette.compatible_slots, colors_applied,
                                                 source_color_count, mapped_color_count);
        } else if (color_mode == AI::ImportColorMode::ManualMatch) {
            color_mapper = [extruder_colors = palette.project_colors, &colors_applied, &source_color_count,
                            &mapped_color_count](ObjDialogInOut& in_out) {
                colors_applied = false;
                source_color_count = 0;
                mapped_color_count = 0;
                std::vector<RGBA> distinct_source_colors;
                for (const RGBA& color : in_out.input_colors) {
                    if (std::none_of(distinct_source_colors.begin(), distinct_source_colors.end(),
                                     [&color](const RGBA& existing) {
                                         return calc_color_distance(color, existing) < 1.0f;
                                     })) {
                        distinct_source_colors.emplace_back(color);
                        if (distinct_source_colors.size() > 1)
                            break;
                    }
                }
                source_color_count = distinct_source_colors.size();

                ObjColorDialog color_dialog(nullptr, in_out, extruder_colors, Sidebar::should_show_SEMM_buttons());
                if (color_dialog.ShowModal() != wxID_OK) {
                    in_out.filament_ids.clear();
                    return;
                }
                std::vector<unsigned char> used_filaments;
                for (const unsigned char filament_id : in_out.filament_ids) {
                    if (filament_id != 0 &&
                        std::find(used_filaments.begin(), used_filaments.end(), filament_id) == used_filaments.end())
                        used_filaments.emplace_back(filament_id);
                }
                mapped_color_count = used_filaments.size();
                colors_applied = !in_out.filament_ids.empty();
            };
        } else {
            color_mapper = [](ObjDialogInOut&) {};
        }
        Plater::TakeSnapshot snapshot(m_plater, snapshot_name);
        return m_plater->load_files({path}, LoadStrategy::LoadModel, false, std::move(color_mapper));
    };

    const size_t before = m_plater->model().objects.size();
    std::vector<size_t> loaded = load_model("Import AI generated model", request.color_mode, result.colors_applied,
                                            result.source_color_count, result.mapped_color_count);
    if (loaded.empty() || m_plater->model().objects.size() <= before) {
        if (!loaded.empty() && m_plater->model().objects.size() > before)
            m_plater->undo();
        result.outcome = AI::ModelImportOutcome::ImportFailed;
        result.error = "OBJ import failed.";
        workflow.update_ai_workflow_step(Sidebar::AIImportModel, Sidebar::AIWorkflowStatus::Failed,
                                         _L("OBJ 导入失败"));
        workflow.finish_ai_workflow(false, _L("模型导入失败，未开始切片"));
        return result;
    }

    workflow.update_ai_workflow_step(Sidebar::AIImportModel, Sidebar::AIWorkflowStatus::Success);
    workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Running);

    result.color_mapping_collapsed = request.color_mode != AI::ImportColorMode::SingleColor && result.colors_applied &&
                                     result.source_color_count > 1 && result.mapped_color_count < 2;
    result.manual_coloring_required = request.color_mode != AI::ImportColorMode::SingleColor &&
                                      (!result.colors_applied || result.color_mapping_collapsed);
    BOOST_LOG_TRIVIAL(info) << "AI OBJ color import: mode=" << static_cast<int>(request.color_mode)
                            << ", source_colours=" << result.source_color_count
                            << ", mapped_colours=" << result.mapped_color_count
                            << ", applied=" << result.colors_applied
                            << ", collapsed=" << result.color_mapping_collapsed;

    if (request.color_mode == AI::ImportColorMode::ManualMatch) {
        workflow.update_ai_workflow_step(
            Sidebar::AIProcessColors,
            result.manual_coloring_required ? Sidebar::AIWorkflowStatus::Warning : Sidebar::AIWorkflowStatus::Success,
            result.manual_coloring_required ? _L("颜色匹配未完成") : _L("已确认模型颜色与耗材槽"));
    } else if (request.color_mode == AI::ImportColorMode::SingleColor) {
        workflow.update_ai_workflow_step(Sidebar::AIProcessColors, Sidebar::AIWorkflowStatus::Success,
                                         _L("单色导入"));
    } else if (result.colors_applied) {
        workflow.update_ai_workflow_step(Sidebar::AIProcessColors, Sidebar::AIWorkflowStatus::Success,
                                         _L("已映射耗材颜色"));
    } else {
        workflow.update_ai_workflow_step(Sidebar::AIProcessColors, Sidebar::AIWorkflowStatus::Warning,
                                         _L("需要手动上色"));
    }

    result.slice_after_import = request.auto_slice_after_import && !result.manual_coloring_required;
    bool requires_repair = false;
    for (size_t object_index : loaded) {
        if (object_index < m_plater->model().objects.size()) {
            const ModelObject* object = m_plater->model().objects[object_index];
            requires_repair |= object != nullptr && has_open_mesh_edges(*object);
        }
    }
    if (requires_repair) {
        workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Running,
                                         _L("发现开放边，正在自动修复"));
        ProgressDialog progress_dlg(_L("修复 AI 生成模型"), "", 100, find_toplevel_parent(m_plater),
                                    wxPD_AUTO_HIDE | wxPD_APP_MODAL, true);
        std::string repair_error;
        for (size_t object_index : loaded) {
            if (object_index >= m_plater->model().objects.size())
                continue;
            ModelObject* object = m_plater->model().objects[object_index];
            if (object == nullptr || !has_open_mesh_edges(*object))
                continue;
            const bool had_painting = has_mmu_painting(*object);
            std::string object_error;
            const bool completed = fix_model_with_cgal_gui(
                *object, -1, progress_dlg, _L("正在修复模型对象") + ":\n", object_error, true);
            object->ensure_on_bed();
            m_plater->changed_mesh(static_cast<int>(object_index));
            if (!completed || !object_error.empty() || has_open_mesh_edges(*object) ||
                (had_painting && !has_mmu_painting(*object))) {
                repair_error = object_error.empty() ? "网格仍存在非流体问题，或无法保留耗材颜色。" : object_error;
                break;
            }
        }

        if (!repair_error.empty()) {
            m_plater->undo();
            RichMessageDialog fallback(
                m_plater,
                _L("自动网格修复失败，模型不会自动切片。\n\n"
                   "可以将原始 OBJ 手动导入准备页，再使用准备页中的修复工具处理。"),
                _L("自动修复失败"), wxYES_NO | wxICON_WARNING);
            fallback.SetYesNoLabels(_L("手动导入"), _L("取消"));
            if (fallback.ShowModal() != wxID_YES) {
                result.outcome = AI::ModelImportOutcome::RepairFailed;
                result.error = std::move(repair_error);
                result.slice_after_import = false;
                workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Failed,
                                                 _L("自动修复失败"));
                workflow.finish_ai_workflow(false, _L("网格修复失败，未开始切片"));
                return result;
            }

            const size_t manual_before = m_plater->model().objects.size();
            bool manual_colors_applied = false;
            size_t manual_source_color_count = 0;
            size_t manual_mapped_color_count = 0;
            const std::vector<size_t> manual_loaded =
                load_model("Manually import AI generated model", request.color_mode, manual_colors_applied,
                           manual_source_color_count, manual_mapped_color_count);
            if (manual_loaded.empty() || m_plater->model().objects.size() <= manual_before) {
                if (!manual_loaded.empty() && m_plater->model().objects.size() > manual_before)
                    m_plater->undo();
                result.outcome = AI::ModelImportOutcome::ImportFailed;
                result.error = "Manual OBJ import failed after automatic repair.";
                workflow.update_ai_workflow_step(Sidebar::AIImportModel, Sidebar::AIWorkflowStatus::Failed,
                                                 _L("手动导入失败"));
                workflow.finish_ai_workflow(false, _L("模型导入失败，未开始切片"));
                return result;
            }
            result.slice_after_import = false;
            result.manual_repair_required = true;
            workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Warning,
                                             _L("需要手动修复"));
        }
    }

    if (!requires_repair)
        workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Success, _L("封闭网格"));
    else if (!result.manual_repair_required)
        workflow.update_ai_workflow_step(Sidebar::AICheckMesh, Sidebar::AIWorkflowStatus::Success,
                                         _L("自动修复完成"));

    workflow.update_ai_workflow_step(Sidebar::AIArrange, Sidebar::AIWorkflowStatus::Running,
                                     _L("正在放置到打印板"));
    if (result.slice_after_import && wxGetApp().preset_bundle != nullptr) {
        PresetBundle* preset_bundle = wxGetApp().preset_bundle;
        DynamicPrintConfig& print_config = preset_bundle->prints.get_edited_preset().config;
        bool config_changed = false;
        const ConfigOptionBool* independent_support =
            print_config.option<ConfigOptionBool>("independent_support_layer_height");
        if (independent_support != nullptr && independent_support->value) {
            print_config.set_key_value("independent_support_layer_height", new ConfigOptionBool(false));
            config_changed = true;
        }
        const ConfigOptionBool* prime_tower = print_config.option<ConfigOptionBool>("enable_prime_tower");
        if (prime_tower != nullptr && prime_tower->value) {
            print_config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
            config_changed = true;
        }
        if (config_changed) {
            preset_bundle->prints.update_dirty();
            m_plater->update_project_dirty_from_presets();
            m_plater->on_config_change(preset_bundle->full_config());
        }
    }

    result.outcome = AI::ModelImportOutcome::Imported;
    if (!m_on_import_succeeded) {
        result.error = "The Orca workspace navigation callback is unavailable.";
        workflow.update_ai_workflow_step(Sidebar::AIArrange, Sidebar::AIWorkflowStatus::Failed,
                                         _L("无法切换工作区"));
        workflow.finish_ai_workflow(false, _L("模型已导入，但无法继续自动流程"));
        return result;
    }

    m_on_import_succeeded(result.slice_after_import);
    workflow.update_ai_workflow_step(Sidebar::AIArrange, Sidebar::AIWorkflowStatus::Success,
                                     _L("已放置到打印板"));
    if (result.slice_after_import) {
        workflow.update_ai_workflow_step(Sidebar::AISlice, Sidebar::AIWorkflowStatus::Running);
        workflow.update_ai_workflow_step(Sidebar::AIGCode, Sidebar::AIWorkflowStatus::Waiting);
    } else if (result.manual_repair_required) {
        workflow.update_ai_workflow_step(Sidebar::AISlice, Sidebar::AIWorkflowStatus::Warning,
                                         _L("等待手动修复"));
        workflow.finish_ai_workflow(false, _L("模型已导入，请手动修复后切片"));
    } else if (result.manual_coloring_required) {
        workflow.update_ai_workflow_step(Sidebar::AISlice, Sidebar::AIWorkflowStatus::Warning,
                                         _L("等待手动上色"));
        workflow.finish_ai_workflow(false, _L("模型已导入，请手动上色后切片"));
    } else {
        workflow.update_ai_workflow_step(Sidebar::AISlice, Sidebar::AIWorkflowStatus::Waiting,
                                         _L("等待手动切片"));
        workflow.update_ai_workflow_step(Sidebar::AIGCode, Sidebar::AIWorkflowStatus::Waiting,
                                         _L("手动切片后生成"));
        workflow.finish_ai_workflow(true, _L("模型已导入准备页，可手动调整并切片"));
    }
    return result;
}

} // namespace Slic3r::GUI
