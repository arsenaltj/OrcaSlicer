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
#include "../PartPlate.hpp"
#include "../GLCanvas3D.hpp"
#include "../ImGuiWrapper.hpp"
#include "../../Utils/NetworkAgent.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/filesystem/path.hpp>
#include <algorithm>
#include <array>
#include <exception>
#include <iomanip>
#include <sstream>
#include <wx/msgdlg.h>

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

std::string print_device_ref(const std::string& value)
{
    if (value.empty()) return "none";
    std::ostringstream output;
    output << std::hex << std::hash<std::string>{}(value);
    return output.str();
}

const char* print_action_name(OrcaPrintAction action)
{
    switch (action) {
    case OrcaPrintAction::Cancel: return "cancel";
    case OrcaPrintAction::ExportLocal: return "export_local";
    case OrcaPrintAction::UploadOnly: return "upload_only";
    case OrcaPrintAction::UploadAndPrint: return "upload_and_print";
    }
    return "unknown";
}

const char* task_action_name(PrinterTaskAction action)
{
    switch (action) {
    case PrinterTaskAction::Pause: return "pause";
    case PrinterTaskAction::Resume: return "resume";
    case PrinterTaskAction::Stop: return "stop";
    }
    return "unknown";
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
        imgui->render(imgui->end_frame());
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
        state.preset_name = bundle->printers.get_edited_preset().name;
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
    auto* manager = wxGetApp().getDeviceManager();
    state.active_agent_id = manager ? manager->get_current_printer_agent_id() : std::string();
    auto* network = wxGetApp().getAgent();
    const auto printer_agent = network ? network->get_printer_agent() : nullptr;
    const bool active_printer_agent = printer_agent && printer_agent->get_agent_info().id == state.active_agent_id;
    if (active_printer_agent) {
        const auto traits = printer_agent->get_traits();
        state.requires_task_identity_for_control = traits.requires_task_identity_for_control;
        state.supports_upload_only = traits.supports_upload_only;
        state.supports_upload_and_print = traits.supports_upload_and_print;
        state.native_dispatch = state.supports_upload_only || state.supports_upload_and_print;
        state.connection.native_backend = state.connection.native_backend || traits.supports_monitoring_telemetry;
    }
    auto* machine = manager ? manager->get_selected_machine() : nullptr;
    if (machine && !state.active_agent_id.empty()) {
        const auto active_devices = manager->get_my_machine_list(state.active_agent_id);
        const auto active = active_devices.find(machine->get_dev_id());
        if (active == active_devices.end() || active->second != machine)
            machine = nullptr;
    } else {
        machine = nullptr;
    }
    if (!machine)
        return state;

    state.device_id = machine->get_dev_id();
    state.device_name = machine->get_dev_name();
    state.device_address = machine->get_dev_ip();
    state.printer_type = machine->printer_type;
    state.connected = machine->is_connected();
    state.connecting = machine->is_connecting();
    state.device_selected = true;
    state.device_online = machine->is_online();
    state.device_info_ready = machine->is_info_ready() && machine->m_push_count > 0;
    state.phase = printer_task_phase(machine->print_status.empty() ? machine->iot_print_status : machine->print_status);
    state.can_print = machine->can_print();
    state.transfer_target_ready = state.device_selected && state.device_online && state.connected &&
                                  state.device_info_ready && !state.active_agent_id.empty();
    // Only agents that explicitly publish authoritative MachineObject telemetry
    // may feed monitoring, task controls, or freshness-sensitive dispatch.
    if (!state.connection.native_backend) return state;
    state.connection.native_selected = true;
    state.connection.native_online = state.device_online;
    state.connection.native_connected = state.connected;
    if (machine->last_update_time.time_since_epoch().count() > 0)
        state.connection.telemetry_age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - machine->last_update_time);
    state.info_ready = orca_print_connection_state(state.connection) == OrcaPrintConnectionState::Online &&
                       state.device_info_ready;
    OrcaDeviceMediaFacts media;
    media.connection = state.connection;
    media.information_ready = state.info_ready;
    media.storage_readable = machine->GetStorage() &&
        machine->GetStorage()->get_sdcard_state() == DevStorage::HAS_SDCARD_NORMAL;
    // A printer-agent camera URL does not imply support for Bambu's remote media
    // filesystem protocol. Keep file browsing closed until an agent exposes a
    // dedicated media-file contract.
    media.file_protocol_supported = !active_printer_agent &&
        (machine->file_local || (!machine->is_lan_mode_printer() && machine->get_file_remote()));
    media.busy = machine->is_camera_busy_off();
    state.media_state = orca_device_media_state(media);
    // Never keep stale telemetry or enabled task actions when a device drops.
    if (!state.info_ready) return state;
    const bool agent_camera_available = active_printer_agent &&
        printer_agent->get_camera_stream_mode() != CameraStreamMode::none &&
        !printer_agent->get_camera_url().empty();
    state.has_camera = machine->has_ipcam || agent_camera_available;
    if (!machine->job_id_.empty() || !machine->task_id_.empty() || !machine->subtask_id_.empty() || !machine->obj_subtask_id.empty())
        state.task_id = machine->job_id_ + "/" + machine->task_id_ + "/" + machine->subtask_id_ + "/" + machine->obj_subtask_id;
    state.task_name = machine->subtask_name;
    state.can_pause = machine->can_pause();
    state.can_resume = machine->can_resume();
    state.can_stop = machine->can_abort();
    if (printer_task_active(state.phase) || state.phase == PrinterTaskPhase::Complete) {
        if (machine->mc_print_percent >= 0 && machine->mc_print_percent <= 100)
            state.progress = machine->mc_print_percent;
        if (machine->mc_left_time >= 0) state.remaining_seconds = machine->mc_left_time;
    }
    if (auto* extruders = machine->GetExtderSystem()) {
        if (auto extruder = extruders->GetCurrentExtder()) {
            state.nozzle_temperature = extruder->GetCurrentTemp();
            state.nozzle_target_temperature = extruder->GetTargetTemp();
        }
    }
    if (auto* bed = machine->GetBed()) {
        state.bed_temperature = static_cast<int>(bed->GetBedTemp());
        state.bed_target_temperature = static_cast<int>(bed->GetBedTempTarget());
    }
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

