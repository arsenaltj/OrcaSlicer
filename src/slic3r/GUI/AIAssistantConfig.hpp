#ifndef slic3r_GUI_AIAssistantConfig_hpp_
#define slic3r_GUI_AIAssistantConfig_hpp_

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::GUI {

class Plater;

namespace AIAssistantConfig {

using json = nlohmann::json;

struct ValidatedChange
{
    Preset::Type preset_type { Preset::TYPE_PRINT };
    std::string  scope;
    std::string  key;
    std::string  old_value;
    std::string  new_value;
    std::string  reason;
};

struct ValidationResult
{
    std::vector<ValidatedChange> accepted;
    std::vector<std::string>     rejected;
};

json build_context(const Plater& plater, const std::string& user_message, const std::string& request_id);
json allowed_changes();
ValidationResult validate_proposal(const json& proposal);
DynamicPrintConfig build_patch_config(const std::vector<ValidatedChange>& changes);
json validation_result_to_json(const ValidationResult& result);

} // namespace AIAssistantConfig
} // namespace Slic3r::GUI

#endif // slic3r_GUI_AIAssistantConfig_hpp_
