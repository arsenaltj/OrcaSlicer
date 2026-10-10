bool Plater::is_ai_assistant_shown() const
{
    return p->ai_assistant_panel != nullptr && p->m_aui_mgr.GetPane(p->ai_assistant_panel).IsShown();
}

void Plater::enable_ai_assistant()
{
    if (p->ai_assistant_panel != nullptr)
        return;

    p->ai_assistant_panel = new AIAssistantPanel(this, this);
    p->m_aui_mgr.AddPane(p->ai_assistant_panel, wxAuiPaneInfo()
                                                     .Name("ai_assistant")
                                                     .Caption(_L("AI Assistant"))
                                                     .Right()
                                                     .CloseButton(true)
                                                     .TopDockable(false)
                                                     .BottomDockable(false)
                                                     .BestSize(wxSize(32 * wxGetApp().em_unit(), 70 * wxGetApp().em_unit()))
                                                     .Hide());
    p->m_aui_mgr.Update();
}

void Plater::show_ai_assistant(bool show)
{
    if (p->ai_assistant_panel == nullptr)
        return;
    auto& pane = p->m_aui_mgr.GetPane(p->ai_assistant_panel);
    if (!pane.IsOk())
        return;
    pane.Show(show);
    p->m_aui_mgr.Update();
}

bool Plater::is_smart_slicing_shown() const
{
    return p->smart_slicing_host != nullptr && p->smart_slicing_host->is_shown();
}

void Plater::enable_smart_slicing()
{
    if (p->smart_slicing_host != nullptr)
        return;

    p->smart_slicing_host = std::make_unique<SmartSlicingFeatureHost>(
        *this, p->m_aui_mgr, *p->sidebar,
        [this] {
            if (printer_technology() != ptFFF ||
                p->process_completed_with_error == p->partplate_list.get_curr_plate_index())
                return false;
            PartPlate* plate = p->partplate_list.get_curr_plate();
            if (plate == nullptr || !plate->has_printable_instances())
                return false;
            const DynamicPrintConfig& config = wxGetApp().preset_bundle->full_config();
            Print& print = p->partplate_list.get_current_fff_print();
            Model::setExtruderParams(config, wxGetApp().preset_bundle->filament_presets.size());
            Model::setPrintSpeedTable(config, print.config());
            p->m_slice_all = false;
            plate->update_slice_result_valid_state(false);
            reslice();
            return p->m_is_slicing;
        });
}

void Plater::show_smart_slicing(bool show)
{
    if (p->smart_slicing_host != nullptr)
        p->smart_slicing_host->show(show);
}

SmartSlicingFeatureHost* Plater::smart_slicing_feature_host()
{
    enable_smart_slicing();
    return p->smart_slicing_host.get();
}

bool Plater::latest_main_snapshot_identity(const std::string& action_name,
                                           UndoRedo::ActionSnapshotIdentity& identity) const
{
    const UndoRedo::Stack& stack = p->undo_redo_stack_main();
    const std::vector<UndoRedo::Snapshot>& snapshots = stack.snapshots();
    const size_t active_time = stack.active_snapshot_time();
    const auto active = std::lower_bound(snapshots.begin(), snapshots.end(), UndoRedo::Snapshot(active_time));
    if (active == snapshots.end() || active->timestamp != active_time || active == snapshots.begin())
        return false;
    const auto action = std::prev(active);
    if (action->name != action_name || !UndoRedo::snapshot_modifies_project(*action))
        return false;
    identity = {action->timestamp, active->timestamp, action->name};
    return identity.valid();
}

bool Plater::main_snapshot_identity_is_current(
    const UndoRedo::ActionSnapshotIdentity& identity)
{
    if (!identity.valid() || inside_snapshot_capture() ||
        get_view3D_canvas3D()->get_gizmos_manager().is_running())
        return false;
    const UndoRedo::Stack& stack = p->undo_redo_stack_main();
    const std::vector<UndoRedo::Snapshot>& snapshots = stack.snapshots();
    if (stack.active_snapshot_time() != identity.active_snapshot_time ||
        stack.has_redo_snapshot())
        return false;
    const auto action = std::lower_bound(
        snapshots.begin(), snapshots.end(), UndoRedo::Snapshot(identity.action_snapshot_time));
    const auto active = std::lower_bound(
        snapshots.begin(), snapshots.end(), UndoRedo::Snapshot(identity.active_snapshot_time));
    return action != snapshots.end() && action->timestamp == identity.action_snapshot_time &&
           active != snapshots.end() && active->timestamp == identity.active_snapshot_time &&
           std::next(action) == active && action->name == identity.action_name &&
           UndoRedo::snapshot_modifies_project(*action) && active->is_topmost() &&
           !active->is_topmost_captured();
}

bool Plater::undo_main_snapshot_exact(const UndoRedo::ActionSnapshotIdentity& identity,
                                      std::string& diagnostic)
{
    diagnostic.clear();
    if (!main_snapshot_identity_is_current(identity)) {
        diagnostic = "snapshot_history_identity_changed";
        return false;
    }
    if (!p->undo_redo_to(identity.action_snapshot_time)) {
        diagnostic = "snapshot_exact_undo_failed";
        return false;
    }
    if (p->undo_redo_stack_main().active_snapshot_time() != identity.action_snapshot_time) {
        diagnostic = "snapshot_exact_undo_unverified";
        return false;
    }
    return true;
}

bool Plater::rollback_main_snapshot_exact(const UndoRedo::ActionSnapshotIdentity& identity,
                                          std::string& diagnostic)
{
    diagnostic.clear();
    if (!identity.valid()) {
        diagnostic = "snapshot_identity_invalid";
        return false;
    }
    const UndoRedo::Stack& stack = p->undo_redo_stack_main();
    const std::vector<UndoRedo::Snapshot>& snapshots = stack.snapshots();
    if (stack.active_snapshot_time() != identity.active_snapshot_time) {
        diagnostic = "snapshot_active_identity_changed";
        return false;
    }
    const auto action = std::lower_bound(
        snapshots.begin(), snapshots.end(), UndoRedo::Snapshot(identity.action_snapshot_time));
    const auto active = std::lower_bound(
        snapshots.begin(), snapshots.end(), UndoRedo::Snapshot(identity.active_snapshot_time));
    if (action == snapshots.end() || action->timestamp != identity.action_snapshot_time ||
        active == snapshots.end() || active->timestamp != identity.active_snapshot_time ||
        std::next(action) != active || action->name != identity.action_name ||
        !UndoRedo::snapshot_modifies_project(*action)) {
        diagnostic = "snapshot_history_identity_changed";
        return false;
    }
    if (!p->undo_redo_to(identity.action_snapshot_time)) {
        diagnostic = "snapshot_exact_rollback_failed";
        return false;
    }
    if (p->undo_redo_stack_main().active_snapshot_time() != identity.action_snapshot_time) {
        diagnostic = "snapshot_exact_rollback_unverified";
        return false;
    }
    if (!p->undo_redo_stack_main().abort_top_action(identity)) {
        diagnostic = "snapshot_exact_abort_failed";
        return false;
    }
    const std::vector<UndoRedo::Snapshot>& restored = p->undo_redo_stack_main().snapshots();
    if (restored.empty() || restored.back().timestamp != identity.action_snapshot_time ||
        !restored.back().is_topmost() || restored.back().is_topmost_captured() ||
        p->undo_redo_stack_main().has_redo_snapshot()) {
        diagnostic = "snapshot_exact_abort_unverified";
        return false;
    }
    return true;
}

#include "PlaterProjectConfirmation.ipp"
