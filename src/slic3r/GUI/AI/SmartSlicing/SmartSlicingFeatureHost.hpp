#pragma once

#include <functional>
#include <memory>
#include <string>

class wxAuiManager;

namespace Slic3r::GUI {

class Plater;
class Sidebar;

class SmartSlicingFeatureHost final
{
public:
    using StartOfficialSliceFn = std::function<bool()>;

    SmartSlicingFeatureHost(Plater& plater, wxAuiManager& aui_manager, Sidebar& sidebar,
                            StartOfficialSliceFn start_official_slice);
    ~SmartSlicingFeatureHost();

    SmartSlicingFeatureHost(const SmartSlicingFeatureHost&) = delete;
    SmartSlicingFeatureHost& operator=(const SmartSlicingFeatureHost&) = delete;

    bool is_shown() const;
    void show(bool show);
    void notify_slice_completed(bool success, const std::string& failure_code);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::GUI
