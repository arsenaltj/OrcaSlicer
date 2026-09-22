#pragma once

#include <string_view>

namespace Slic3r::GUI {

class RedesignFeatureFlags final
{
public:
    static bool enabled();
    static bool surface_enabled(std::string_view surface);
};

}
