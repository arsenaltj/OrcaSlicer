#pragma once
#include <wx/string.h>
#include <functional>
#include <vector>

namespace Slic3r::Diagnostics {
// Shared writer for the existing troubleshooting export and diagnostic log snapshots.
bool save_zip_sources(const std::vector<wxString>& sources,const wxString& target,
    const std::function<bool()>& cancelled={});
}
