#include "RedesignFeatureFlags.hpp"

#include <cstdlib>
#include <string>

namespace Slic3r::GUI {
namespace {

bool is_enabled_value(const char* value)
{
    if (value == nullptr)
        return false;

    const std::string normalized(value);
    return normalized == "1" || normalized == "true" || normalized == "TRUE" ||
           normalized == "on" || normalized == "ON" || normalized == "yes" || normalized == "YES";
}

}

bool RedesignFeatureFlags::enabled()
{
    return is_enabled_value(std::getenv("ORCASLICER_UI_REDESIGN"));
}

bool RedesignFeatureFlags::model_workflow_review_enabled()
{
    return is_enabled_value(std::getenv("ORCASLICER_MODEL_WORKFLOW_REVIEW"));
}

bool RedesignFeatureFlags::surface_enabled(std::string_view surface)
{
    const std::string variable = "ORCASLICER_UI_REDESIGN_" + std::string(surface);
    if (const char* value = std::getenv(variable.c_str()))
        return is_enabled_value(value);
    if (const char* value = std::getenv("ORCASLICER_UI_REDESIGN"))
        return is_enabled_value(value);
    return surface == "IMAGE_HOME";
}

bool RedesignFeatureFlags::image_home_on_startup(bool shell_active, bool has_input_files,
                                                  bool default_prepare, bool restore_available)
{
    return shell_active && !has_input_files && !default_prepare && !restore_available;
}

}
