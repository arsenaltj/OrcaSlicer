#pragma once

#include <cstdint>

class wxWindow;

namespace Slic3r::GUI {

struct WorkbenchImportColorComparison;

enum class WorkbenchImportColorAction { Close, MapToModel, ImportToFilaments };

const char* workbench_import_color_name(uint32_t rgb);
WorkbenchImportColorAction show_workbench_import_color_comparison(
    wxWindow* parent, const WorkbenchImportColorComparison& comparison);

} // namespace Slic3r::GUI
