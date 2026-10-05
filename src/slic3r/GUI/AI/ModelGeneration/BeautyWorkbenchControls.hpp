#pragma once

#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include "slic3r/GUI/AI/Model/BeautyPuzzle.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include <boost/filesystem/path.hpp>
#include <wx/panel.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

class Button;
class wxChoice;
class wxBitmapComboBox;
class wxSlider;
class wxStaticText;
class ComboBox;

namespace Slic3r::GUI {
class ModelPreview3D;
class WorkbenchSwitch;

// Adapter for the existing result-window finishing controls. It deliberately
// does not own or replace ModelPreview3D: selection, semantic preview and
// manual face overrides stay in the current window implementation.
class BeautyWorkbenchControls final : public wxPanel {
public:
    BeautyWorkbenchControls(wxWindow* parent, ModelPreview3D* preview,
                            AI::IPrintablePaletteProvider& palette,
                            std::function<void()> layout_changed);
    ~BeautyWorkbenchControls() override;

    void synchronize(const boost::filesystem::path& source, bool editable, bool visible,
                     bool processing = false, bool candidate_ready = false);
    void synchronize_preview_options();
    void mark_saved();
    void attach_actions(wxWindow* footer);
    void attach_parameters(wxWindow* surface, wxWindow* strength_label, wxWindow* strength);
    void set_history_permissions(bool undo, bool redo);
    void request_secondary_partition(const boost::filesystem::path& source);
    void set_dirty(bool dirty) { m_dirty = dirty; update_text(); }
    bool has_changes() const { return m_dirty; }
    bool ready() const { return m_ready; }
    bool preview_protected_details() const;
    bool partitioning() const { return bool(m_partition_task); }
    void cancel_partition();
    bool geometry_deform_selected() const;
    int operation_index() const;
    double displacement_mm() const;
    double falloff_mm() const;
    void prepare_options(AI::ModelFinishingOptions& options,
                         const AI::SurfaceSelectionPersistence::SelectionState& selection) const;
    static nlohmann::json accepted_document(const AI::ModelFinishingOptions& options,
                                            const std::string& output_geometry);
    nlohmann::json partition_metadata() const;
    static void prepare_import(const boost::filesystem::path&, AI::ModelImportRequest&) {}

    std::function<void()> on_boundary_adjust;
    // Beauty operation index: recolor, soften, repair, cleanup.
    std::function<void(int)> on_operation_changed;
    std::function<void(bool)> on_original_view_changed;
    std::function<void()> on_undo;
    std::function<void()> on_redo;
    std::function<void()> on_save;
    std::function<size_t(const std::string&)> on_auto_match;
    std::function<size_t(const std::string&)> on_auto_detail_match;
    std::function<void()> on_import_secondary_evidence;
    std::function<void()> on_regenerate_readonly_evidence;
    std::function<bool()> on_reoptimize;
    std::function<void()> on_preview;
    std::function<void()> on_accept;
    std::function<void()> on_discard;
    std::function<void()> on_cancel;
    std::function<bool()> on_partition_started;
    std::function<void(bool)> on_partition_finished;
    std::function<void()> on_pick_mode;
    std::function<void(const std::string&, std::function<void()>, std::function<void()>)> on_record;
    std::function<std::vector<std::string>()> on_available_colors;
    std::function<void(size_t)> on_color_slot_changed;

private:
    void update_text();
    void wrap_status(wxStaticText* label);
    void update_secondary_details();
    void start_partition(const nlohmann::json& saved = {});
    void finish_partition();
    void clear_partition(const AI::SurfaceSelectionPersistence::SelectionState& selection);
    void select_piece(size_t face);
    void apply_partition_selection();
    void restore_partition(const AI::BeautyPuzzle& puzzle, uint32_t selected);
    struct PartitionTask {
        std::atomic<bool> canceled {false};
        std::atomic<bool> done {false};
        std::shared_ptr<const AI::BeautySurface> surface;
        AI::BeautyPuzzle result;
        AI::SurfaceSelectionPersistence::SelectionState before_selection;
        nlohmann::json saved;
        bool restoring {false};
        std::string error;
        std::string geometry;
    };
    wxWeakRef<ModelPreview3D> m_preview;
    AI::IPrintablePaletteProvider& m_palette;
    std::function<void()> m_layout_changed;
    wxStaticText* m_status {nullptr};
    wxStaticText* m_secondary_status {nullptr};
    Button* m_import_secondary {nullptr};
    Button* m_regenerate_evidence {nullptr};
    WorkbenchSwitch* m_preview_protected {nullptr};
    wxPanel* m_preview_protected_row {nullptr};
    ComboBox* m_operation {nullptr};
    wxStaticText* m_operation_label {nullptr};
    Button* m_details_toggle {nullptr};
    ComboBox* m_color_slot {nullptr};
    std::vector<std::string> m_palette_colors;
    ComboBox* m_auto_region {nullptr};
    ComboBox* m_auto_detail {nullptr};
    std::vector<std::string> m_auto_detail_ids;
    Button* m_auto_match {nullptr};
    Button* m_reoptimize {nullptr};
    Button* m_boundary {nullptr};
    Button* m_undo {nullptr};
    Button* m_redo {nullptr};
    Button* m_save {nullptr};
    Button* m_preview_button {nullptr};
    Button* m_accept {nullptr};
    Button* m_discard {nullptr};
    Button* m_cancel {nullptr};
    Button* m_auto_partition {nullptr};
    Button* m_pick_partition {nullptr};
    Button* m_apply_partition {nullptr};
    WorkbenchSwitch* m_lighting {nullptr};
    WorkbenchSwitch* m_original_view {nullptr};
    wxPanel* m_lighting_row {nullptr};
    wxPanel* m_original_view_row {nullptr};
    wxSlider* m_displacement {nullptr};
    wxSlider* m_falloff {nullptr};
    wxStaticText* m_displacement_label {nullptr};
    wxStaticText* m_falloff_label {nullptr};
    boost::filesystem::path m_source;
    boost::filesystem::path m_partition_restore_source;
    boost::filesystem::path m_pending_secondary_partition_source;
    std::string m_geometry_id;
    std::shared_ptr<const AI::BeautySurface> m_surface;
    std::optional<AI::BeautyPuzzle> m_partition;
    uint32_t m_selected_piece {0};
    std::shared_ptr<PartitionTask> m_partition_task;
    std::thread m_partition_worker;
    wxTimer m_partition_timer;
    AI::BeautyDocument m_document;
    bool m_editable {false};
    bool m_processing {false};
    bool m_candidate_ready {false};
    bool m_visible {false};
    bool m_ready {false};
    bool m_dirty {false};
    bool m_can_undo {false};
    bool m_can_redo {false};
    bool m_details_open {false};
};
}