std::string OrcaPrinterAdapter::preview_toolpath_diagnostic() const
{
    auto* plate = m_plater ? m_plater->get_partplate_list().get_curr_plate() : nullptr;
    const auto* result = plate ? plate->get_slice_result() : nullptr;
    auto* canvas = m_plater ? m_plater->get_preview_canvas3D() : nullptr;
    if (!result || !canvas || !plate->is_slice_result_valid()) return {};

    const auto& preview = canvas->get_gcode_viewer();
    if (!preview.has_data() || preview.loaded_result_id() != result->id || preview.is_contained_in_bed()) return {};

    const auto* parsed = preview.get_gcode_result();
    if (!parsed || !m_plater) return _u8L("路径超出热床边界，请检查模型摆放或擦料塔位置。");

    // Reuse BuildVolume::all_paths_inside(), the same native predicate used by
    // GCodeViewer, but run it per extrusion role group so the UI can identify
    // the actual source.  Travel/custom moves are deliberately excluded by
    // BuildVolume's native move_valid predicate.
    enum class PathGroup { Model, Support, SkirtOrBrim, WipeTower };
    std::array<bool, 4> group_outside{};
    auto group_for_role = [](const ExtrusionRole role) {
        if (role == erSupportMaterial || role == erSupportMaterialInterface || role == erSupportTransition)
            return PathGroup::Support;
        if (role == erSkirt || role == erBrim)
            return PathGroup::SkirtOrBrim;
        if (role == erWipeTower)
            return PathGroup::WipeTower;
        return PathGroup::Model;
    };
    const auto& build_volume = m_plater->build_volume();
    for (const auto group : {PathGroup::Model, PathGroup::Support, PathGroup::SkirtOrBrim, PathGroup::WipeTower}) {
        GCodeProcessorResult grouped;
        const auto group_index = static_cast<size_t>(group);
        for (const auto& move : parsed->moves) {
            if (move.type == EMoveType::Extrude && move.extrusion_role != erCustom &&
                move.width != 0.f && move.height != 0.f && group_for_role(move.extrusion_role) == group)
                grouped.moves.push_back(move);
        }
        if (!grouped.moves.empty())
            group_outside[group_index] = !build_volume.all_paths_inside(grouped, BoundingBoxf3());
    }

    std::ostringstream detail;
    detail << _u8L("检测到超出热床边界的 G-code 路径：");
    bool listed = false;
    const auto append = [&](const std::string& text) {
        if (listed) detail << _u8L("、");
        detail << text;
        listed = true;
    };
    if (group_outside[static_cast<size_t>(PathGroup::Model)]) append(_u8L("模型/墙体/填充"));
    if (group_outside[static_cast<size_t>(PathGroup::Support)]) append(_u8L("支撑"));
    if (group_outside[static_cast<size_t>(PathGroup::SkirtOrBrim)]) append(_u8L("裙边或 Brim"));
    if (group_outside[static_cast<size_t>(PathGroup::WipeTower)]) append(_u8L("擦料塔"));
    if (!listed) detail << _u8L("未知挤出路径");
    detail << _u8L("。请在 Orca 原生预览中定位越界路径后调整模型、支撑或擦料塔位置并重新切片。");
    return detail.str();
}

