#pragma once

#include <functional>
#include <sstream>
#include <string>

namespace Slic3r::GUI {

enum class PrinterTransferPresentation {
    Native,
    Redesign
};

enum class PrinterTransferAction {
    UploadOnly,
    UploadAndPrint
};

struct PrinterTransferPresentationContext
{
    PrinterTransferPresentation presentation { PrinterTransferPresentation::Native };
    PrinterTransferAction action { PrinterTransferAction::UploadOnly };
    std::string agent_id;
    std::string device_id;

    bool redesign() const { return presentation == PrinterTransferPresentation::Redesign; }
};

inline const char* printer_transfer_action_name(PrinterTransferAction action)
{
    return action == PrinterTransferAction::UploadOnly ? "upload_only" : "upload_and_print";
}

inline std::string printer_transfer_device_ref(const std::string& value)
{
    if (value.empty())
        return "none";
    std::ostringstream output;
    output << std::hex << std::hash<std::string>{}(value);
    return output.str();
}

} // namespace Slic3r::GUI
