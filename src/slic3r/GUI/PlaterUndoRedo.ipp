// Included once by Plater.cpp inside Slic3r::GUI after Plater::priv.
// Keep native history jumps and project-configuration commits together.

bool Plater::priv::can_begin_project_config_change() const
{
    return m_prevent_snapshots == 0 && m_single == nullptr && m_undo_redo_stack_active == &m_undo_redo_stack_main;
}

bool Plater::apply_project_config(DynamicPrintConfig config, std::vector<std::string> presets,
    const std::string& action, std::string& error)
{
    auto& bundle = *wxGetApp().preset_bundle;
    error.clear();
    if (!p->can_begin_project_config_change()) {
        error = "Finish the current editing operation before changing materials.";
        return false;
    }
    if (!ProjectConfigRestore::validate(config, presets, bundle, error)) return false;
    auto change = UndoRedo::ProjectConfigUndo::Change::capture(bundle.project_config, bundle.filament_presets, config, presets);
    if (!change) return true;
    UndoRedo::ProjectConfigUndo::Prepared prepared {std::move(config), std::move(presets), true};
    auto cache = ProjectConfigRestore::prepare_cache(prepared.filament_presets.size(), bundle);
    const size_t before = p->undo_redo_stack().active_snapshot_time();
    TakeSnapshot snapshot(this, action);
    if (p->undo_redo_stack().active_snapshot_time() == before || !p->undo_redo_stack().record_project_config_change(std::move(change))) {
        error = "Unable to record material settings in the project history.";
        return false;
    }
    ProjectConfigRestore::commit(prepared, cache, bundle);
    return true;
}


bool Plater::apply_local_print_colors(ModelVolume& volume, LocalPrintColorCommit::Prepared& painting,
    const PresetBundle* staged, std::string& error)
{
    error.clear();
    if (!p->can_begin_project_config_change()) {
        error = "Finish the current editing operation before applying colors.";
        return false;
    }
    bool owned = false;
    for (const auto* object : model().objects)
        for (const auto* item : object->volumes) if (item == &volume) owned = true;
    if (!owned) { error = "The prepared model is no longer in this project."; return false; }
    if (!LocalPrintColorCommit::validate(volume, painting, error)) return false;
    auto& bundle = *wxGetApp().preset_bundle;
    UndoRedo::ProjectConfigUndo::Prepared config;
    std::shared_ptr<const UndoRedo::ProjectConfigUndo::Change> change;
    ProjectConfigRestore::ColorCache cache;
    if (staged) {
        if (!ProjectConfigRestore::validate(staged->project_config, staged->filament_presets, bundle, error)) return false;
        change = UndoRedo::ProjectConfigUndo::Change::capture(bundle.project_config, bundle.filament_presets,
            staged->project_config, staged->filament_presets);
        if (change) {
            config = {staged->project_config, staged->filament_presets, true};
            cache = ProjectConfigRestore::prepare_cache(config.filament_presets.size(), bundle);
        }
    }
    const size_t before = p->undo_redo_stack().active_snapshot_time();
    TakeSnapshot snapshot(this, "Apply local print colors");
    if (p->undo_redo_stack().active_snapshot_time() == before ||
        (change && !p->undo_redo_stack().record_project_config_change(std::move(change)))) {
        error = "Unable to record the color operation in project history.";
        return false;
    }
    // No callbacks or allocations between these commits. Observers see both
    // the new slot table and painting together; normal rejection is above.
    LocalPrintColorCommit::commit(volume, painting, config, cache, bundle);
    return true;
}

