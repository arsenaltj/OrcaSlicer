#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r::AI {

struct PrintablePaletteSnapshot
{
    std::vector<std::string> project_colors;
    std::vector<size_t>      valid_slots;
    std::vector<size_t>      compatible_slots;
    std::vector<std::string> compatible_colors;
};

class IPrintablePaletteProvider
{
public:
    virtual ~IPrintablePaletteProvider() = default;

    virtual PrintablePaletteSnapshot printable_palette() const = 0;
};

} // namespace Slic3r::AI
