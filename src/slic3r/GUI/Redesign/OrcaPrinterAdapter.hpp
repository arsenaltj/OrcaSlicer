#pragma once

#include "PrinterWorkspaceState.hpp"
#include "../../Utils/IPrinterAgent.hpp"

#include <cstddef>
#include <vector>
#include <wx/image.h>

namespace Slic3r::GUI {

class Plater;

struct PrinterWorkspaceDevice
{
    std::string id;
    std::string name;
    std::string address;
    std::string agent_id;
    bool online { false };
    bool connecting { false };
    bool connected { false };
    bool information_ready { false };
    bool selected { false };
    bool configured_host_target { false };
    bool discovered_on_network { false };
};

struct PrinterDiscoveryStartResult
{
    bool accepted { false };
    PrinterDiscoveryError error { PrinterDiscoveryError::None };
    std::string agent_id;
    size_t device_count { 0 };
    std::uint64_t backend_request_id {0};
};

class OrcaPrinterAdapter final
{
public:
    explicit OrcaPrinterAdapter(Plater* plater) : m_plater(plater) {}
    PrinterWorkspaceSnapshot snapshot() const;
    bool preview_toolpath_outside() const;
    // Returns a user-facing explanation of the native G-code boundary check.
    // The string is empty when no current preview is outside the build volume.
    std::string preview_toolpath_diagnostic() const;
    PrinterDiscoveryStartResult request_discovery() const;
    PrinterDiscoveryBackendStatus discovery_status() const;
    std::string active_agent_id() const;
    std::vector<PrinterWorkspaceDevice> devices() const;
    bool select_device(const std::string& id) const;
    OrcaPrintTargetSnapshot print_target_snapshot() const;
    bool review_print(wxWindow* parent, bool allow_unready_review = false) const;
    bool start_slice() const;
    bool can_import_model() const;
    bool import_model() const;
    bool control_task(const PrinterCommandTarget& target, PrinterTaskAction action) const;
    bool set_bed_leveling(const std::string& device_id, bool enabled) const;
    bool set_recording(const std::string& device_id, bool enabled) const;
    bool set_detection(const std::string& device_id, bool enabled) const;
    wxImage plate_preview() const;

private:
    Plater* m_plater;
};

} // namespace Slic3r::GUI