std::vector<PrinterWorkspaceDevice> OrcaPrinterAdapter::devices() const
{
    std::vector<PrinterWorkspaceDevice> result;
    if (auto* manager = wxGetApp().getDeviceManager()) {
        const std::string agent_id = manager->get_current_printer_agent_id();
        if (agent_id.empty()) return result;
        auto* selected = manager->get_selected_machine();
        std::string configured_host_id;
        if (auto* bundle = wxGetApp().preset_bundle) {
            const auto& config = bundle->printers.get_edited_preset().config;
            const std::string host = config.opt_string("print_host");
            if (!host.empty())
                configured_host_id = MachineObject::dev_id_from_address(host, config.opt_string("printhost_port"));
        }
        for (const auto& entry : manager->get_my_machine_list(agent_id)) {
            if (!entry.second) continue;
            auto* machine = entry.second;
            result.push_back({entry.first, machine->get_dev_name(), machine->get_dev_ip(), agent_id,
                              machine->is_online(), machine->is_connecting(), machine->is_connected(),
                              machine->is_info_ready() && machine->m_push_count > 0,
                              selected == machine, !configured_host_id.empty() && entry.first == configured_host_id,
                              machine->discovered_on_network});
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.selected != right.selected) return left.selected;
        const auto left_state = printer_device_connection_state(
            left.online, left.connecting, left.connected, left.information_ready);
        const auto right_state = printer_device_connection_state(
            right.online, right.connecting, right.connected, right.information_ready);
        if (left_state != right_state) return left_state > right_state;
        return left.name < right.name;
    });
    return result;
}

std::string OrcaPrinterAdapter::active_agent_id() const
{
    auto* manager = wxGetApp().getDeviceManager();
    return manager ? manager->get_current_printer_agent_id() : std::string();
}

PrinterDiscoveryStartResult OrcaPrinterAdapter::request_discovery() const
{
    PrinterDiscoveryStartResult result;
    auto* manager = wxGetApp().getDeviceManager();
    if (!manager) {
        result.error = PrinterDiscoveryError::DeviceManagerUnavailable;
        return result;
    }
    auto* network = wxGetApp().getAgent();
    if (!network) {
        result.error = PrinterDiscoveryError::NetworkAgentUnavailable;
        return result;
    }
    const auto printer_agent = network->get_printer_agent();
    if (!printer_agent) {
        result.error = PrinterDiscoveryError::PrinterAgentUnavailable;
        return result;
    }
    result.agent_id = printer_agent->get_agent_info().id;
    const std::string manager_agent_id = manager->get_current_printer_agent_id();
    if (!printer_discovery_agent_matches(result.agent_id, manager_agent_id)) {
        result.error = PrinterDiscoveryError::AgentMismatch;
        return result;
    }
    for (const auto& [id, machine] : manager->get_my_machine_list(result.agent_id)) {
        (void) id;
        if (machine && machine->discovered_on_network)
            ++result.device_count;
    }
    if (!printer_agent->get_traits().supports_active_discovery) {
        result.error = PrinterDiscoveryError::DiscoveryUnsupported;
        return result;
    }
    try {
        result.accepted = network->start_discovery(true, false);
    } catch (const std::exception&) {
        result.accepted = false;
    }
    if (!result.accepted)
        result.error = PrinterDiscoveryError::RequestRejected;
    else
        result.backend_request_id = printer_agent->discovery_status().request_id;
    return result;
}

