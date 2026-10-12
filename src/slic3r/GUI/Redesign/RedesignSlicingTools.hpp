#pragma once

#include "../AI/Orca/SlicingModelPalette.hpp"
#include "../AI/Orca/OrcaWorkspaceAdapter.hpp"
#include "../AI/Orca/FilamentColorPack.hpp"
#include "../AI/ModelGeneration/WorkbenchModelInspection.hpp"
#include "../Plater.hpp"
#include "../GUI_App.hpp"
#include <future>
#include <atomic>
#include <wx/timer.h>

namespace Slic3r::GUI {

// Local project tools for the slicing page. Background work owns frozen inputs;
// the UI thread revalidates the whole workspace before its single Undo commit.
class RedesignSlicingTools final {
public:
    RedesignSlicingTools(Plater& plater, std::function<void()> changed)
        : m_plater(plater), m_changed(std::move(changed)) {
        m_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { finish(); });
    }
    ~RedesignSlicingTools() { m_timer.Stop(); m_cancel = true; if (m_work.valid()) m_work.wait(); }
    bool busy() const { return m_work.valid(); }
    const std::vector<std::string>& palette() const { return m_palette; }
    const WorkbenchCheckResult& check() const { return m_check; }
    const std::string& message() const { return m_message; }
    const std::vector<FilamentColorPack>& packs() const { return m_packs; }
    std::string editability_reason() {
        if (busy()) return "正在处理模型，请稍候。";
        if (m_plater.is_background_process_slicing() || !m_plater.get_ui_job_worker().is_idle())
            return "请先完成当前切片或模型处理。";
        return target() ? std::string{} : "请选择当前打印板上仅含一个模型部件的对象，再修改配色。";
    }
    bool editable() { return editability_reason().empty(); }

    void refresh() {
        if (m_check_guard && !m_check_guard() && m_check.status != WorkbenchCheckStatus::Running) {
            m_check = {}; m_check_guard = {};
        }
        m_palette.clear();
        const auto physical = OrcaWorkspaceAdapter(&m_plater, {}).printable_palette();
        auto* volume = target();
        if (!volume) return;
        const auto& data = volume->mmu_segmentation_facets.get_data();
        std::vector<int> slots;
        if (data.triangles_to_split.size() < volume->mesh().facets_count() || data.used_states[0])
            slots.push_back(SlicingModelPalette::base_extruder(*volume)-1);
        for (const auto state : TriangleSelector::extract_used_facet_states(data))
            if (int(state) > 0) slots.push_back(int(state)-1);
        std::sort(slots.begin(), slots.end()); slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
        for (int slot : slots) {
            const auto found = std::find_if(physical.physical_channels.begin(), physical.physical_channels.end(),
                [slot](const auto& channel) { return channel.slot == size_t(slot); });
            if (found != physical.physical_channels.end()) m_palette.push_back(found->display_color);
        }
    }

    bool request_check() {
        if (busy() || m_plater.is_background_process_slicing()) return false;
        auto* plate = m_plater.get_partplate_list().get_curr_plate();
        if (!plate) return false;
        std::vector<TriangleMesh> meshes;
        const auto& objects = m_plater.model().objects;
        for (size_t i = 0; i < objects.size(); ++i) {
            const auto* object = objects[i]; if (!object) continue;
            bool on_plate = false;
            for (size_t j = 0; j < object->instances.size(); ++j)
                on_plate |= plate->contain_instance(int(i), int(j));
            if (on_plate) for (const auto* volume : object->volumes)
                if (volume && volume->is_model_part()) meshes.push_back(volume->mesh());
        }
        if (meshes.empty()) return false;
        m_guard = OrcaWorkspaceAdapter(&m_plater, {}).capture_import_guard();
        m_check.status = WorkbenchCheckStatus::Running;
        m_work = std::async(std::launch::async, [this, meshes = std::move(meshes)] {
            Result result; result.is_check = true; result.check.status = WorkbenchCheckStatus::Normal;
            for (const auto& mesh : meshes) {
                const auto checked = inspect_workbench_mesh(mesh, [this] { return m_cancel.load(); });
                if (checked.status == WorkbenchCheckStatus::Failed) { result.check = checked; break; }
                result.check.boundary_edges += checked.boundary_edges;
                result.check.nonmanifold_edges += checked.nonmanifold_edges;
                result.check.degenerate_faces += checked.degenerate_faces;
                result.check.duplicate_faces += checked.duplicate_faces;
                result.check.inconsistent_edges += checked.inconsistent_edges;
                if (checked.status == WorkbenchCheckStatus::Invalid) result.check.status = checked.status;
                else if (checked.status == WorkbenchCheckStatus::Attention && result.check.status == WorkbenchCheckStatus::Normal)
                    result.check.status = checked.status;
            }
            const auto& c = result.check;
            if (c.status != WorkbenchCheckStatus::Failed)
                result.check.summary = "开放边 " + std::to_string(c.boundary_edges) + "；非流形边 " +
                    std::to_string(c.nonmanifold_edges) + "；退化面 " + std::to_string(c.degenerate_faces) +
                    "；重复面 " + std::to_string(c.duplicate_faces) + "；方向不一致 " +
                    std::to_string(c.inconsistent_edges) + "。壁厚、悬垂和自交未检查。";
            return result;
        });
        m_timer.Start(100); m_changed(); return true;
    }

