#include "SmartSlicingFeatureHost.hpp"

#include "slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.hpp"
#include "slic3r/GUI/AI/Orca/OrcaParameterProposalAdapter.hpp"
#include "slic3r/GUI/AI/Orca/OrcaSmartSlicingAdapter.hpp"
#include "slic3r/GUI/AI/Orca/OrcaTrialSliceExecutor.hpp"
#include "slic3r/GUI/AI/Orca/OrcaWorkflowRuntimeStore.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingPanel.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingPresenter.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"

#include <wx/aui/framemanager.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>

namespace Slic3r::GUI {
namespace {

struct TransformTarget
{
    size_t object_index { 0 };
    ModelInstance* instance { nullptr };
    Transform3d matrix { Transform3d::Identity() };
};

bool collect_transform_targets(Plater& plater, const AI::SmartSlicing::SliceCandidate& candidate,
                               std::vector<TransformTarget>& targets, std::string& diagnostic)
{
    PartPlate* plate = plater.get_partplate_list().get_curr_plate();
    if (plate == nullptr) {
        diagnostic = "current_plate_unavailable";
        return false;
    }
    if (plate->is_locked()) {
        diagnostic = "current_plate_locked";
        return false;
    }

    std::set<uint64_t> seen_instances;
    targets.reserve(candidate.placement.transforms.size());
    Model& model = plater.model();
    for (const AI::SmartSlicing::ObjectTransform& requested : candidate.placement.transforms) {
        if (!seen_instances.insert(requested.instance_id).second) {
            diagnostic = "duplicate_transform_target";
            return false;
        }
        Transform3d matrix;
        for (Eigen::Index row = 0; row < matrix.rows(); ++row)
            for (Eigen::Index column = 0; column < matrix.cols(); ++column) {
                const double value = requested.matrix[static_cast<size_t>(row * matrix.cols() + column)];
                if (!std::isfinite(value)) {
                    diagnostic = "invalid_transform";
                    return false;
                }
                matrix(row, column) = value;
            }
        if (!matrix.matrix().row(3).isApprox(Eigen::RowVector4d(0.0, 0.0, 0.0, 1.0)) ||
            std::abs(matrix.linear().determinant()) < 1e-12) {
            diagnostic = "invalid_transform";
            return false;
        }

        TransformTarget target;
        bool found = false;
        for (size_t object_index = 0; object_index < model.objects.size() && !found; ++object_index) {
            ModelObject* object = model.objects[object_index];
            if (object == nullptr || object->id().id != requested.object_id)
                continue;
            for (size_t instance_index = 0; instance_index < object->instances.size(); ++instance_index) {
                ModelInstance* instance = object->instances[instance_index];
                if (instance != nullptr && instance->id().id == requested.instance_id) {
                    if (!plate->contain_instance(static_cast<int>(object_index), static_cast<int>(instance_index))) {
                        diagnostic = "transform_target_not_on_current_plate";
                        return false;
                    }
                    target = { object_index, instance, matrix };
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            diagnostic = "transform_target_missing";
            return false;
        }
        targets.push_back(std::move(target));
    }
    return true;
}

bool prepare_parameter_patch(Plater& plater, const AI::SmartSlicing::SliceCandidate& candidate,
                             DynamicPrintConfig& plate_patch, std::string& diagnostic)
{
    if (candidate.parameters.entries.empty())
        return true;
    if (wxGetApp().preset_bundle == nullptr) {
        diagnostic = "current_config_unavailable";
        return false;
    }
    PartPlate* plate = plater.get_partplate_list().get_curr_plate();
    if (plate == nullptr) {
        diagnostic = "current_plate_unavailable";
        return false;
    }

    DynamicPrintConfig current_config = wxGetApp().preset_bundle->full_config();
    current_config.apply(*plate->config(), true);
    DynamicPrintConfig patched_config;
    const OrcaParameterApplyResult result = OrcaParameterProposalAdapter().validate_and_apply(
        candidate.parameters, plate->id().id, current_config, patched_config);
    if (!result.accepted) {
        diagnostic = result.diagnostic_code;
        return false;
    }
    for (const AI::SmartSlicing::ConfigPatchEntry& entry : candidate.parameters.entries) {
        const ConfigOption* replacement = patched_config.option(entry.key);
        if (replacement == nullptr) {
            diagnostic = "parameter_native_option_unavailable";
            return false;
        }
        plate_patch.set_key_value(entry.key, replacement->clone());
    }
    return true;
}

} // namespace

struct SmartSlicingFeatureHost::Impl
{
    Impl(Plater& plater, wxAuiManager& aui_manager, Sidebar& sidebar, StartOfficialSliceFn start_official_slice)
        : plater(plater)
        , aui_manager(aui_manager)
        , sidebar(sidebar)
        , workspace(std::make_unique<OrcaSmartSlicingAdapter>(&plater))
        , trial_executor(std::make_unique<OrcaTrialSliceExecutor>([this] {
            return workspace->capture_trial_slice_input();
        }))
        , official_gateway(std::make_unique<OrcaOfficialSliceGateway>(
            [this] { return workspace->current_revision(); },
            [this](const AI::SmartSlicing::SliceCandidate& candidate) { return validate_candidate(candidate); },
            [this](const AI::SmartSlicing::SliceCandidate& candidate) { return apply_candidate(candidate); },
            std::move(start_official_slice),
            [this] {
                this->plater.select_view_3D("Preview");
                return this->plater.is_preview_shown();
            },
            [this] {
                if (!this->plater.can_undo())
                    return false;
                this->plater.undo();
                return true;
            }))
        , coordinator(std::make_unique<AI::SmartSlicing::SmartSlicingCoordinator>(
            *workspace, *trial_executor, *official_gateway))
        , runtime_store(std::make_unique<OrcaWorkflowRuntimeStore>(
            boost::filesystem::temp_directory_path() / "OrcaSlicer-smart-slicing-runtime-v1.json"))
        , presenter(std::make_unique<SmartSlicingPresenter>(*coordinator, [](std::function<void()> publish) {
            if (wxIsMainThread())
                publish();
            else
                wxGetApp().CallAfter(std::move(publish));
        }))
    {
        AI::SmartSlicing::WorkflowResourceBudget budget;
        trial_executor->set_resource_limits(
            budget.maximum_elapsed, budget.maximum_memory_bytes, budget.maximum_temporary_disk_bytes);
        coordinator->set_resource_budget(budget);
        coordinator->set_runtime_store(*runtime_store);

        panel = new SmartSlicingPanel(&plater, *coordinator, [this] {
            const auto& snapshot = coordinator->snapshot();
            if (!snapshot.context)
                return std::vector<AI::SmartSlicing::SliceCandidate> {};
            trial_executor->prepare_session_input(workspace->capture_trial_slice_input());
            return workspace->candidate_proposals(snapshot.context->revision);
        }, [this] {
            trial_executor->cancel_trial_slice();
        });
        presenter->set_view_changed([this](const SmartSlicingViewModel& view) { render(view); });

        aui_manager.AddPane(panel, wxAuiPaneInfo()
                                       .Name("smart_slicing")
                                       .Caption(_L("智能切片"))
                                       .Right()
                                       .CloseButton(true)
                                       .TopDockable(false)
                                       .BottomDockable(false)
                                       .BestSize(wxSize(38 * wxGetApp().em_unit(), 70 * wxGetApp().em_unit()))
                                       .Hide());
        aui_manager.Update();
    }

    std::string validate_candidate(const AI::SmartSlicing::SliceCandidate& candidate)
    {
        std::vector<TransformTarget> targets;
        DynamicPrintConfig parameter_patch;
        std::string diagnostic;
        if (!collect_transform_targets(plater, candidate, targets, diagnostic) ||
            !prepare_parameter_patch(plater, candidate, parameter_patch, diagnostic))
            return diagnostic;
        return {};
    }

    OrcaApplyMutationResult apply_candidate(const AI::SmartSlicing::SliceCandidate& candidate)
    {
        std::vector<TransformTarget> targets;
        DynamicPrintConfig parameter_patch;
        std::string diagnostic;
        if (!collect_transform_targets(plater, candidate, targets, diagnostic) ||
            !prepare_parameter_patch(plater, candidate, parameter_patch, diagnostic))
            return { false, false, std::move(diagnostic) };

        std::vector<TransformTarget> changed;
        std::vector<size_t> changed_object_indices;
        for (const TransformTarget& target : targets) {
            if (!target.instance->get_matrix().isApprox(target.matrix)) {
                changed.push_back(target);
                changed_object_indices.push_back(target.object_index);
            }
        }
        if (changed.empty() && candidate.parameters.entries.empty())
            return { true, false, {} };
        std::sort(changed_object_indices.begin(), changed_object_indices.end());
        changed_object_indices.erase(
            std::unique(changed_object_indices.begin(), changed_object_indices.end()), changed_object_indices.end());

        bool transaction_started = false;
        try {
            {
                Plater::TakeSnapshot transaction(&plater, "Apply Smart Slicing Candidate");
                transaction_started = true;
                for (const TransformTarget& target : changed)
                    target.instance->set_transformation(Geometry::Transformation(target.matrix));
                PartPlate* plate = plater.get_partplate_list().get_curr_plate();
                if (plate == nullptr)
                    throw std::runtime_error("Current plate disappeared while applying a smart-slicing candidate.");
                for (const AI::SmartSlicing::ConfigPatchEntry& entry : candidate.parameters.entries) {
                    const ConfigOption* replacement = parameter_patch.option(entry.key);
                    if (replacement == nullptr)
                        throw std::runtime_error("Validated smart-slicing parameter disappeared before apply.");
                    plate->config()->set_key_value(entry.key, replacement->clone());
                }
                if (!changed_object_indices.empty())
                    plater.changed_objects(changed_object_indices);
                if (!candidate.parameters.entries.empty())
                    plate->update_slice_result_valid_state(false);
                plater.update_title_dirty_status();
            }
            return { true, true, {} };
        } catch (...) {
            if (transaction_started && plater.can_undo())
                plater.undo();
            return { false, false, "candidate_apply_rolled_back" };
        }
    }

    void render(const SmartSlicingViewModel& view)
    {
        if (panel != nullptr)
            panel->render(view);
        if (view.summary_key == "official_slice_complete" || view.summary_key == "canceled" ||
            view.summary_key == "workspace_changed" || view.summary_key == "preflight_failed")
            trial_executor->clear_session_input();

        if (view.summary_key == "ready_to_start")
            return;
        auto to_sidebar_status = [](LegacyAIWorkflowStatus status) {
            switch (status) {
            case LegacyAIWorkflowStatus::Running: return Sidebar::AIWorkflowStatus::Running;
            case LegacyAIWorkflowStatus::Success: return Sidebar::AIWorkflowStatus::Success;
            case LegacyAIWorkflowStatus::Warning: return Sidebar::AIWorkflowStatus::Warning;
            case LegacyAIWorkflowStatus::Failed: return Sidebar::AIWorkflowStatus::Failed;
            case LegacyAIWorkflowStatus::Waiting: return Sidebar::AIWorkflowStatus::Waiting;
            }
            return Sidebar::AIWorkflowStatus::Waiting;
        };
        const wxString summary = view.is_stale ? _L("工程已变化，需要重新检查") :
                                 view.summary_key == "preflight_complete" ? _L("可打印性检查完成") :
                                 view.summary_key == "preflight_complete_with_warnings" ? _L("可打印性检查完成，仍有提示") :
                                 view.summary_key == "printability_action_required" ? _L("发现需要处理的问题") :
                                 view.summary_key == "preflight_failed" ? _L("可打印性检查失败") :
                                 view.summary_key == "canceled" ? _L("可打印性检查已取消") :
                                 _L("正在执行智能切片预检");
        sidebar.start_ai_workflow(summary);
        for (size_t index = 0; index < view.legacy_steps.size(); ++index)
            sidebar.update_ai_workflow_step(
                static_cast<Sidebar::AIWorkflowStep>(index), to_sidebar_status(view.legacy_steps[index]));
        if (view.can_start && !view.can_cancel)
            sidebar.finish_ai_workflow(false, summary);
    }

    bool is_shown() const
    {
        return panel != nullptr && aui_manager.GetPane(panel).IsShown();
    }

    void show(bool should_show)
    {
        if (panel == nullptr)
            return;
        auto& pane = aui_manager.GetPane(panel);
        if (!pane.IsOk())
            return;
        pane.Show(should_show);
        aui_manager.Update();
    }

    Plater& plater;
    wxAuiManager& aui_manager;
    Sidebar& sidebar;
    std::unique_ptr<OrcaSmartSlicingAdapter> workspace;
    std::unique_ptr<OrcaTrialSliceExecutor> trial_executor;
    std::unique_ptr<OrcaOfficialSliceGateway> official_gateway;
    std::unique_ptr<AI::SmartSlicing::SmartSlicingCoordinator> coordinator;
    std::unique_ptr<OrcaWorkflowRuntimeStore> runtime_store;
    std::unique_ptr<SmartSlicingPresenter> presenter;
    SmartSlicingPanel* panel { nullptr };
};

SmartSlicingFeatureHost::SmartSlicingFeatureHost(Plater& plater, wxAuiManager& aui_manager, Sidebar& sidebar,
                                                 StartOfficialSliceFn start_official_slice)
    : m_impl(std::make_unique<Impl>(plater, aui_manager, sidebar, std::move(start_official_slice)))
{}

SmartSlicingFeatureHost::~SmartSlicingFeatureHost() = default;

bool SmartSlicingFeatureHost::is_shown() const
{
    return m_impl->is_shown();
}

void SmartSlicingFeatureHost::show(bool show)
{
    m_impl->show(show);
}

void SmartSlicingFeatureHost::notify_slice_completed(bool success, const std::string& failure_code)
{
    m_impl->official_gateway->notify_slice_completed(success, failure_code);
}

} // namespace Slic3r::GUI
