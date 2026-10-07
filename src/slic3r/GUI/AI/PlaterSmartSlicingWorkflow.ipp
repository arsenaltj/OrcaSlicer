// Included once by Plater.cpp after Plater::priv is complete.
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
            // Unchanged candidates reuse Orca's finished result. Queue completion so the
            // gateway has entered its pending state before it receives the native event.
            if (!p->m_is_slicing && p->background_process.idle() && p->background_process.finished() &&
                plate == p->partplate_list.get_curr_plate() && plate->is_slice_result_valid()) {
                BOOST_LOG_TRIVIAL(info) << "AI official slice: reuse completed native result";
                SlicingProcessCompletedEvent event(EVT_PROCESS_COMPLETED, 0,
                    SlicingProcessCompletedEvent::Finished, nullptr);
                wxQueueEvent(this, event.Clone());
                return true;
            }
            return p->m_is_slicing;
        });
}
