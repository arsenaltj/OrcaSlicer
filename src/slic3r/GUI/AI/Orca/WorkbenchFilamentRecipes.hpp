#pragma once

#include "../ModelGeneration/ModelPreviewPalette.hpp"
#include <string>
#include <vector>
namespace Slic3r { class PresetBundle; }
namespace Slic3r::GUI::WorkbenchFilamentRecipes {
PreviewPalette::Color color(const std::string& hex);
double distance(const std::string& a, const std::string& b);
double reconstruction_error(const std::vector<std::string>& targets, const std::vector<size_t>& bases);
std::vector<size_t> select_bases(const std::vector<std::string>& targets, size_t capacity);
bool compatible_components(const PresetBundle& bundle, const std::vector<unsigned>& ids);
bool valid_recipe(const PresetBundle& bundle, size_t slot);
bool stage_targets(PresetBundle& staged, const std::vector<std::string>& targets,
    std::vector<size_t>& bindings, std::string& error, const std::vector<size_t>& preferred = {});
} // namespace Slic3r::GUI::WorkbenchFilamentRecipes
