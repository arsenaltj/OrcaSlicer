#pragma once

#include "slic3r/GUI/AIModelGenerationClient.hpp"

#include <wx/string.h>

#include <string>

namespace Slic3r::GUI {
struct ModelGenerationUIState;

namespace ModelGenerationStatusText {

wxString localized_service_error(const std::string& error);
wxString localized_job_status(const AIModelGenerationClient::JobStatus& status, bool advanced = false);
// Formats a temporary advanced display projection, never the host's task state.
void localize_advanced_model_state(ModelGenerationUIState& state);
wxString localized_advanced_service_error(const std::string& error);
void present_advanced_model(ModelGenerationUIState& state);
wxString model_input_quality_label(const std::string& code);

} // namespace ModelGenerationStatusText
} // namespace Slic3r::GUI
