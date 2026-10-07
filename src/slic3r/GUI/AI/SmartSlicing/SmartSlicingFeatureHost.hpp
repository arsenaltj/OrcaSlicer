#pragma once

#include <functional>
#include <memory>
#include <string>
#include "SmartSlicingWorkbenchState.hpp"

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
    SmartSlicingWorkbenchState workbench_snapshot() const;
    void set_workbench_listener(SmartSlicingWorkbenchListener listener);
    void set_workbench_active(bool active);
    bool analyze_workbench();
    bool keep_current_mesh_and_analyze(const AI::SmartSlicing::WorkspaceRevision& reviewed_revision);
    bool select_goal(AI::SmartSlicing::RecommendationGoal goal);
    AI::SmartSlicing::OfficialSliceResult start_workbench_slice(
        const SmartSlicingWorkbenchState& reviewed,
        const std::vector<AI::SmartSlicing::RiskConfirmationKind>& confirmations);
    bool start_native_slice();
    bool undo_workbench_apply();
    void cancel_workbench_analysis();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::GUI
