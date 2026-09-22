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

bool surface_value(std::string_view surface)
{
    const std::string variable = "ORCASLICER_UI_REDESIGN_" + std::string(surface);
    return is_enabled_value(std::getenv(variable.c_str()));
}

}

bool RedesignFeatureFlags::enabled()
{
    return is_enabled_value(std::getenv("ORCASLICER_UI_REDESIGN"));
}

bool RedesignFeatureFlags::surface_enabled(std::string_view surface)
{
    return enabled() || surface_value(surface);
}

}