PrinterDiscoveryBackendStatus OrcaPrinterAdapter::discovery_status() const
{
    auto* network = wxGetApp().getAgent();
    const auto printer_agent = network ? network->get_printer_agent() : nullptr;
    return printer_agent ? printer_agent->discovery_status() : PrinterDiscoveryBackendStatus{};
}

bool OrcaPrinterAdapter::select_device(const std::string& id) const
{
    auto* manager = wxGetApp().getDeviceManager();
    if (!manager || id.empty()) return false;
    const auto list = devices();
    if (std::none_of(list.begin(), list.end(), [&](const auto& device) { return device.id == id; })) return false;
    wxGetApp().mark_printer_connection_user_initiated(id);
    return manager->set_selected_machine(id);
}

OrcaPrintTargetSnapshot OrcaPrinterAdapter::print_target_snapshot() const
{
    const auto state = snapshot();
    OrcaPrintTargetSnapshot target;
    target.agent_id = state.active_agent_id;
    target.device_id = state.device_id;
    target.device_name = printer_device_display_name(state.device_name, state.device_address);
    target.selected = state.device_selected;
    target.online = state.device_online;
    target.connected = state.connected;
    target.information_ready = state.connection.native_backend ? state.info_ready : state.device_info_ready;
    target.printer_busy = printer_task_active(state.phase);
    target.upload_only_available = printer_can_upload(state);
    target.upload_and_print_available = printer_can_upload_and_print(state);
    target.legacy_print_host = !state.native_dispatch && state.configured_host;
    return target;
}

