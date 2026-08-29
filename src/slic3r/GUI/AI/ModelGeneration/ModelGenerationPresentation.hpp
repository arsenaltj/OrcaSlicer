#pragma once

#include "slic3r/GUI/AIModelGenerationClient.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json_fwd.hpp>
#include <wx/defs.h>
#include <wx/string.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

class wxStaticText;
class wxWindow;

namespace Slic3r::GUI::ModelGenerationPresentation {

inline constexpr size_t MAX_MODEL_INPUT_BYTES = 2000;
inline constexpr double MIN_PREVIEW_ZOOM = 0.5;
inline constexpr double MAX_PREVIEW_ZOOM = 4.0;
inline constexpr int MAX_PREVIEW_BITMAP_DIMENSION = 4096;
inline constexpr std::array<const char*, 4> PALETTE_ROLE_IDS {"primary", "structure", "light", "accent"};

wxString thin_local_region_metrics(
    const AIModelGenerationClient::ModelQuality::ThinLocalRegion& region,
    bool threshold_available,
    double minimum_wall_thickness_mm);
wxString thin_local_region_status(
    size_t region_index,
    size_t region_count,
    const AIModelGenerationClient::ModelQuality::ThinLocalRegion& region,
    bool threshold_available,
    double minimum_wall_thickness_mm);
AIModelGenerationClient::PaletteRoles automatic_palette_roles(const std::vector<std::string>& palette);
bool same_palette_color(const std::string& left, const std::string& right);
wxString palette_role_label(const std::string& role);
double minimum_palette_distance(const std::vector<std::string>& palette);
int remap_progress(int value, int input_start, int input_end, int output_start, int output_end);
int display_progress(const AIModelGenerationClient::JobStatus& status);
std::string new_request_id();
bool is_supported_image(const boost::filesystem::path& path);
bool is_nonempty_obj(const boost::filesystem::path& path);
boost::filesystem::path generated_models_root();
boost::filesystem::path temp_path(const std::string& job_id, const std::string& extension);
boost::filesystem::path library_metadata_path(const std::string& job_id);
std::string download_job_id(const boost::filesystem::path& path);
nlohmann::json read_json(const boost::filesystem::path& path);
bool write_json(const boost::filesystem::path& path, const nlohmann::json& value);
bool path_is_inside(const boost::filesystem::path& root, const boost::filesystem::path& candidate);
wxString model_load_summary(size_t triangle_count, double load_seconds);
wxString style_label(const std::string& style);
wxStaticText* section_label(wxWindow* parent, const wxString& text);
wxString model_quality_code_label(const std::string& code);
wxString visual_quality_code_label(const std::string& code);

} // namespace Slic3r::GUI::ModelGenerationPresentation
