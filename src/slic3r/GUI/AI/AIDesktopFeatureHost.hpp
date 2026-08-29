#pragma once

#include <functional>
#include <memory>

class wxWindow;

namespace Slic3r::GUI {

class Plater;

class AIDesktopFeatureHost final
{
public:
    using NavigateAfterImportFn = std::function<void(bool slice)>;
    using SmartSlicingAvailableFn = std::function<void()>;

    AIDesktopFeatureHost(wxWindow* parent, Plater* plater, NavigateAfterImportFn navigate_after_import,
                         SmartSlicingAvailableFn smart_slicing_available);
    ~AIDesktopFeatureHost();

    AIDesktopFeatureHost(const AIDesktopFeatureHost&) = delete;
    AIDesktopFeatureHost& operator=(const AIDesktopFeatureHost&) = delete;

    wxWindow* model_generation_panel() const;
    void start();
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::GUI