bool OrcaPrinterAdapter::review_print(wxWindow* parent, bool allow_unready_review) const
{
    const auto reviewed_state = snapshot();
    if (!m_plater || (!allow_unready_review && !printer_can_review(reviewed_state))) return false;
    const auto reviewed_target = print_target_snapshot();
    BOOST_LOG_TRIVIAL(info) << "[PrinterDispatch] review_opened agent="
                            << (reviewed_target.agent_id.empty() ? "none" : reviewed_target.agent_id)
                            << " device_ref=" << print_device_ref(reviewed_target.device_id)
                            << " selected=" << reviewed_target.selected
                            << " online=" << reviewed_target.online
                            << " connected=" << reviewed_target.connected
                            << " information_ready=" << reviewed_target.information_ready
                            << " upload_only=" << reviewed_target.upload_only_available
                            << " upload_and_print=" << reviewed_target.upload_and_print_available
                            << " legacy_print_host=" << reviewed_target.legacy_print_host;
    // Preserve PR20's full workspace revision and native slice/config checks,
    // export, send dialog and cancellation. Opening the review sends nothing.
    const OrcaPrintAction action = show_orca_print_confirmation(parent, *m_plater, reviewed_target);
    BOOST_LOG_TRIVIAL(info) << "[PrinterDispatch] action_selected action=" << print_action_name(action)
                            << " agent=" << (reviewed_target.agent_id.empty() ? "none" : reviewed_target.agent_id)
                            << " device_ref=" << print_device_ref(reviewed_target.device_id);
    if (action == OrcaPrintAction::Cancel)
        return true;

    if (action == OrcaPrintAction::ExportLocal) {
        BOOST_LOG_TRIVIAL(info) << "[PrinterDispatch] route_selected action=export_local route=local_export";
        m_plater->export_gcode(false);
        return true;
    }

    const auto current_state = snapshot();
    const auto current_target = print_target_snapshot();
    const bool target_unchanged = reviewed_target.legacy_print_host ?
        current_target.legacy_print_host :
        current_target.agent_id == reviewed_target.agent_id &&
        current_target.device_id == reviewed_target.device_id;
    const bool action_still_available = current_target.legacy_print_host ?
        action == OrcaPrintAction::UploadAndPrint && printer_can_start(current_state) :
        orca_print_action_available(action, current_target) &&
            (action == OrcaPrintAction::UploadOnly ? printer_can_upload(current_state) :
                                                    printer_can_upload_and_print(current_state));
    if (!target_unchanged || !action_still_available) {
        BOOST_LOG_TRIVIAL(warning) << "[PrinterDispatch] action_rejected action=" << print_action_name(action)
                                   << " reason=" << (!target_unchanged ? "target_changed" : "target_not_ready")
                                   << " reviewed_agent=" << reviewed_target.agent_id
                                   << " current_agent=" << current_target.agent_id
                                   << " reviewed_device_ref=" << print_device_ref(reviewed_target.device_id)
                                   << " current_device_ref=" << print_device_ref(current_target.device_id);
        wxMessageBox(_L("打印机、连接状态或切片已变化，请重新确认。"), _L("打印确认"),
                     wxOK | wxICON_WARNING, wxGetTopLevelParent(parent));
        return true;
    }

    const bool upload_only = action == OrcaPrintAction::UploadOnly;
    const char* route_name = upload_only ? "send_job" :
        (current_target.legacy_print_host ? "print_host" : "print_job");
    BOOST_LOG_TRIVIAL(info) << "[PrinterDispatch] route_selected action=" << print_action_name(action)
                            << " route=" << route_name
                            << " agent=" << current_target.agent_id
                            << " device_ref=" << print_device_ref(current_target.device_id);
    if (!current_target.legacy_print_host) {
        PrinterTransferPresentationContext presentation;
        presentation.presentation = PrinterTransferPresentation::Redesign;
        presentation.action = upload_only ? PrinterTransferAction::UploadOnly :
                                            PrinterTransferAction::UploadAndPrint;
        presentation.agent_id = current_target.agent_id;
        presentation.device_id = current_target.device_id;
        if (upload_only)
            m_plater->show_send_to_printer_dialog_for_current_plate(presentation);
        else
            m_plater->show_select_machine_dialog_for_current_plate(presentation);
    } else {
        SimpleEvent event(EVT_GLTOOLBAR_PRINT_PLATE);
        m_plater->GetEventHandler()->ProcessEvent(event);
    }
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
    const auto state = snapshot();
    const bool allowed = printer_command_allowed(state, target, action);
    BOOST_LOG_TRIVIAL(info) << "[PrinterMonitoring] control_request action=" << task_action_name(action)
                            << " device_ref=" << print_device_ref(target.device_id)
                            << " phase=" << static_cast<int>(state.phase)
                            << " task_identity_required=" << state.requires_task_identity_for_control
                            << " task_identity_present=" << !target.task_id.empty()
                            << " allowed=" << allowed;
    if (!allowed) return false;
    auto* machine = selected_target(target.device_id);
    if (!machine) {
        BOOST_LOG_TRIVIAL(warning) << "[PrinterMonitoring] control_result action=" << task_action_name(action)
                                   << " device_ref=" << print_device_ref(target.device_id)
                                   << " result=target_unavailable";
        return false;
    }
    int result = -1;
    switch (action) {
    case PrinterTaskAction::Pause: result = machine->command_task_pause(); break;
    case PrinterTaskAction::Resume:
        if (machine->check_resume_condition() != 0) {
            BOOST_LOG_TRIVIAL(warning) << "[PrinterMonitoring] control_result action=resume"
                                       << " device_ref=" << print_device_ref(target.device_id)
                                       << " result=resume_condition_rejected";
            return false;
        }
        result = machine->command_task_resume();
        break;
    case PrinterTaskAction::Stop: result = machine->command_task_abort(); break;
    }
    if (result == 0) {
        BOOST_LOG_TRIVIAL(info) << "[PrinterMonitoring] control_result action=" << task_action_name(action)
                                << " device_ref=" << print_device_ref(target.device_id)
                                << " result_code=" << result;
    } else {
        BOOST_LOG_TRIVIAL(warning) << "[PrinterMonitoring] control_result action=" << task_action_name(action)
                                   << " device_ref=" << print_device_ref(target.device_id)
                                   << " result_code=" << result;
    }
    return result == 0;
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