void Plater::priv::undo_redo_to(std::vector<UndoRedo::Snapshot>::const_iterator it_snapshot)
{
    UndoRedo::ProjectConfigUndo::Prepared restored_config;
    std::string config_error;
    auto& bundle = *wxGetApp().preset_bundle;
    if (!undo_redo_stack().prepare_project_config_jump(it_snapshot->timestamp, bundle.project_config,
            bundle.filament_presets, restored_config, config_error) ||
        (restored_config.changed && !ProjectConfigRestore::validate(restored_config.config, restored_config.filament_presets, bundle, config_error))) {
        GUI::show_error(q, wxString::FromUTF8(config_error.c_str()));
        return;
    }
    if (restored_config.changed && printer_technology != it_snapshot->snapshot_data.printer_technology) {
        GUI::show_error(q, _L("Cannot restore material settings across printer technologies."));
        return;
    }
    auto restored_cache = restored_config.changed ? ProjectConfigRestore::prepare_cache(restored_config.filament_presets.size(), bundle) : ProjectConfigRestore::ColorCache{};
    // Make sure that no updating function calls take_snapshot until we are done.
    SuppressSnapshots snapshot_supressor(q);

    bool 				temp_snapshot_was_taken 	= this->undo_redo_stack().temp_snapshot_active();
    PrinterTechnology 	new_printer_technology 		= it_snapshot->snapshot_data.printer_technology;
    bool 				printer_technology_changed 	= this->printer_technology != new_printer_technology;
    if (printer_technology_changed) {
        //BBS do not support SLA
    }
    // Save the last active preset name of a particular printer technology.
    ((this->printer_technology == ptFFF) ? m_last_fff_printer_profile_name : m_last_sla_printer_profile_name) = wxGetApp().preset_bundle->printers.get_selected_preset_name();
    //FIXME updating the Wipe tower config values at the ModelWipeTower from the Print config.
    // This is a workaround until we refactor the Wipe Tower position / orientation to live solely inside the Model, not in the Print config.
    // BBS: add partplate logic
    if (this->printer_technology == ptFFF) {
        const DynamicPrintConfig& config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        const DynamicPrintConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
        const ConfigOptionFloats* tower_x_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_x");
        const ConfigOptionFloats* tower_y_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_y");
        assert(tower_x_opt->values.size() == tower_y_opt->values.size());
        model.wipe_tower.positions.clear();
        model.wipe_tower.positions.resize(tower_x_opt->values.size());
        for (int plate_idx = 0; plate_idx < tower_x_opt->values.size(); plate_idx++) {
            ModelWipeTower& tower = model.wipe_tower;

            tower.positions[plate_idx] = Vec2d(tower_x_opt->get_at(plate_idx), tower_y_opt->get_at(plate_idx));
            tower.rotation = config.opt_float("wipe_tower_rotation_angle");
        }
    }
    const int layer_range_idx = it_snapshot->snapshot_data.layer_range_idx;
    // Flags made of Snapshot::Flags enum values.
    unsigned int new_flags = it_snapshot->snapshot_data.flags;
    UndoRedo::SnapshotData top_snapshot_data;
    top_snapshot_data.printer_technology = this->printer_technology;
    if (this->view3D->is_layers_editing_enabled())
        top_snapshot_data.flags |= UndoRedo::SnapshotData::VARIABLE_LAYER_EDITING_ACTIVE;
    if (this->sidebar->obj_list()->is_selected(itSettings)) {
        top_snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_SETTINGS_ON_SIDEBAR;
        top_snapshot_data.layer_range_idx = this->sidebar->obj_list()->get_selected_layers_range_idx();
    }
    else if (this->sidebar->obj_list()->is_selected(itLayer)) {
        top_snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_LAYER_ON_SIDEBAR;
        top_snapshot_data.layer_range_idx = this->sidebar->obj_list()->get_selected_layers_range_idx();
    }
    else if (this->sidebar->obj_list()->is_selected(itLayerRoot))
        top_snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_LAYERROOT_ON_SIDEBAR;
    bool   		 new_variable_layer_editing_active = (new_flags & UndoRedo::SnapshotData::VARIABLE_LAYER_EDITING_ACTIVE) != 0;
    bool         new_selected_settings_on_sidebar  = (new_flags & UndoRedo::SnapshotData::SELECTED_SETTINGS_ON_SIDEBAR) != 0;
    bool         new_selected_layer_on_sidebar     = (new_flags & UndoRedo::SnapshotData::SELECTED_LAYER_ON_SIDEBAR) != 0;
    bool         new_selected_layerroot_on_sidebar = (new_flags & UndoRedo::SnapshotData::SELECTED_LAYERROOT_ON_SIDEBAR) != 0;

    if (this->view3D->get_canvas3d()->get_gizmos_manager().wants_reslice_supports_on_undo())
        top_snapshot_data.flags |= UndoRedo::SnapshotData::RECALCULATE_SLA_SUPPORTS;

    // Disable layer editing before the Undo / Redo jump.
    if (!new_variable_layer_editing_active && view3D->is_layers_editing_enabled())
        view3D->get_canvas3d()->force_main_toolbar_left_action(view3D->get_canvas3d()->get_main_toolbar_item_id("layersediting"));

    // Make a copy of the snapshot, undo/redo could invalidate the iterator
    const UndoRedo::Snapshot snapshot_copy = *it_snapshot;
    // Do the jump in time.
    if (it_snapshot->timestamp < this->undo_redo_stack().active_snapshot_time() ?
        this->undo_redo_stack().undo(model, get_current_canvas3D()->get_canvas_type() == GLCanvas3D::CanvasAssembleView ? assemble_view->get_canvas3d()->get_selection() : this->view3D->get_canvas3d()->get_selection(), get_current_canvas3D()->get_canvas_type() == GLCanvas3D::CanvasAssembleView ? assemble_view->get_canvas3d()->get_gizmos_manager() : this->view3D->get_canvas3d()->get_gizmos_manager(), this->partplate_list, top_snapshot_data, it_snapshot->timestamp) :
        this->undo_redo_stack().redo(model, get_current_canvas3D()->get_canvas_type() == GLCanvas3D::CanvasAssembleView ? assemble_view->get_canvas3d()->get_gizmos_manager() : this->view3D->get_canvas3d()->get_gizmos_manager(), this->partplate_list, it_snapshot->timestamp)) {
        const bool materials_restored = restored_config.changed;
        ProjectConfigRestore::commit(restored_config, restored_cache, bundle);
        if (materials_restored) {
            partplate_list.set_filament_count(int(bundle.filament_presets.size()));
            q->on_config_change(bundle.full_config());
            sidebar->on_filament_count_change(bundle.filament_presets.size());
            sidebar->obj_list()->update_objects_list_filament_column(bundle.filament_presets.size());
            partplate_list.invalid_all_slice_result();
            bundle.export_selections(*wxGetApp().app_config);
        }
        if (printer_technology_changed) {
            // Switch to the other printer technology. Switch to the last printer active for that particular technology.
            AppConfig *app_config = wxGetApp().app_config;
            app_config->set("presets", PRESET_PRINTER_NAME, (new_printer_technology == ptFFF) ? m_last_fff_printer_profile_name : m_last_sla_printer_profile_name);
            //FIXME Why are we reloading the whole preset bundle here? Please document. This is fishy and it is unnecessarily expensive.
            // Anyways, don't report any config value substitutions, they have been already reported to the user at application start up.
            wxGetApp().preset_bundle->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
            // load_current_presets() calls Tab::load_current_preset() -> TabPrint::update() -> Object_list::update_and_show_object_settings_item(),
            // but the Object list still keeps pointer to the old Model. Avoid a crash by removing selection first.
            this->sidebar->obj_list()->unselect_objects();
            // Load the currently selected preset into the GUI, update the preset selection box.
            // This also switches the printer technology based on the printer technology of the active printer profile.
            wxGetApp().load_current_presets();
        }
        //FIXME updating the Print config from the Wipe tower config values at the ModelWipeTower.
        // This is a workaround until we refactor the Wipe Tower position / orientation to live solely inside the Model, not in the Print config.
        // BBS: add partplate logic
        if (this->printer_technology == ptFFF) {
            const DynamicPrintConfig& config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
            const DynamicPrintConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
            ConfigOptionFloats* tower_x_opt = const_cast<ConfigOptionFloats*>(proj_cfg.option<ConfigOptionFloats>("wipe_tower_x"));
            ConfigOptionFloats* tower_y_opt = const_cast<ConfigOptionFloats*>(proj_cfg.option<ConfigOptionFloats>("wipe_tower_y"));
            // BBS: don't support wipe tower rotation
            //double current_rotation = proj_cfg.opt_float("wipe_tower_rotation_angle");
            bool need_update = false;
            if (tower_x_opt->values.size() != model.wipe_tower.positions.size()) {
                tower_x_opt->clear();
                ConfigOptionFloat default_tower_x(40.f);
                tower_x_opt->resize(model.wipe_tower.positions.size(), &default_tower_x);
                need_update = true;
            }

            if (tower_y_opt->values.size() != model.wipe_tower.positions.size()) {
                tower_y_opt->clear();
                ConfigOptionFloat default_tower_y(200.f);
                tower_y_opt->resize(model.wipe_tower.positions.size(), &default_tower_y);
                need_update = true;
            }

            for (int plate_idx = 0; plate_idx < model.wipe_tower.positions.size(); plate_idx++) {
                if (Vec2d(tower_x_opt->get_at(plate_idx), tower_y_opt->get_at(plate_idx)) != model.wipe_tower.positions[plate_idx]) {
                    ConfigOptionFloat tower_x_new(model.wipe_tower.positions[plate_idx].x());
                    ConfigOptionFloat tower_y_new(model.wipe_tower.positions[plate_idx].y());
                    tower_x_opt->set_at(&tower_x_new, plate_idx, 0);
                    tower_y_opt->set_at(&tower_y_new, plate_idx, 0);
                    need_update = true;
                    break;
                }
            }

            if (need_update) {
                // update print to current plate (preview->m_process)
                this->partplate_list.update_slice_context_to_current_plate(this->background_process);
                this->preview->update_gcode_result(this->partplate_list.get_current_slice_result());
                this->update();
            }
        }
        // set selection mode for ObjectList on sidebar
        this->sidebar->obj_list()->set_selection_mode(new_selected_settings_on_sidebar  ? ObjectList::SELECTION_MODE::smSettings :
                                                      new_selected_layer_on_sidebar     ? ObjectList::SELECTION_MODE::smLayer :
                                                      new_selected_layerroot_on_sidebar ? ObjectList::SELECTION_MODE::smLayerRoot :
                                                                                          ObjectList::SELECTION_MODE::smUndef);
        if (new_selected_settings_on_sidebar || new_selected_layer_on_sidebar)
            this->sidebar->obj_list()->set_selected_layers_range_idx(layer_range_idx);

        this->update_after_undo_redo(snapshot_copy, temp_snapshot_was_taken);
        // Enable layer editing after the Undo / Redo jump.
        if (!view3D->is_layers_editing_enabled() && this->layers_height_allowed() && new_variable_layer_editing_active)
            view3D->get_canvas3d()->force_main_toolbar_left_action(view3D->get_canvas3d()->get_main_toolbar_item_id("layersediting"));
    }

    dirty_state.update_from_undo_redo_stack(m_undo_redo_stack_main.project_modified());
    update_title_dirty_status();
}


