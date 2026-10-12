#pragma once

#include "slic3r/AI/Contracts/ColorIntent.hpp"
#include <boost/filesystem/path.hpp>
#include <wx/string.h>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace Slic3r::GUI {

// Read-only asset projection. Strings are presentation values, never controls;
// loading an asset still reads its authoritative model/history document.
struct ModelLibraryEntry {
    wxString title, details, search_text;
    boost::filesystem::path model_path, preview_path, reference_image_path, ai_image_path, color_intent_path;
    std::vector<std::string> palette;
    std::map<std::string, std::string> palette_roles;
    std::string color_intent_schema, color_intent_sha256;
    std::string job_id, provider_name, provider_task_id, provider_conversion_task_id;
    std::time_t generated_at {0}, imported_at {0};
    size_t triangle_count {0};
    double load_seconds {0.0};
    std::string print_feedback;
    bool use_printable_colors {false}, design_only {false}, accepted_finishing {false};
};

} // namespace Slic3r::GUI
