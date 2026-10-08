#pragma once

#include <string_view>

namespace Slic3r::GUI {

class RedesignFeatureFlags final
{
public:
    static bool enabled();
    static bool surface_enabled(std::string_view surface);
    static bool model_workflow_review_enabled();
    static bool image_home_on_startup(bool shell_active, bool has_input_files,
                                      bool default_prepare, bool restore_available);
};

}
