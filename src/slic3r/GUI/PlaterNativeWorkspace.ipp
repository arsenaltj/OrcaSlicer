// Native workspace setup, appearance and slice cancellation.
void Plater::priv::schedule_auto_reslice_if_needed()
{
    if (smart_slicing_host && smart_slicing_host->workbench_active()) return;
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr || !cfg->get_bool("auto_slice_after_change"))
        return;

    if (!is_preview_shown())
        return;

    if (model.objects.empty())
        return;

    PartPlate* plate = partplate_list.get_curr_plate();
    if (plate == nullptr || !plate->has_printable_instances())
        return;

    if (background_process.running() || m_is_slicing) {
        // Remember to restart once the current slice stops and cancel it now.
        auto_reslice_after_cancel = true;
        background_process.stop();
        return;
    }

    const int delay_seconds = auto_slice_delay_seconds();
    if (delay_seconds > 0) {
        auto_reslice_pending = true;
        auto_reslice_timer.Stop();
        auto_reslice_timer.Start(delay_seconds * 1000, wxTIMER_ONE_SHOT);
        return;
    }

    if (auto_reslice_pending)
        return;

    auto_reslice_pending = true;
    auto_reslice_timer.Stop();
    wxGetApp().CallAfter([this]() { this->trigger_auto_reslice_now(); });
}

void Plater::priv::trigger_auto_reslice_now()
{
    this->auto_reslice_pending = false;
    if (smart_slicing_host && smart_slicing_host->workbench_active()) return;

    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr || !cfg->get_bool("auto_slice_after_change"))
        return;

    if (!is_preview_shown())
        return;

    if (this->model.objects.empty())
        return;

    if (this->background_process.running() || this->m_is_slicing)
        return;

    PartPlate* plate = this->partplate_list.get_curr_plate();
    if (plate == nullptr || !plate->has_printable_instances())
        return;

    this->q->reslice();
}

bool Plater::priv::finish_workbench_slice_attempt(SlicingProcessCompletedEvent& evt, bool is_finished)
{
    background_process.stop();
    background_process.set_workbench_placement_check(false);
    if (!is_finished || !evt.error() || !smart_slicing_host || !smart_slicing_host->can_retry_custom_tower())
        return false;

    try {
        evt.rethrow_exception();
    } catch (const WipeTowerPlacementError&) {
        try {
            if (const auto position = background_process.fff_print()->find_wipe_tower_position()) {
                m_is_slicing = false;
                return smart_slicing_host->retry_custom_tower(*position);
            }
        } catch (const std::exception& error) {
            BOOST_LOG_TRIVIAL(warning) << "Could not recover the automatic wipe tower: " << error.what();
        }
    } catch (...) {
        // The existing completion path reports other failures.
    }
    return false;
}

void Plater::priv::publish_workbench_slice_completion(SlicingProcessCompletedEvent& evt, bool publish, bool has_error)
{
    if (publish && smart_slicing_host) {
        const bool success = evt.success() && !has_error && !evt.cancelled();
        smart_slicing_host->notify_slice_completed(
            success, evt.cancelled() ? "official_slice_canceled" : "official_slice_failed");
    }
}

void Plater::priv::fix_loaded_wipe_tower_positions()
{
    for (PartPlate* plate : partplate_list.get_plate_list())
        if (!plate->has_saved_tower_source())
            plate->set_automatic_wipe_tower_position(false);
}

static bool resolve_automatic_wipe_tower(PartPlateList& plates, PartPlate& plate,
    const Print& print, StringObjectException& error)
{
    if (!plate.automatic_wipe_tower_position() || plate.get_shape().size() < 3) return false;
    // Resolve the default against the model and effective filament mapping
    // already applied to Print, before native validation or scene refresh.
    const int index = plate.get_index();
    const Vec2d previous(print.config().wipe_tower_x.get_at(index), print.config().wipe_tower_y.get_at(index));
    const auto status = print.check_wipe_tower_position(previous);
    if (status == WipeTowerPositionStatus::NotRequired || status == WipeTowerPositionStatus::Valid ||
        status == WipeTowerPositionStatus::Unavailable) return false;
    if (const auto position = print.find_wipe_tower_position()) {
        plates.set_wipe_tower_position_for_plate(index, *position, true);
        BOOST_LOG_TRIVIAL(info) << "[WipeTowerPlacement] automatic plate=" << index
            << " old=" << previous.transpose() << " new=" << position->transpose();
        return true;
    }
    error = {_u8L("当前打印盘没有可用的擦料塔位置。"), nullptr, "wipe_tower_x"};
    return false;
}

Print::ApplyStatus Plater::priv::apply_native_plate_settings(bool was_running, bool force_validation,
    unsigned int& state, StringObjectException& error)
{
    auto& plate = *background_process.get_current_plate();
    auto& bundle = *wxGetApp().preset_bundle;
    if (bundle.get_printer_extruder_count() > 1)
        background_process.fff_print()->set_extruder_filament_info(get_extruder_filament_info());
    auto invalidated = background_process.apply(model, plate.slice_preset_config(bundle));
    if (printer_technology == ptFFF && !was_running && (invalidated != Print::APPLY_STATUS_UNCHANGED || force_validation) &&
        !(smart_slicing_host && smart_slicing_host->workbench_tower_pinned()) &&
        resolve_automatic_wipe_tower(partplate_list, plate, *background_process.fff_print(), error)) {
        if (smart_slicing_host) smart_slicing_host->rebind_custom_tower_position();
        invalidated = std::max(invalidated, background_process.apply(model, plate.slice_preset_config(bundle)));
        state |= UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE;
    }
    return invalidated;
}

void Plater::priv::apply_color_mode()
{
    const bool is_dark         = wxGetApp().dark_mode();
    const bool redesign = main_frame && main_frame->is_redesign_shell_active();
    wxColour   orca_color      = wxColour(59, 68, 70);//wxColour(ColorRGBA::ORCA().r_uchar(), ColorRGBA::ORCA().g_uchar(), ColorRGBA::ORCA().b_uchar());
    orca_color                 = is_dark ? StateColor::darkModeColorFor(orca_color) : StateColor::lightModeColorFor(orca_color);
    wxColour sash_color = redesign ? wxColour(35, 35, 38)
        : is_dark ? wxColour(38, 46, 48) : wxColour(206, 206, 206);
    m_aui_mgr.GetArtProvider()->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_COLOUR, sash_color);
    m_aui_mgr.GetArtProvider()->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_TEXT_COLOUR, *wxWHITE);
    m_aui_mgr.GetArtProvider()->SetColour(wxAUI_DOCKART_SASH_COLOUR, sash_color);
    m_aui_mgr.GetArtProvider()->SetColour(wxAUI_DOCKART_BORDER_COLOUR,
        redesign ? wxColour(49, 49, 54) : is_dark ? *wxBLACK : wxColour(165, 165, 165));
}

bool Plater::cancel_current_slice()
{
    if (p->background_process.idle()) return false;
    p->background_process.stop();
    return true;
}