    bool request_colors(bool portrait, int source, size_t count,
        std::vector<PreviewPalette::Color> manual = {}) {
        if (!editable()) return false;
        auto* volume = target();
        if (!volume) return false;
        m_guard = OrcaWorkspaceAdapter(&m_plater, {}).capture_import_guard();
        m_target_id = volume->id();
        const auto geometry = AI::SurfaceSelectionPersistence::geometry_fingerprint(volume->mesh().its);
        const auto live = volume->mmu_segmentation_facets.get_data();
        const int base = SlicingModelPalette::base_extruder(*volume);
        // Own recoloring can be expanded again. A native paint edit establishes
        // a new source; Undo/Redo of our committed states retains its source.
        const bool known = std::any_of(m_history.begin(), m_history.end(), [&](const auto& previous) {
            return previous.first == live && previous.second == base;
        });
        if (m_source_id != m_target_id || m_geometry != geometry || !known) {
            m_source_id = m_target_id; m_geometry = geometry; m_source = live; m_base = base;
            m_history.clear(); m_history.emplace_back(live, base);
        }
        const auto physical = OrcaWorkspaceAdapter(&m_plater, {}).printable_palette();
        std::vector<PreviewPalette::Color> colors(physical.project_colors.size());
        std::vector<bool> compatible(colors.size(), false);
        for (size_t i = 0; i < colors.size(); ++i) colors[i] = rgb(physical.project_colors[i]);
        for (const auto& channel : physical.physical_channels)
            if (channel.slot < compatible.size()) compatible[channel.slot] = channel.compatible;
        count = std::clamp(count, size_t(1), portrait ? size_t(6) : size_t(32));
        auto packs = m_packs;
        m_work = std::async(std::launch::async, [this, mesh = volume->mesh(), painting = m_source, base = m_base,
            colors = std::move(colors), compatible = std::move(compatible), portrait, source, count,
            packs = std::move(packs), manual = std::move(manual)] {
            Result result;
            try {
                SlicingModelPalette::Selector selector(mesh); selector.deserialize(painting);
                const auto weights = selector.areas(base);
                PreviewPalette::Histogram histogram;
                for (size_t i = 0; i < colors.size() && i < weights.size(); ++i) {
                    const auto& c = colors[i];
                    histogram.add((uint32_t(c[0]*255+.5f)<<16) | (uint32_t(c[1]*255+.5f)<<8) | uint32_t(c[2]*255+.5f), weights[i]);
                }
                std::vector<PreviewPalette::Color> preferred;
                if (source >= 3 && size_t(source-3) < packs.size()) {
                    for (const auto& hex : packs[source-3].colors) preferred.push_back(rgb(hex));
                } else if (portrait && source == 0) {
                    for (const auto& hex : young_portrait_color_pack().colors) preferred.push_back(rgb(hex));
                }
                const auto targets = SlicingModelPalette::select_targets(histogram, colors, compatible,
                    count, source, manual, preferred);
                result.success = SlicingModelPalette::prepare(mesh, painting, base, colors, compatible,
                    targets, result.painting, result.used, result.error);
            } catch (const std::exception& error) { result.error = error.what(); }
            return result;
        });
        m_timer.Start(100); m_changed(); return true;
    }
    static PreviewPalette::Color rgb(const std::string& hex) {
        const wxColour color(wxString::FromUTF8(hex));
        if (!color.IsOk()) return {0.f, 0.f, 0.f};
        return {color.Red()/255.f, color.Green()/255.f, color.Blue()/255.f};
    }
private:
    struct Result {
        bool is_check{false}, success{false};
        WorkbenchCheckResult check;
        LocalPrintColorApplication::PreparedPainting painting;
        std::vector<size_t> used;
        std::string error;
    };
    ModelVolume* target() {
        auto* plate = m_plater.get_partplate_list().get_curr_plate(); if (!plate) return nullptr;
        const int selected = m_plater.get_selection().get_object_idx();
        ModelObject* target_object = nullptr;
        const auto& objects = m_plater.model().objects;
        for (size_t i = 0; i < objects.size(); ++i) {
            auto* object = objects[i]; if (!object || (selected >= 0 && int(i) != selected)) continue;
            bool on_plate = false;
            for (size_t j = 0; j < object->instances.size(); ++j) on_plate |= plate->contain_instance(int(i), int(j));
            if (!on_plate) continue;
            if (target_object) return nullptr;
            target_object = object;
        }
        ModelVolume* part = nullptr;
        if (target_object) for (auto* volume : target_object->volumes) if (volume && volume->is_model_part()) {
            if (part) return nullptr;
            part = volume;
        }
        return part;
    }
    void finish() {
        if (!m_work.valid() || m_work.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        m_timer.Stop();
        Result result;
        try { result = m_work.get(); } catch (const std::exception& error) { result.error = error.what(); }
        if (!result.is_check && m_check.status == WorkbenchCheckStatus::Running) {
            m_check.status = WorkbenchCheckStatus::Failed; m_check.summary = result.error;
        }
        if (!m_guard || !m_guard()) {
            m_check = {}; m_message = "工程已变化，本次操作未应用，请重新操作。";
        } else if (result.is_check) {
            m_check = std::move(result.check); m_check_guard = m_guard;
        } else if (!result.success) m_message = result.error;
        else {
            auto* volume = target();
            LocalPrintColorCommit::Prepared commit;
            std::string error;
            if (!volume || volume->id() != m_target_id || !LocalPrintColorCommit::prepare(*volume,
                std::move(result.painting), volume->source.input_file.empty() ? "project-model" : volume->source.input_file,
                commit, error) || !LocalPrintColorCommit::validate(*volume, commit, error))
                m_message = error.empty() ? "当前模型已变化，配色未应用。" : error;
            else if (volume->mmu_segmentation_facets.get_data() == commit.painting.data &&
                     SlicingModelPalette::base_extruder(*volume) == commit.painting.base_extruder)
                m_message = "当前配色已是所选方案。";
            else {
                commit.source = volume->source.input_file;
                m_history.emplace_back(commit.painting.data, commit.painting.base_extruder);
                if (m_history.size() > 33) m_history.erase(m_history.begin()+1);
                Plater::TakeSnapshot snapshot(&m_plater, "Change model palette");
                LocalPrintColorCommit::commit(*volume, commit);
                const auto& objects = m_plater.model().objects;
                const auto found = std::find(objects.begin(), objects.end(), volume->get_object());
                if (found != objects.end()) m_plater.changed_mesh(int(found-objects.begin()));
                m_plater.get_partplate_list().invalid_all_slice_result();
                m_plater.update();
                m_message = "已更新模型区域配色，可撤销。实际使用 " + std::to_string(result.used.size()) +
                    " 个兼容耗材色；物理耗材配置保留。";
            }
        }
        refresh(); m_changed();
    }
    Plater& m_plater;
    std::function<void()> m_changed;
    std::function<bool()> m_guard, m_check_guard;
    wxTimer m_timer;
    std::future<Result> m_work;
    std::atomic<bool> m_cancel{false};
    ObjectID m_target_id, m_source_id;
    std::string m_geometry, m_message;
    TriangleSelector::TriangleSplittingData m_source;
    int m_base{1};
    std::vector<std::pair<TriangleSelector::TriangleSplittingData, int>> m_history;
    std::vector<std::string> m_palette;
    std::vector<FilamentColorPack> m_packs{load_filament_color_packs()};
    WorkbenchCheckResult m_check;
};
} // namespace Slic3r::GUI
