#pragma once

#include <functional>
#include <utility>
#include <wx/weakref.h>

namespace Slic3r::GUI {

// Construct, inspect and destroy on the UI thread. Background handoffs share
// this owner instead of copying wxWeakRef or callbacks that contain one:
// copying those references mutates wxTrackable's unsynchronized tracker list.
template<class Window>
class WorkbenchImportUiGuard
{
public:
    WorkbenchImportUiGuard(Window* window, std::function<bool()> current)
        : m_window(window), m_current(std::move(current)) {}

    WorkbenchImportUiGuard(const WorkbenchImportUiGuard&) = delete;
    WorkbenchImportUiGuard& operator=(const WorkbenchImportUiGuard&) = delete;

    Window* window() const { return m_window.get(); }
    bool current() const { return m_window && m_current && m_current(); }

private:
    wxWeakRef<Window> m_window;
    std::function<bool()> m_current;
};

} // namespace Slic3r::GUI
