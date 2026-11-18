#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelLibraryDeletion.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "WorkbenchHistorySnapshot.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Redesign/RedesignMessageDialog.hpp"
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace Slic3r::GUI {

void ModelGenerationPanel::apply_library_removal_projection(bool retain_scan)
{
    Presentation::assert_gui_thread();
    if (m_shutdown) return;
    if (!retain_scan || !m_library_scan_pending) cancel_library_loading();
    m_ui_history_loading = false;
    m_library_entries = m_library_removals->project(m_library_snapshot_entries);
    std::map<std::string, ModelGenerationUIHistoryEntry> previous;
    for (auto& item : m_ui_history_entries) previous.emplace(item.asset_key, std::move(item));
    m_ui_history_entries.clear();
    for (const auto& entry : m_library_entries) {
        const auto key = workbench_history_key(entry);
        if (auto found = previous.find(key); found != previous.end()) {
            m_ui_history_entries.push_back(std::move(found->second)); previous.erase(found);
        } else if (auto saved = m_library_removed_thumbnails.find(key); saved != m_library_removed_thumbnails.end()) {
            m_ui_history_entries.push_back(std::move(saved->second)); m_library_removed_thumbnails.erase(saved);
        } else {
            ModelGenerationUIHistoryEntry item;
            item.asset_key = key; item.job_id = entry.job_id; item.design_only = entry.design_only;
            item.title = entry.title.ToStdString(wxConvUTF8); item.details = entry.details.ToStdString(wxConvUTF8);
            m_ui_history_entries.push_back(std::move(item));
        }
    }
    for (auto& [key, item] : previous) m_library_removed_thumbnails[key] = std::move(item);
    m_ui_history_thumbnails_pending = std::count_if(m_ui_history_entries.begin(), m_ui_history_entries.end(),
        [](const auto& item) { return !item.thumbnail_ready; });
    m_library_removal_refresh.configure([this] { return !m_shutdown && m_page_initialized; }, [this](Presentation::Dirty) {
        refresh_workbench_history();
        refresh_post_generation_workbench();
        publish_workbench_state();
        if (IsShownOnScreen()) refresh_controls(); else publish_ui_state();
    }, "history-removal");
    m_library_removal_refresh.request(Presentation::Dirty::Content | Presentation::Dirty::Layout);
}

void ModelGenerationPanel::notify_library_removal(const ModelLibraryRemovalState::Event& event)
{
    apply_library_removal_projection();
    if (event.phase == ModelLibraryRemovalState::Phase::Committed) m_library_removed_thumbnails.clear();
    if (m_library_removal_listener) m_library_removal_listener(event);
}

bool ModelGenerationPanel::library_removal_affects_current(const ModelLibraryEntry& entry) const
{
    ModelLibraryEntry current; current.model_path = m_displayed_model_path; current.job_id = m_displayed_model_job_id;
    return m_workbench_library_binding.affected(*m_library_removals, entry, current);
}

void ModelGenerationPanel::unload_removed_workbench_model()
{
    Presentation::assert_gui_thread();
    ++m_sequence; ++m_model_generation_session; ++m_history_open_generation;
    invalidate_printer_filament_trial_for_model_change();
    reset_beauty_asset();
    if (m_beauty_transactions) m_beauty_transactions->reset();
    m_native_workbench_context = {};
    m_native_workbench_partition_context = {};
    m_native_workbench_partition_reference = m_native_workbench_partition_report = nullptr;
    m_native_workbench_partition_certificate = m_native_workbench_partition_commit = nullptr;
    m_honor_suite_partition_pending = m_honor_suite_partition_resume_queued = false;
    publish_native_workbench_context();
    if (m_model_preview) {
        m_model_preview->retain_history_rollback(false);
        m_model_preview->clear();
    }
    m_workbench_library_binding = {};
    m_displayed_model_path.clear(); m_displayed_model_job_id.clear();
    m_displayed_model_palette.clear(); m_displayed_model_palette_roles.clear();
    m_artifact_path.clear(); m_artifact_format.clear(); m_artifact_color_encoding.clear();
    m_color_intent_path.clear(); m_color_intent_schema.clear(); m_color_intent_sha256.clear();
    m_reference_image_path.clear(); m_raw_preview_path.clear(); m_preview_path.clear();
    m_reference_image = m_raw_preview_image = m_clean_preview_image = m_style_preview_image = wxImage();
    m_reference_bitmap = m_style_preview_bitmap = wxBitmap();
    m_style_preview_ready = m_model_preview_ready = m_library_model_loaded = m_ready = false;
    m_model_quality = {}; m_visual_quality = {}; m_model_refinement = {};
    m_workbench_check_revision = 0; m_workbench_check_card_visible = false;
    m_workbench_load_error = false; m_ui_model_generation_context = false;
    m_last_imported_model_path.clear();
    if (m_role == Role::WorkbenchAssets) { m_job_id.clear(); m_job_state.clear(); }
    m_model_stats->SetLabel(_L("模型尚未加载"));
    m_model_preview_message->SetLabel(_L("当前历史模型已删除，请选择其他模型。"));
}

bool ModelGenerationPanel::delete_library_entry(const GeneratedModelEntry& selected)
{
    Presentation::assert_gui_thread();
    if (m_shutdown) return false;
    if (!can_replace_model_asset())
        return report_library_deletion_error(_L("当前模型正在处理或有未保存的修改，请完成或取消当前操作后再删除。"));
    const auto entry = selected;
    const auto previous_sequence = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    if (show_redesign_confirmation(wxGetTopLevelParent(this),
            _L("要删除这个历史模型吗？\n\n独立文件会一并清理，其他成果仍需使用的共享文件将保留。此操作无法撤销，也不会取消远端任务。"),
            _L("删除本地资产")) != wxID_YES) return false;
    if (!weak || weak->m_shutdown) return false;
    if (previous_sequence != m_sequence || !can_replace_model_asset())
        return report_library_deletion_error(_L("当前模型状态已变化，删除已取消，请重试。"));
    struct Flow { uint64_t sequence; bool current, unloaded {false}; std::vector<std::string> fallback; };
    auto flow = std::make_shared<Flow>();
    flow->current = library_removal_affects_current(entry);
    if (flow->current) {
        std::vector<std::string> order;
        const auto filtered = filtered_workbench_history_entries(m_library_entries,
            m_workbench_history_search ? m_workbench_history_search->GetValue().Lower() : wxString(), m_workbench_history_filter);
        for (const auto* item : filtered) order.push_back(m_library_removals->key(*item));
        flow->fallback = workbench_history_fallback_keys(order, m_library_removals->key(entry));
    }
    const auto root = ModelGenerationPresentation::generated_models_root();
    const auto entries = m_library_entries;
    const auto event = m_library_removals->begin(entry);
    const auto task = m_library_deletion_scope.begin(entry.job_id, Presentation::TaskLifetime::Transaction);
    m_busy = true;
    flow->sequence = m_sequence;
    cancel_library_loading();
    const auto commit = std::make_shared<std::function<void()>>([weak, task, event, flow] {
        if (!weak || weak->m_shutdown || !weak->m_library_deletion_scope.owns(task) ||
            !weak->m_library_removals->commit(event.request)) return;
        if (flow->current && weak->m_sequence == flow->sequence && weak->library_removal_affects_current(event.entry)) {
            weak->unload_removed_workbench_model();
            flow->sequence = weak->m_sequence; flow->unloaded = true;
        }
        auto committed = event; committed.phase = ModelLibraryRemovalState::Phase::Committed;
        weak->notify_library_removal(committed);
    });
    const bool accepted = Presentation::background_tasks().submit(task,
        [root, entry, entries, commit](const Presentation::TaskContext& context) {
            return ModelLibraryDeletion::remove(root, entry, entries, [&context] { return context.canceled(); },
                [commit] { Presentation::post_gui([commit] { (*commit)(); }); });
        }, [weak, task, event, flow, commit](Presentation::TaskResult<ModelLibraryDeletion::Result> result) {
            if (!weak || weak->m_shutdown || !weak->m_library_deletion_scope.owns(task)) return;
            auto* self = weak.get();
            const bool committed = (result.value && result.value->committed) || self->m_library_removals->committed(event.request);
            if (committed) (*commit)(); // Also handles completion delivered before the commit notification.
            if (!self->m_library_removals->finish(event.request, committed)) return;
            self->m_busy = false;
            auto finished = event;
            finished.phase = committed ? ModelLibraryRemovalState::Phase::Finished : ModelLibraryRemovalState::Phase::Failed;
            self->notify_library_removal(finished);
            if (!committed) {
                if (result.value && result.value->identity_changed && flow->current && self->m_sequence == flow->sequence) {
                    self->m_workbench_check_result = {}; self->m_workbench_check_path.clear();
                    self->m_workbench_load_error = true;
                }
                if (result.canceled || (result.value && result.value->canceled))
                    self->m_status->SetLabel(_L("删除已取消，原文件保留。"));
                else self->report_library_deletion_error(result.value && !result.value->error.empty()
                    ? wxString::FromUTF8(result.value->error) : _L("无法完成历史删除，原文件保留，请重试。"));
            } else {
                if (event.entry.design_only) {
                    if (self->m_job_id == event.entry.job_id) { self->m_selected_image_path.clear(); self->reset(false); }
                    self->m_client.remove(event.entry.job_id, [] {}, [](std::string) {});
                }
                self->m_status->SetLabel(!result.value ? _L("历史记录已删除，部分文件未能清理，文件已保留。") :
                    !result.value->error.empty() ? wxString::FromUTF8(result.value->error) :
                    result.value->retained_dependencies ? _L("历史模型已删除，共享或暂无法确认依赖的文件已保留。") : _L("历史模型及独立文件已删除，无法撤销。"));
                // A different page/asset selected during the transaction owns navigation.
                if (flow->unloaded && self->m_sequence == flow->sequence && self->m_workbench_presented) {
                    for (const auto& key : flow->fallback) {
                        const auto found = std::find_if(self->m_library_entries.begin(), self->m_library_entries.end(),
                            [&](const auto& item) { return self->m_library_removals->key(item) == key; });
                        if (found == self->m_library_entries.end()) continue;
                        const auto next = *found;
                        self->load_library_entry(next.model_path, next.reference_image_path, next.ai_image_path,
                            next.palette, next.palette_roles, next.use_printable_colors, next.color_intent_path,
                            next.color_intent_schema, next.color_intent_sha256, next.job_id, next.title, false, next.repair_index);
                        break;
                    }
                }
            }
            self->load_library_entries(true, true);
        }, Presentation::TaskPriority::Visible);
    if (!accepted) {
        m_library_removals->finish(event.request, false); m_busy = false;
        load_library_entries();
        return report_library_deletion_error(_L("后台队列暂时繁忙，尚未删除文件，请稍后重试。"));
    }
    if (flow->current) {
        const bool checked = m_workbench_check_revision == m_sequence;
        const bool native_context_current = m_native_workbench_context.generation == m_sequence;
        ++m_sequence; ++m_model_generation_session; ++m_history_open_generation;
        if (checked) m_workbench_check_revision = m_sequence;
        if (native_context_current) m_native_workbench_context.generation = m_sequence;
        flow->sequence = m_sequence;
    }
    m_status->SetLabel(_L("正在删除历史模型…"));
    notify_library_removal(event);
    return true;
}
} // namespace Slic3r::GUI
