#pragma once

#include "PrinterWorkspaceState.hpp"

#include <vector>
#include <wx/image.h>

namespace Slic3r::GUI {

class Plater;

struct PrinterWorkspaceDevice
{
    std::string id;
    std::string name;
    bool online { false };
};

class OrcaPrinterAdapter final
{
public:
    explicit OrcaPrinterAdapter(Plater* plater) : m_plater(plater) {}
    PrinterWorkspaceSnapshot snapshot() const;
    std::vector<PrinterWorkspaceDevice> devices() const;
    bool select_device(const std::string& id) const;
    bool review_print(wxWindow* parent) const;
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