bool Plater::adopt_local_print_model(const ModelObject& object, const PresetBundle* staged,
    size_t& index, std::string& error)
{
    error.clear();
    if (!p->can_begin_project_config_change()) {
        error = "Finish the current editing operation before importing colors."; return false;
    }
    auto* plate = get_partplate_list().get_curr_plate();
    if (!plate || !wxGetApp().preset_bundle) { error = "The current plate or materials are unavailable."; return false; }
    const auto& volume = build_volume();
    const Vec2d center = volume.bounding_volume2d().center();
    Vec2d placement = center;
    if (!plate->empty()) {
        const auto empty = canvas3D()->get_nearest_empty_cell({center.x(), center.y()});
        placement = {empty.x(), empty.y()};
    }
    std::unique_ptr<Model> prepared;
    if (!LocalPrintModelImport::prepare(object, placement, volume.bounding_volume2d().size() - 2. * Vec2d::Ones(), prepared, error))
        return false;
    auto& bundle = *wxGetApp().preset_bundle;
    UndoRedo::ProjectConfigUndo::Prepared config;
    std::shared_ptr<const UndoRedo::ProjectConfigUndo::Change> change;
    ProjectConfigRestore::ColorCache cache;
    if (staged) {
        if (!ProjectConfigRestore::validate(staged->project_config, staged->filament_presets, bundle, error)) return false;
        change = UndoRedo::ProjectConfigUndo::Change::capture(bundle.project_config, bundle.filament_presets,
            staged->project_config, staged->filament_presets);
        if (change) {
            config = {staged->project_config, staged->filament_presets, true};
            cache = ProjectConfigRestore::prepare_cache(config.filament_presets.size(), bundle);
        }
    }
    model().objects.reserve(model().objects.size() + 1);
    const size_t before = p->undo_redo_stack().active_snapshot_time();
    TakeSnapshot snapshot(this, "Apply local print colors");
    if (p->undo_redo_stack().active_snapshot_time() == before) {
        error = "Unable to create the color import history entry."; return false;
    }
    return LocalPrintModelImport::adopt(model(), *prepared->objects.front(), config, cache, bundle,
        [&] { return !change || p->undo_redo_stack().record_project_config_change(std::move(change)); }, index, error);
}

void Plater::finish_local_print_model_import(size_t index)
{
    get_notification_manager()->close_notification_of_type(NotificationType::UpdatedItemsInfo);
    wxGetApp().obj_list()->add_objects_to_list({index});
    update();
    wxGetApp().obj_list()->update_info_items(index);
    object_list_changed();
    schedule_background_process();
}
