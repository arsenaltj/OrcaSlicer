#include "OrcaPrinterAdapter.hpp"

#include "../GUI_App.hpp"
#include "../DeviceManager.hpp"
#include "../DeviceCore/DevManager.h"
#include "../DeviceCore/DevBed.h"
#include "../DeviceCore/DevExtruderSystem.h"
#include "../DeviceCore/DevFilaSystem.h"
#include "../DeviceCore/DevHMS.h"
#include "../DeviceCore/DevStorage.h"
#include "../Plater.hpp"
#include "../GLCanvas3D.hpp"
#include "../ImGuiWrapper.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/filesystem/path.hpp>
#include <algorithm>

namespace Slic3r::GUI {
namespace {

MachineObject* selected_machine()
{
    auto* manager = wxGetApp().getDeviceManager();
    return manager ? manager->get_selected_machine() : nullptr;
}

MachineObject* selected_target(const std::string& id)
{
    auto* machine = selected_machine();
    return machine && machine->get_dev_id() == id && machine->is_connected() && machine->is_info_ready()
        ? machine : nullptr;
}

GLCanvas3D* prepare_plate_renderer(Plater* plater)
{
    auto* canvas = plater ? plater->get_view3D_canvas3D() : nullptr;
    if (!canvas || !canvas->make_current_for_postinit() || !wxGetApp().init_opengl()) return nullptr;
    // The hidden native canvas still owns thumbnails and slicing notifications.
    // Initialize their fonts offscreen before a progress event measures text.
    auto* imgui = wxGetApp().imgui();
    if (!imgui || !ImGui::GetCurrentContext()) return nullptr;
    if (!ImGui::GetFont()) {
        const auto size = plater->GetClientSize();
        imgui->set_display_size(float(std::max(1, size.x)), float(std::max(1, size.y)));
        imgui->new_frame();
        imgui->render();
    }
    return canvas->init() ? canvas : nullptr;
}

} // namespace

PrinterWorkspaceSnapshot OrcaPrinterAdapter::snapshot() const
{
    PrinterWorkspaceSnapshot state;
    if (auto* bundle = wxGetApp().preset_bundle) {
        state.native_dispatch = bundle->use_bbl_network() ||
            wxGetApp().app_config->get_bool("use_printer_agents");
        state.connection.native_backend = bundle->use_bbl_device_tab() &&
            !wxGetApp().app_config->get_bool("use_printer_agents");
        const auto& config = bundle->printers.get_edited_preset().config;
        const auto* host = config.option<ConfigOptionString>("print_host");
        state.configured_host = host && !host->value.empty();
        state.connection.web_entry_configured = state.configured_host;
        state.device_name = bundle->printers.get_edited_preset().name;
    }
    if (m_plater) {
        state.slicing = m_plater->is_background_process_slicing();
        auto* plate = m_plater->get_partplate_list().get_curr_plate();
        if (plate) {
            state.can_slice = !m_plater->only_gcode_mode() && !m_plater->using_exported_file() &&
                              plate->can_slice() && plate->has_printable_instances();
            state.gcode_ready = plate->is_slice_result_ready_for_export() && !state.slicing &&
                                !m_plater->is_export_gcode_scheduled() && !preview_toolpath_outside();
            if (state.gcode_ready) {
                state.gcode_name = m_plater->get_export_gcode_filename(".gcode", true).ToUTF8().data();
                if (state.gcode_name.empty())
                    state.gcode_name = boost::filesystem::path(plate->get_gcode_filename()).filename().string();
                if (auto* result = plate->get_slice_result())
                    state.estimated_seconds = static_cast<int>(result->print_statistics.modes[
                        static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time);
            }
        }
    }
    auto* machine = selected_machine();
    // Printer-agent and WebView backends keep their native confirmation path;
    // a DeviceManager selection from a different backend is not their telemetry.
    if (!machine || !state.connection.native_backend) return state;
    state.device_id = machine->get_dev_id();
    state.device_name = machine->get_dev_name();
    state.printer_type = machine->printer_type;
    state.connected = machine->is_connected();
    state.connecting = machine->is_connecting();
    state.connection.native_selected = true;
    state.connection.native_online = machine->is_online();
    state.connection.native_connected = state.connected;
    if (machine->last_update_time.time_since_epoch().count() > 0)
        state.connection.telemetry_age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - machine->last_update_time);
    state.info_ready = orca_print_connection_state(state.connection) == OrcaPrintConnectionState::Online &&
                       machine->is_info_ready() && machine->m_push_count > 0;
    OrcaDeviceMediaFacts media;
    media.connection = state.connection;
    media.information_ready = state.info_ready;
    media.storage_readable = machine->GetStorage() &&
        machine->GetStorage()->get_sdcard_state() == DevStorage::HAS_SDCARD_NORMAL;
    media.file_protocol_supported = machine->file_local ||
        (!machine->is_lan_mode_printer() && machine->get_file_remote());
    media.busy = machine->is_camera_busy_off();
    state.media_state = orca_device_media_state(media);
    // Never keep stale telemetry or enabled task actions when a device drops.
    if (!state.info_ready) return state;
    state.has_camera = machine->has_ipcam;
    if (!machine->job_id_.empty() || !machine->task_id_.empty() || !machine->subtask_id_.empty() || !machine->obj_subtask_id.empty())
        state.task_id = machine->job_id_ + "/" + machine->task_id_ + "/" + machine->subtask_id_ + "/" + machine->obj_subtask_id;
    state.task_name = machine->subtask_name;
    state.phase = printer_task_phase(machine->print_status.empty() ? machine->iot_print_status : machine->print_status);
    state.can_print = machine->can_print();
    state.can_pause = machine->can_pause();
    state.can_resume = machine->can_resume();
    state.can_stop = machine->can_abort();
    if (printer_task_active(state.phase) || state.phase == PrinterTaskPhase::Complete) {
        if (machine->mc_print_percent >= 0 && machine->mc_print_percent <= 100)
            state.progress = machine->mc_print_percent;
        if (machine->mc_left_time >= 0) state.remaining_seconds = machine->mc_left_time;
    }
    if (auto* extruders = machine->GetExtderSystem()) {
        if (auto extruder = extruders->GetCurrentExtder())
            state.nozzle_temperature = extruder->GetCurrentTemp();
    }
    if (auto* bed = machine->GetBed()) state.bed_temperature = static_cast<int>(bed->GetBedTemp());
    state.can_level = machine->is_support_bed_leveling != 0;
    state.bed_leveling = wxGetApp().app_config->get(machine->printer_type, "bed_leveling") != "off";
    state.recording = machine->camera_recording_when_printing;
    state.can_record = state.has_camera && machine->GetStorage() &&
        machine->GetStorage()->get_sdcard_state() == DevStorage::HAS_SDCARD_NORMAL;
    state.can_detect = !machine->xcam_disable_ai_detection_display &&
        (machine->is_support_spaghetti_detection || machine->xcam_ai_monitoring);
    state.detection_enabled = machine->xcam_ai_monitoring;
    state.device_warning = machine->print_error != 0;
    if (auto* hms = machine->GetHMS()) {
        for (const auto& alarm : hms->GetHMSItems()) {
            if (alarm.get_level() >= HMS_FATAL && alarm.get_level() <= HMS_COMMON)
                state.device_warning = true;
        }
    }
    if (machine->is_support_update_remain) {
        auto* extruders = machine->GetExtderSystem();
        auto filaments = machine->GetFilaSystem();
        if (extruders && filaments) {
            auto* tray = filaments->GetAmsTray(extruders->GetCurrentAmsId(), extruders->GetCurrentSlotId());
            if (tray && tray->is_exists && tray->remain >= 0 && tray->remain <= 100)
                state.filament_percent = tray->remain;
        }
    }
    return state;
}

bool OrcaPrinterAdapter::preview_toolpath_outside() const
{
    auto* plate = m_plater ? m_plater->get_partplate_list().get_curr_plate() : nullptr;
    const auto* result = plate ? plate->get_slice_result() : nullptr;
    auto* canvas = m_plater ? m_plater->get_preview_canvas3D() : nullptr;
    if (!result || !canvas || !plate->is_slice_result_valid()) return false;
    const auto& preview = canvas->get_gcode_viewer();
    // Preview checks include the wipe tower. Ignore a previous plate/result.
    return preview.has_data() && preview.loaded_result_id() == result->id && !preview.is_contained_in_bed();
}

std::vector<PrinterWorkspaceDevice> OrcaPrinterAdapter::devices() const
{
    std::vector<PrinterWorkspaceDevice> result;
    if (auto* manager = wxGetApp().getDeviceManager()) {
        for (const auto& entry : manager->get_my_machine_list(manager->get_current_printer_agent_id())) {
            if (entry.second)
                result.push_back({entry.first, entry.second->get_dev_name(), entry.second->is_online()});
        }
    }
    return result;
}

bool OrcaPrinterAdapter::select_device(const std::string& id) const
{
    auto* manager = wxGetApp().getDeviceManager();
    if (!manager || id.empty()) return false;
    auto* selected = manager->get_selected_machine();
    if (selected && selected->get_dev_id() == id) return true;
    const auto list = devices();
    if (std::none_of(list.begin(), list.end(), [&](const auto& device) { return device.id == id; })) return false;
    return manager->set_selected_machine(id);
}

bool OrcaPrinterAdapter::review_print(wxWindow* parent) const
{
    if (!m_plater || !printer_can_review(snapshot())) return false;
    // Preserve PR20's full workspace revision and native slice/config checks,
    // export, send dialog and cancellation. Opening the review sends nothing.
    show_orca_print_confirmation(parent, *m_plater);
    return true;
}

bool OrcaPrinterAdapter::start_slice() const
{
    if (!m_plater || !printer_can_slice(snapshot())) return false;
    if (!prepare_plate_renderer(m_plater)) return false;
    m_plater->reslice();
    // reslice() may return immediately when model/preset validation refuses it.
    return m_plater->is_background_process_slicing() || snapshot().gcode_ready;
}

bool OrcaPrinterAdapter::can_import_model() const
{
    return m_plater && m_plater->can_add_model();
}

bool OrcaPrinterAdapter::import_model() const
{
    if (!can_import_model()) return false;
    m_plater->add_file();
    return true;
}

bool OrcaPrinterAdapter::control_task(const PrinterCommandTarget& target, PrinterTaskAction action) const
{
    if (!printer_command_allowed(snapshot(), target, action)) return false;
    auto* machine = selected_target(target.device_id);
    if (!machine) return false;
    switch (action) {
    case PrinterTaskAction::Pause: return machine->command_task_pause() == 0;
    case PrinterTaskAction::Resume:
        if (machine->check_resume_condition() != 0) return false;
        return machine->command_task_resume() == 0;
    case PrinterTaskAction::Stop: return machine->command_task_abort() == 0;
    }
    return false;
}

bool OrcaPrinterAdapter::set_bed_leveling(const std::string& id, bool enabled) const
{
    auto* machine = selected_target(id);
    const auto state = snapshot();
    if (!machine || !state.can_level || printer_task_active(state.phase)) return false;
    wxGetApp().app_config->set(machine->printer_type, "bed_leveling", enabled ? "on" : "off");
    return true;
}

bool OrcaPrinterAdapter::set_recording(const std::string& id, bool enabled) const
{
    auto* machine = selected_target(id);
    return machine && snapshot().can_record && machine->command_ipcam_record(enabled) == 0;
}

bool OrcaPrinterAdapter::set_detection(const std::string& id, bool enabled) const
{
    auto* machine = selected_target(id);
    return machine && snapshot().can_detect && machine->command_xcam_control_ai_monitoring(
        enabled, machine->xcam_ai_monitoring_sensitivity.empty() ? "medium" : machine->xcam_ai_monitoring_sensitivity) == 0;
}

wxImage OrcaPrinterAdapter::plate_preview() const
{
    if (!m_plater || m_plater->model().objects.empty()) return {};
    auto* plate = m_plater->get_partplate_list().get_curr_plate();
    if (!plate) return {};
    if (!plate->no_light_thumbnail_data.is_valid() && !plate->thumbnail_data.is_valid()) {
        auto* canvas = prepare_plate_renderer(m_plater);
        if (!canvas) return {};
        canvas->reload_scene(true, true);
        m_plater->update_all_plate_thumbnails(false);
    }
    const auto& thumbnail = plate->thumbnail_data.is_valid()
        ? plate->thumbnail_data : plate->no_light_thumbnail_data;
    if (!thumbnail.is_valid()) return {};
    wxImage image(thumbnail.width, thumbnail.height);
    image.InitAlpha();
    for (unsigned y = 0; y < thumbnail.height; ++y) {
        for (unsigned x = 0; x < thumbnail.width; ++x) {
            const auto offset = 4 * ((thumbnail.height - y - 1) * thumbnail.width + x);
            image.SetRGB(x, y, thumbnail.pixels[offset], thumbnail.pixels[offset + 1], thumbnail.pixels[offset + 2]);
            image.SetAlpha(x, y, thumbnail.pixels[offset + 3]);
        }
    }
    return image;
}

} // namespace Slic3r::GUI
