#include "SmartSlicingFeatureHost.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationInputStyle.hpp"

#include "slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.hpp"
#include "slic3r/GUI/AI/Orca/OrcaParameterProposalAdapter.hpp"
#include "slic3r/GUI/AI/Orca/OrcaSmartSlicingAdapter.hpp"
#include "slic3r/GUI/AI/Orca/OrcaTrialSliceExecutor.hpp"
#include "slic3r/GUI/AI/Orca/OrcaWorkflowRuntimeStore.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingPanel.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingPresenter.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "slic3r/AI/SmartSlicing/Application/CandidateSearchPipeline.hpp"
#include "slic3r/AI/SmartSlicing/Application/CandidateSearchSessionPlanner.hpp"
#include "slic3r/AI/SmartSlicing/Application/RecommendationSessionCoordinator.hpp"
#include "slic3r/AI/SmartSlicing/Application/RecommendationWorkerFailure.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/AI/SmartSlicing/Application/TrialSliceScheduler.hpp"
#include "slic3r/AI/SmartSlicing/Application/VersionedApplyWorkflow.hpp"

#include <wx/aui/framemanager.h>
#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/weakref.h>
#include <wx/glcanvas.h>
#include <wx/timer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <map>

#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>

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
                             std::vector<OrcaObjectParameterPatch>& object_patches, std::string& diagnostic)
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
    std::vector<ModelObject*> targets;
    Model& model = plater.model();
    for (size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        auto* object = model.objects[object_index];
        if (object == nullptr || !object->printable)
            continue;
        bool on_plate = false, outside_plate = false;
        for (size_t i = 0; i < object->instances.size(); ++i) {
            const auto* instance = object->instances[i];
            if (instance == nullptr) continue;
            if (plate->contain_instance(static_cast<int>(object_index), static_cast<int>(i))) {
                if (instance->printable) on_plate = true;
            } else {
                outside_plate = true;
            }
        }
        if (!on_plate) continue;
        if (outside_plate) {
            diagnostic = "parameter_object_shared_across_plates";
            return false;
        }
        targets.push_back(object);
    }
    const auto result = OrcaParameterProposalAdapter().prepare_object_patches(
        candidate.parameters, plate->id().id, current_config, targets, object_patches);
    if (!result.accepted) {
        diagnostic = result.diagnostic_code;
        return false;
    }
    return true;
}

bool prepare_parameter_patch(Plater& plater, const AI::SmartSlicing::SliceCandidate& candidate,
                             DynamicPrintConfig& plate_patch, std::string& diagnostic,
                             bool* has_effective_changes = nullptr)
{
    if (has_effective_changes != nullptr)
        *has_effective_changes = false;
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
    OrcaSmartSlicingAdapter adapter(&plater);
    const auto intent_constraints = adapter.capture_context().intent_constraints;
    const auto captured = adapter.capture_candidate_search_input();
    if (!captured.completed()) { diagnostic = captured.diagnostic_code; return false; }
    std::vector<AI::SmartSlicing::ParameterBoundEvidence> bounds;
    for (const auto& parameter : captured.input->profile_parameters)
        bounds.insert(bounds.end(), parameter.bounds.begin(), parameter.bounds.end());
    DynamicPrintConfig patched_config;
    const OrcaParameterApplyResult result = OrcaParameterProposalAdapter().validate_and_apply(
        candidate.parameters, plate->id().id, current_config, intent_constraints, bounds, patched_config);
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
        const ConfigOption* current = current_config.option(entry.key);
        if (has_effective_changes != nullptr &&
            (current == nullptr || current->type() != replacement->type() || *current != *replacement))
            *has_effective_changes = true;
        plate_patch.set_key_value(entry.key, replacement->clone());
    }
    return true;
}

class RecommendationHostEventBindings
{
public:
    ~RecommendationHostEventBindings() { unbind(); }

    void bind(wxEvtHandler& handler, int timer_id, std::function<void()> on_timer,
              std::function<void(bool)> on_show)
    {
        unbind();
        m_handler = &handler;
        m_timer_id = timer_id;
        m_on_timer = std::move(on_timer);
        m_on_show = std::move(on_show);
        m_handler->Bind(wxEVT_TIMER, &RecommendationHostEventBindings::handle_timer, this,
                        m_timer_id);
        m_handler->Bind(wxEVT_SHOW, &RecommendationHostEventBindings::handle_show, this);
    }

    void unbind()
    {
        if (m_handler != nullptr) {
            m_handler->Unbind(wxEVT_TIMER, &RecommendationHostEventBindings::handle_timer, this,
                              m_timer_id);
            m_handler->Unbind(wxEVT_SHOW, &RecommendationHostEventBindings::handle_show, this);
        }
        m_handler = nullptr;
        m_timer_id = wxID_ANY;
        m_on_timer = {};
        m_on_show = {};
    }

private:
    void handle_timer(wxTimerEvent&)
    {
        if (m_on_timer)
            m_on_timer();
    }

    void handle_show(wxShowEvent& event)
    {
        if (m_on_show)
            m_on_show(event.IsShown());
        event.Skip();
    }

    wxEvtHandler* m_handler{nullptr};
    int m_timer_id{wxID_ANY};
    std::function<void()> m_on_timer;
    std::function<void(bool)> m_on_show;
};

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
            start_official_slice,
            [this] {
                this->plater.select_view_3D("Preview");
                if (this->plater.IsShownOnScreen() && wxGetApp().mainframe)
                    wxGetApp().mainframe->select_tab(TAB_ID_PREVIEW);
                return this->plater.is_preview_shown();
            },
            [this] {
                if (!applied_snapshot_time ||
                    this->plater.undo_redo_stack_main().active_snapshot_time() != *applied_snapshot_time ||
                    this->plater.get_view3D_canvas3D()->get_gizmos_manager().is_running())
                    return false;
                this->plater.select_view_3D("3D");
                if (!this->plater.can_undo())
                    return false;
                this->plater.undo();
                if (wxGetApp().mainframe) wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);
                return this->plater.undo_redo_stack_main().active_snapshot_time() != *applied_snapshot_time;
            },
            [this](const AI::SmartSlicing::AtomicApplyPlan& plan) { return apply_plan(plan); },
            [] { return wxIsMainThread(); },
            [this](const AI::SmartSlicing::OfficialApplyTransactionIdentity& transaction) {
                if (!wxIsMainThread())
                    return false;
                const UndoRedo::ActionSnapshotIdentity identity{
                    static_cast<size_t>(transaction.snapshot.action_snapshot_time),
                    static_cast<size_t>(transaction.snapshot.applied_snapshot_time),
                    transaction.snapshot.action_name};
                return this->plater.main_snapshot_identity_is_current(identity);
            },
            [this](const AI::SmartSlicing::OfficialApplyTransactionIdentity& transaction,
                   std::string& diagnostic) {
                if (!wxIsMainThread()) {
                    diagnostic = "apply_undo_requires_owner_thread";
                    return false;
                }
                const UndoRedo::ActionSnapshotIdentity identity{
                    static_cast<size_t>(transaction.snapshot.action_snapshot_time),
                    static_cast<size_t>(transaction.snapshot.applied_snapshot_time),
                    transaction.snapshot.action_name};
                this->plater.select_view_3D("3D");
                return this->plater.undo_main_snapshot_exact(identity, diagnostic);
            }))
        , versioned_apply_workflow(
              std::make_unique<AI::SmartSlicing::VersionedApplyWorkflow>(*official_gateway))
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
        native_slice = std::move(start_official_slice);
        AI::SmartSlicing::WorkflowResourceBudget budget;
        trial_executor->set_resource_limits(
            budget.maximum_elapsed, budget.maximum_memory_bytes, budget.maximum_temporary_disk_bytes);
        coordinator->set_resource_budget(budget);
        coordinator->set_runtime_store(*runtime_store);
        coordinator->set_versioned_apply_callbacks(
            [this](const AI::SmartSlicing::WorkflowSnapshot&,
                   const AI::SmartSlicing::SliceCandidate& candidate) {
                AI::SmartSlicing::ApplyExpectedContext context;
                context.selected_candidate = candidate;
                context.goal = AI::SmartSlicing::RecommendationGoal::Balanced;
                context.workspace.revision = candidate.base_revision;
                AI::SmartSlicing::VersionedApplyRequest request;
                request.command_id = "smart-slicing-apply";
                // The current development panel does not yet publish the owner risk
                // confirmation contract. VersionedApplyWorkflow therefore rejects
                // before commit_plan; legacy apply is intentionally unreachable here.
                return versioned_apply_workflow->start(request, context);
            },
            [this] { return versioned_apply_workflow->poll(); },
            [this] {
                const auto& transaction = versioned_apply_workflow->active_transaction();
                if (!transaction)
                    return AI::SmartSlicing::OfficialSliceResult{
                        AI::SmartSlicing::OfficialSlicePhase::Rejected,
                        "apply_undo_unavailable", false, false};
                return versioned_apply_workflow->undo(*transaction);
            });

        panel = new SmartSlicingPanel(&plater, *coordinator, [this] {
            start_candidate_search_session();
            coordinator->cancel();
            return std::vector<AI::SmartSlicing::SliceCandidate> {};
        }, [this] {
            trial_executor->cancel_trial_slice();
            cancel_recommendation_session(AI::SmartSlicing::RecommendationCancellationReason::User);
        }, [this] {
            this->plater.add_file();
        }, &plater, [executor = trial_executor.get()](const AI::SmartSlicing::SliceCandidate& candidate) {
            return executor->execute_trial_slice(candidate);
        }, [](std::function<void()> callback) {
            if (wxIsMainThread())
                callback();
            else
                wxGetApp().CallAfter(std::move(callback));
        }, [this](SmartSlicingMode mode) {
            presenter->set_mode(mode);
            if (mode == SmartSlicingMode::Orca) {
                cancel_recommendation_session(AI::SmartSlicing::RecommendationCancellationReason::ModeChanged);
                show(false);
            }
        }, [this](SmartSlicingPurpose purpose) {
            presenter->set_purpose(purpose);
            using AI::SmartSlicing::UsagePurpose;
            switch (purpose) {
            case SmartSlicingPurpose::Decoration: workspace->set_usage_purpose(UsagePurpose::Decoration); break;
            case SmartSlicingPurpose::Functional: workspace->set_usage_purpose(UsagePurpose::Functional); break;
            case SmartSlicingPurpose::General: workspace->set_usage_purpose(UsagePurpose::General); break;
            }
        });
        recommendation_revision_timer = std::make_unique<wxTimer>(panel);
        recommendation_event_bindings.bind(
            *panel, recommendation_revision_timer->GetId(),
            [this] { process_recommendation_timer(); }, [this](bool shown) {
            if (!shown && !workbench_active)
                cancel_recommendation_session(
                    AI::SmartSlicing::RecommendationCancellationReason::ModeChanged);
        });
        presenter->set_view_changed([this](const SmartSlicingViewModel& view) { render(view); });
        recommendation_coordinator.set_cleanup_handler(
            [this](AI::SmartSlicing::RecommendationCancellationReason) {
                trial_executor->cancel_trial_slice();
            });
        entry_button = new wxButton(&sidebar, wxID_ANY, _L("智能切片：检查与优化…"));
        entry_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show(true); });
        sidebar.GetSizer()->Insert(0, entry_button, 0, wxEXPAND | wxALL, sidebar.FromDIP(8));
        sidebar.Layout();

        aui_manager.AddPane(panel, wxAuiPaneInfo()
                                       .Name("smart_slicing")
                                       .Caption(_L("智能切片"))
                                       .Left()
                                       .CaptionVisible(false)
                                       .PaneBorder(false)
                                       .Movable(false)
                                       .Floatable(false)
                                       .CloseButton(true)
                                       .TopDockable(false)
                                       .BottomDockable(false)
                                       .BestSize(plater.FromDIP(wxSize(380, 520)))
                                       .MinSize(plater.FromDIP(wxSize(320, 200)))
                                       .Hide());
        aui_manager.Update();
        plater.Bind(wxEVT_AUI_PANE_CLOSE, &Impl::on_pane_close, this);
        selection_canvas = plater.get_view3D_canvas3D()->get_wxglcanvas();
        selection_sidebar = &sidebar;
        selection_canvas->Bind(EVT_GLCANVAS_OBJECT_SELECT, &Impl::on_selection_changed, this);
        selection_sidebar->Bind(EVT_OBJ_LIST_OBJECT_SELECT, &Impl::on_selection_changed, this);
    }

    void on_selection_changed(SimpleEvent& event)
    {
        // Native Selection is already updated when these notifications arrive.
        // Refresh only the existing preparation controls; retain Orca's handler.
        if (!wxGetApp().is_closing() && panel != nullptr)
            panel->refresh_preparation_selection();
        event.Skip();
    }

    void on_pane_close(wxAuiManagerEvent& event)
    {
        if (event.GetPane() && event.GetPane()->window == panel) {
            for (auto* canvas : {plater.get_view3D_canvas3D(), plater.get_preview_canvas3D()})
                if (canvas) canvas->set_workspace_background(std::nullopt);
        }
        if (event.GetPane() && event.GetPane()->window == panel && restore_sidebar) {
            plater.collapse_sidebar(false);
            restore_sidebar = false;
        }
        event.Skip();
    }

    void start_candidate_search_session()
    {
        using namespace AI::SmartSlicing;
        const WorkflowSnapshot& legacy_snapshot = coordinator->snapshot();
        if (!legacy_snapshot.context || legacy_snapshot.workflow_id == 0)
            return;

        supersede_active_candidate_search_session(recommendation_coordinator);
        join_recommendation_worker();
        clear_recommendation_results();
        clear_recommendation_projection();
        if (recommendation_revision_timer)
            recommendation_revision_timer->Stop();

        candidate_capture = workspace->capture_candidate_search_input();
        if (!candidate_capture.completed()) {
            candidate_search.reset();
            candidate_session_plan.reset();
            candidate_session_diagnostic = candidate_capture.diagnostic_code;
            clear_recommendation_projection();
            return;
        }
        candidate_search = CandidateSearchPipeline().search(*candidate_capture.input);
        CandidateSearchSessionPlanResult planned = CandidateSearchSessionPlanner().plan(
            *candidate_search, legacy_snapshot.workflow_id, ++last_recommendation_attempt);
        if (!planned.accepted()) {
            candidate_session_plan.reset();
            candidate_session_diagnostic = std::move(planned.diagnostic_code);
            clear_recommendation_projection();
            return;
        }
        candidate_session_diagnostic.clear();
        candidate_session_plan = std::move(*planned.plan);
        OrcaTrialSliceInput trial_input;
        try {
            trial_input = workspace->capture_trial_slice_input();
        } catch (...) {
            candidate_session_plan.reset();
            candidate_session_diagnostic = "trial_input_capture_failed";
            trial_executor->clear_session_input();
            clear_recommendation_projection();
            return;
        }
        for (const CandidateSearchParameterInput& parameter : candidate_capture.input->profile_parameters)
            trial_input.profile_bounds.insert(trial_input.profile_bounds.end(),
                                              parameter.bounds.begin(), parameter.bounds.end());
        trial_executor->prepare_session_input(std::move(trial_input));
        const auto session_started_at = std::chrono::steady_clock::now();
        candidate_session_plan->start_command.requested_at = session_started_at;
        recommendation_coordinator.enqueue(candidate_session_plan->start_command);
        recommendation_coordinator.process_all();
        TrialSliceSchedulerInput scheduler_input;
        scheduler_input.search_result = *candidate_search;
        scheduler_input.session_plan = *candidate_session_plan;
        scheduler_input.usage_purpose = candidate_capture.input->usage_purpose;
        scheduler_input.machine_support_status = MachineSupportStatus::Enabled;
        scheduler_input.cancellation_token = recommendation_coordinator.cancellation_token();
        scheduler_input.deadline = session_started_at + std::chrono::minutes(10);
        recommendation_worker_state.mark_started();
        publish_recommendation_snapshot();
        try {
            recommendation_worker = std::thread([this, scheduler_input = std::move(scheduler_input)]() mutable {
                try {
                    TrialSliceScheduler scheduler;
                    scheduler.run(
                        scheduler_input,
                        [this, executor = trial_executor.get()](
                            const TrialSliceTask& task,
                            const std::shared_ptr<const RecommendationCancellationToken>& token) {
                            auto result = executor->execute_versioned_trial_slice(task, token);
                            if (result.status == TrialSliceStatus::Succeeded && result.metrics) {
                                std::lock_guard<std::mutex> lock(recommendation_result_mutex);
                                trial_metrics[task.identity.candidate_id] = *result.metrics;
                            }
                            return result;
                        },
                        [this](RecommendationTaskResult result) {
                            std::lock_guard<std::mutex> lock(recommendation_result_mutex);
                            recommendation_results.push_back(std::move(result));
                        });
                    recommendation_worker_state.mark_completed();
                } catch (...) {
                    recommendation_worker_state.mark_failed();
                }
            });
        } catch (...) {
            recommendation_worker_state.mark_completed();
            trial_executor->clear_session_input();
            candidate_session_diagnostic = "trial_worker_start_failed";
            recommendation_coordinator.enqueue(RecommendationTaskResult{
                candidate_session_plan->baseline_task, RecommendationTaskOutcome::Failed,
                {candidate_session_diagnostic}});
            for (const RecommendationTaskIdentity& identity : candidate_session_plan->goal_tasks)
                recommendation_coordinator.enqueue(RecommendationTaskResult{
                    identity, RecommendationTaskOutcome::Failed, {candidate_session_diagnostic}});
            recommendation_coordinator.process_all();
            publish_recommendation_snapshot();
            return;
        }
        if (recommendation_revision_timer && !recommendation_revision_timer->IsRunning())
            recommendation_revision_timer->Start(1000);
    }

    void join_recommendation_worker()
    {
        if (recommendation_worker.joinable())
            recommendation_worker.join();
        recommendation_worker_state.mark_completed();
        if (recommendation_worker_state.consume_failure() && candidate_session_plan)
            settle_recommendation_worker_exception(recommendation_coordinator, *candidate_session_plan);
        trial_executor->clear_session_input();
    }

    void clear_recommendation_results()
    {
        std::lock_guard<std::mutex> lock(recommendation_result_mutex);
        recommendation_results.clear();
        trial_metrics.clear();
    }

    void drain_recommendation_results()
    {
        std::deque<AI::SmartSlicing::RecommendationTaskResult> pending;
        {
            std::lock_guard<std::mutex> lock(recommendation_result_mutex);
            pending.swap(recommendation_results);
        }
        for (AI::SmartSlicing::RecommendationTaskResult& result : pending)
            recommendation_coordinator.enqueue(std::move(result));
        if (!pending.empty())
            recommendation_coordinator.process_all();
        if (!recommendation_worker_state.running() &&
            recommendation_worker.joinable())
            join_recommendation_worker();
        publish_recommendation_snapshot();
    }

    void process_recommendation_timer()
    {
        refresh_native_slice_revision();
        if (versioned_apply_workflow->active_transaction()) {
            workbench_official = versioned_apply_workflow->poll();
            publish_workbench();
            return;
        }
        recommendation_coordinator.check_deadline();
        refresh_recommendation_revision();
        drain_recommendation_results();
        const auto& snapshot = recommendation_coordinator.snapshot();
        const bool analyzing = snapshot.recommendation.baseline.status ==
                                   AI::SmartSlicing::GoalResultStatus::Analyzing ||
                               std::any_of(AI::SmartSlicing::RECOMMENDATION_GOALS.begin(),
                                           AI::SmartSlicing::RECOMMENDATION_GOALS.end(),
                                           [&](AI::SmartSlicing::RecommendationGoal goal) {
                                               return snapshot.recommendation.goal_result(goal).status ==
                                                      AI::SmartSlicing::GoalResultStatus::Analyzing;
                                           });
        if (!workbench_active && snapshot.state != AI::SmartSlicing::RecommendationSessionState::Ready &&
            !analyzing && !recommendation_worker_state.running() &&
            recommendation_revision_timer)
            recommendation_revision_timer->Stop();
    }

    void publish_recommendation_snapshot()
    {
        if (presenter)
            presenter->publish_recommendation_snapshot(recommendation_coordinator.snapshot().recommendation);
        publish_workbench();
    }

    void clear_recommendation_projection()
    {
        if (presenter)
            presenter->publish_recommendation_snapshot(AI::SmartSlicing::RecommendationSnapshot{});
        publish_workbench();
    }

    void cancel_recommendation_session(AI::SmartSlicing::RecommendationCancellationReason reason)
    {
        using namespace AI::SmartSlicing;
        const RecommendationSessionSnapshot& snapshot = recommendation_coordinator.snapshot();
        if (snapshot.workflow_id == 0 || snapshot.attempt_id == 0)
            return;
        recommendation_coordinator.enqueue(
            CancelRecommendationSessionCommand{snapshot.workflow_id, snapshot.attempt_id, reason});
        recommendation_coordinator.process_all();
        publish_recommendation_snapshot();
        if (recommendation_revision_timer)
            recommendation_revision_timer->Stop();
    }

    void refresh_recommendation_revision()
    {
        using namespace AI::SmartSlicing;
        const RecommendationSessionSnapshot& snapshot = recommendation_coordinator.snapshot();
        if (snapshot.state != RecommendationSessionState::Recommending &&
            snapshot.state != RecommendationSessionState::Ready) {
            if (!workbench_active && !native_session.pending() && recommendation_revision_timer)
                recommendation_revision_timer->Stop();
            return;
        }
        try {
            const WorkspaceRevision current = workspace->current_revision();
            if (current == snapshot.workspace_revision)
                return;
            recommendation_coordinator.enqueue(WorkspaceRevisionChangedCommand{
                snapshot.workflow_id, snapshot.attempt_id, current});
            recommendation_coordinator.process_all();
            publish_recommendation_snapshot();
            if (!workbench_active && !native_session.pending() && recommendation_revision_timer)
                recommendation_revision_timer->Stop();
        } catch (...) {
        }
    }

    AI::SmartSlicing::WorkspaceRevision current_native_revision() const
    {
        try { return workspace->current_revision(); }
        catch (...) { return {}; }
    }

    void refresh_native_slice_revision()
    {
        if (!native_session.has_result()) return;
        const auto previous = native_session.result().phase;
        const auto current = current_native_revision();
        native_session.refresh(current);
        if (previous != AI::SmartSlicing::OfficialSlicePhase::Failed &&
            native_session.result().phase == AI::SmartSlicing::OfficialSlicePhase::Failed) {
            const auto& original = *native_session.revision();
            BOOST_LOG_TRIVIAL(info) << "[ModelWorkflow] native revision changed model="
                << original.model_revision << ":" << current.model_revision << " config="
                << original.config_revision << ":" << current.config_revision << " plate="
                << original.plate_revision << ":" << current.plate_revision;
        }
        workbench_official = native_session.result();
    }

    ~Impl()
    {
        if (selection_canvas)
            selection_canvas->Unbind(EVT_GLCANVAS_OBJECT_SELECT, &Impl::on_selection_changed, this);
        if (selection_sidebar)
            selection_sidebar->Unbind(EVT_OBJ_LIST_OBJECT_SELECT, &Impl::on_selection_changed, this);
        plater.Unbind(wxEVT_AUI_PANE_CLOSE, &Impl::on_pane_close, this);
        if (recommendation_revision_timer)
            recommendation_revision_timer->Stop();
        recommendation_event_bindings.unbind();
        cancel_recommendation_session(AI::SmartSlicing::RecommendationCancellationReason::Shutdown);
        trial_executor->cancel_trial_slice();
        join_recommendation_worker();
        clear_recommendation_results();
        presenter.reset();
        if (panel != nullptr)
            panel->shutdown_async();
    }

    std::string validate_candidate(const AI::SmartSlicing::SliceCandidate& candidate)
    {
        if (plater.get_view3D_canvas3D()->get_gizmos_manager().is_running())
            return "close_active_model_tool";
        std::vector<TransformTarget> targets;
        std::vector<OrcaObjectParameterPatch> parameter_patch;
        std::string diagnostic;
        if (!collect_transform_targets(plater, candidate, targets, diagnostic) ||
            !prepare_parameter_patch(plater, candidate, parameter_patch, diagnostic))
            return diagnostic;
        return {};
    }

    OrcaApplyMutationResult apply_candidate(const AI::SmartSlicing::SliceCandidate& candidate)
    {
        std::vector<TransformTarget> targets;
        std::vector<OrcaObjectParameterPatch> parameter_patch;
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
        for (size_t i = 0; i < plater.model().objects.size(); ++i)
            for (const auto& patch : parameter_patch)
                if (plater.model().objects[i]->id().id == patch.object_id)
                    changed_object_indices.push_back(i);
        std::sort(changed_object_indices.begin(), changed_object_indices.end());
        changed_object_indices.erase(
            std::unique(changed_object_indices.begin(), changed_object_indices.end()), changed_object_indices.end());

        bool transaction_started = false;
        try {
            plater.select_view_3D("3D");
            {
                Plater::TakeSnapshot transaction(&plater, "Apply Smart Slicing Candidate");
                transaction_started = true;
                for (const TransformTarget& target : changed)
                    target.instance->set_transformation(Geometry::Transformation(target.matrix));
                PartPlate* plate = plater.get_partplate_list().get_curr_plate();
                if (plate == nullptr)
                    throw std::runtime_error("Current plate disappeared while applying a smart-slicing candidate.");
                const auto applied = OrcaParameterProposalAdapter().apply_object_patches(plater.model(), parameter_patch);
                if (!applied.accepted)
                    throw std::runtime_error(applied.diagnostic_code);
                if (!changed_object_indices.empty())
                    plater.changed_objects(changed_object_indices);
                if (!candidate.parameters.entries.empty())
                    plate->update_slice_result_valid_state(false);
                plater.update_title_dirty_status();
            }
            applied_snapshot_time = plater.undo_redo_stack_main().active_snapshot_time();
            return { true, true, {} };
        } catch (...) {
            if (transaction_started && plater.can_undo())
                plater.undo();
            return { false, false, "candidate_apply_rolled_back" };
        }
    }

    OrcaVersionedApplyResult apply_plan(const AI::SmartSlicing::AtomicApplyPlan& plan)
    {
        static constexpr const char* SNAPSHOT_NAME = "Apply Smart Slicing Candidate";
        if (!wxIsMainThread())
            return {false, false, false, false, "versioned_apply_requires_owner_thread", std::nullopt};
        if (plater.inside_snapshot_capture() ||
            plater.get_view3D_canvas3D()->get_gizmos_manager().is_running())
            return {false, false, false, false, "official_transaction_active", std::nullopt};
        if (!plater.undo_redo_stack_main().temp_snapshot_active() ||
            plater.undo_redo_stack_main().has_redo_snapshot())
            return {false, false, false, false, "snapshot_history_not_at_top", std::nullopt};
        AI::SmartSlicing::WorkspaceRevision current_revision;
        try {
            current_revision = workspace->current_revision();
        } catch (...) {
            return {false, false, false, false, "workspace_revision_unavailable", std::nullopt};
        }
        if (current_revision != plan.binding.base_revision)
            return {false, false, false, false, "stale_revision", std::nullopt};

        std::vector<TransformTarget> targets;
        DynamicPrintConfig parameter_patch;
        std::string diagnostic;
        bool config_changed = false;
        if (!collect_transform_targets(plater, plan.candidate, targets, diagnostic) ||
            !prepare_parameter_patch(plater, plan.candidate, parameter_patch, diagnostic, &config_changed))
            return {false, false, false, false, std::move(diagnostic), std::nullopt};

        std::vector<TransformTarget> changed;
        std::vector<size_t> changed_object_indices;
        for (const TransformTarget& target : targets) {
            if (!target.instance->get_matrix().isApprox(target.matrix)) {
                changed.push_back(target);
                changed_object_indices.push_back(target.object_index);
            }
        }
        if (changed.empty() && !config_changed)
            return {false, false, false, false, "apply_plan_has_no_effective_changes", std::nullopt};
        std::sort(changed_object_indices.begin(), changed_object_indices.end());
        changed_object_indices.erase(
            std::unique(changed_object_indices.begin(), changed_object_indices.end()), changed_object_indices.end());

        struct TransformBefore
        {
            uint64_t object_id{0};
            uint64_t instance_id{0};
            Transform3d matrix{Transform3d::Identity()};
        };
        struct ConfigBefore
        {
            std::string key;
            std::unique_ptr<ConfigOption> value;
        };
        std::vector<TransformBefore> transforms_before;
        transforms_before.reserve(changed.size());
        for (const TransformTarget& target : changed) {
            ModelObject* object = plater.model().objects[target.object_index];
            transforms_before.push_back(
                {object->id().id, target.instance->id().id, target.instance->get_matrix()});
        }
        PartPlate* plate_before = plater.get_partplate_list().get_curr_plate();
        if (plate_before == nullptr)
            return {false, false, false, false, "current_plate_unavailable", std::nullopt};
        std::vector<ConfigBefore> config_before;
        config_before.reserve(plan.candidate.parameters.entries.size());
        for (const AI::SmartSlicing::ConfigPatchEntry& entry : plan.candidate.parameters.entries) {
            const ConfigOption* value = plate_before->config()->option(entry.key);
            config_before.push_back({entry.key, value == nullptr ? nullptr :
                                                        std::unique_ptr<ConfigOption>(value->clone())});
        }
        const bool slice_valid_before = plate_before->is_slice_result_valid();
        const bool dirty_before = plater.undo_redo_stack_main().project_modified();
        const size_t active_snapshot_before = plater.undo_redo_stack_main().active_snapshot_time();
        std::optional<UndoRedo::ActionSnapshotIdentity> snapshot_identity;

        try {
            plater.select_view_3D("3D");
            {
                Plater::TakeSnapshot transaction(&plater, SNAPSHOT_NAME);
                UndoRedo::ActionSnapshotIdentity created_snapshot;
                if (!plater.latest_main_snapshot_identity(SNAPSHOT_NAME, created_snapshot))
                    throw std::runtime_error("smart-slicing snapshot identity unavailable");
                snapshot_identity = std::move(created_snapshot);
                if (snapshot_identity->action_snapshot_time != active_snapshot_before)
                    throw std::runtime_error("smart-slicing snapshot identity unavailable");
                for (const TransformTarget& target : changed)
                    target.instance->set_transformation(Geometry::Transformation(target.matrix));
                PartPlate* plate = plater.get_partplate_list().get_curr_plate();
                if (plate == nullptr)
                    throw std::runtime_error("current plate disappeared during smart-slicing apply");
                for (const AI::SmartSlicing::ConfigPatchEntry& entry : plan.candidate.parameters.entries) {
                    const ConfigOption* replacement = parameter_patch.option(entry.key);
                    if (replacement == nullptr)
                        throw std::runtime_error("validated smart-slicing parameter disappeared during apply");
                    plate->config()->set_key_value(entry.key, replacement->clone());
                }
                if (!changed_object_indices.empty())
                    plater.changed_objects(changed_object_indices);
                if (config_changed)
                    plate->update_slice_result_valid_state(false);
                plater.update_title_dirty_status();
            }
            const AI::SmartSlicing::WorkspaceRevision applied_revision = workspace->current_revision();
            if (applied_revision == plan.binding.base_revision)
                throw std::runtime_error("smart-slicing apply did not change workspace revision");
            AI::SmartSlicing::OfficialApplyTransactionIdentity transaction;
            transaction.plan_id = plan.plan_id;
            transaction.candidate_id = plan.candidate.id;
            transaction.applied_revision = applied_revision;
            transaction.workflow_id = plan.binding.workflow_id;
            transaction.attempt_id = plan.binding.attempt_id;
            transaction.snapshot = {
                static_cast<uint64_t>(snapshot_identity->action_snapshot_time),
                static_cast<uint64_t>(snapshot_identity->active_snapshot_time),
                snapshot_identity->action_name};
            return {true, true, false, false, {}, std::move(transaction)};
        } catch (...) {
            if (!snapshot_identity) {
                const bool history_changed =
                    plater.undo_redo_stack_main().active_snapshot_time() != active_snapshot_before;
                return {false, history_changed, history_changed, false,
                        history_changed ? "versioned_apply_snapshot_state_unknown" :
                                          "versioned_apply_snapshot_failed",
                        std::nullopt};
            }

            std::string rollback_diagnostic;
            const bool rollback_jump = plater.rollback_main_snapshot_exact(
                *snapshot_identity, rollback_diagnostic);
            bool restored = rollback_jump;
            PartPlate* restored_plate = plater.get_partplate_list().get_curr_plate();
            if (restored) {
                for (const TransformBefore& before : transforms_before) {
                    const ModelInstance* found = nullptr;
                    for (const ModelObject* object : plater.model().objects) {
                        if (object == nullptr || object->id().id != before.object_id)
                            continue;
                        for (const ModelInstance* instance : object->instances)
                            if (instance != nullptr && instance->id().id == before.instance_id) {
                                found = instance;
                                break;
                            }
                    }
                    if (found == nullptr || !found->get_matrix().isApprox(before.matrix)) {
                        restored = false;
                        rollback_diagnostic = "rollback_transform_verification_failed";
                        break;
                    }
                }
            }
            if (restored && restored_plate != nullptr) {
                for (const ConfigBefore& before : config_before) {
                    const ConfigOption* actual = restored_plate->config()->option(before.key);
                    if ((before.value == nullptr) != (actual == nullptr) ||
                        (before.value != nullptr && actual != nullptr && *before.value != *actual)) {
                        restored = false;
                        rollback_diagnostic = "rollback_config_verification_failed";
                        break;
                    }
                }
            } else if (restored) {
                restored = false;
                rollback_diagnostic = "rollback_plate_verification_failed";
            }
            try {
                if (restored && workspace->current_revision() != plan.binding.base_revision) {
                    restored = false;
                    rollback_diagnostic = "rollback_revision_verification_failed";
                }
            } catch (...) {
                restored = false;
                rollback_diagnostic = "rollback_revision_verification_failed";
            }
            if (restored &&
                (restored_plate->is_slice_result_valid() != slice_valid_before ||
                 plater.undo_redo_stack_main().project_modified() != dirty_before ||
                 plater.undo_redo_stack_main().active_snapshot_time() !=
                     snapshot_identity->action_snapshot_time)) {
                restored = false;
                rollback_diagnostic = "rollback_workspace_state_verification_failed";
            }
            return {false, !restored, true, restored,
                    restored ? "versioned_apply_rolled_back" :
                               (rollback_diagnostic.empty() ? "versioned_apply_rollback_failed" :
                                                              std::move(rollback_diagnostic)),
                    std::nullopt};
        }
    }

    void render(const SmartSlicingViewModel& view)
    {
        refresh_recommendation_revision();
        if (panel != nullptr)
            panel->render(view);
        if (!recommendation_worker_state.running() &&
            (view.is_stale || view.summary_key == "official_slice_complete" || view.summary_key == "canceled" ||
             view.summary_key == "preflight_failed"))
            trial_executor->clear_session_input();
        publish_workbench();
    }

    std::optional<AI::SmartSlicing::SliceCandidate> workbench_candidate(
        AI::SmartSlicing::RecommendationGoal goal) const
    {
        using namespace AI::SmartSlicing;
        if (!candidate_search) return std::nullopt;
        const auto& selected = recommendation_coordinator.snapshot().recommendation.goal_result(goal);
        if (selected.status != GoalResultStatus::Ready || selected.selected_candidate_id.empty()) return std::nullopt;
        for (const auto& draft : candidate_search->goal(goal).selected_for_trial) {
            if (draft.candidate_id != selected.selected_candidate_id) continue;
            SliceCandidate candidate;
            candidate.id = draft.candidate_id;
            candidate.base_revision = candidate_search->baseline.workspace_revision;
            candidate.goal = goal == RecommendationGoal::Speed ? CandidateGoal::Speed :
                goal == RecommendationGoal::Quality ? CandidateGoal::Quality : CandidateGoal::Stability;
            candidate.placement = draft.placement;
            candidate.parameters = draft.parameters;
            candidate.status = CandidateStatus::Ready;
            return candidate;
        }
        return std::nullopt;
    }

    AI::SmartSlicing::ApplyExpectedContext apply_context(const AI::SmartSlicing::SliceCandidate& candidate) const
    {
        using namespace AI::SmartSlicing;
        ApplyExpectedContext context;
        context.session = recommendation_coordinator.snapshot();
        context.goal = selected_goal;
        context.selected_candidate = candidate;
        context.workspace = workspace->capture_context();
        const auto captured = workspace->capture_candidate_search_input();
        if (captured.completed()) {
            context.parameter_validation.goal = selected_goal;
            context.parameter_validation.intent_constraints = captured.input->intent_constraints;
            context.parameter_validation.native_validator = captured.input->native_validator;
            for (const auto& parameter : captured.input->profile_parameters) {
                context.parameter_validation.current_values.push_back({parameter.scope, parameter.owner,
                    parameter.target_id, parameter.key, parameter.current_value});
                context.parameter_validation.bounds.insert(context.parameter_validation.bounds.end(),
                    parameter.bounds.begin(), parameter.bounds.end());
            }
        }
        context.activity.model_tool_active = plater.get_view3D_canvas3D()->get_gizmos_manager().is_running();
        context.activity.official_transaction_active = bool(versioned_apply_workflow->active_transaction());
        return context;
    }

    SmartSlicingWorkbenchState workbench_snapshot() const
    {
        using namespace AI::SmartSlicing;
        SmartSlicingWorkbenchState state;
        state.session = recommendation_coordinator.snapshot();
        if (state.session.state == RecommendationSessionState::Canceled || !candidate_session_plan)
            state.session.recommendation = {};
        state.selected_goal = selected_goal;
        state.official = workbench_official;
        state.diagnostic = candidate_session_diagnostic;
        state.analyzing = recommendation_worker_state.running();
        state.can_analyze = !state.analyzing && !native_session.pending() &&
            workbench_official.phase != OfficialSlicePhase::Slicing;
        state.can_retry = workbench_official.can_retry_slice;
        state.native_execution = native_session.has_result();
        auto* plate = plater.get_partplate_list().get_curr_plate();
        if (native_session.pending() || workbench_official.phase == OfficialSlicePhase::Slicing || plater.is_background_process_slicing())
            state.native_blocked_reason = "正在切片，请等待当前任务完成。";
        else if (!plater.get_ui_job_worker().is_idle() || plater.get_view3D_canvas3D()->get_gizmos_manager().is_running())
            state.native_blocked_reason = "请先完成当前模型处理或编辑操作。";
        else if (plater.printer_technology() != ptFFF || plater.only_gcode_mode() || plater.using_exported_file())
            state.native_blocked_reason = "当前工程模式不支持模型切片。";
        else if (!plate || plate->is_locked() || !plate->has_printable_instances())
            state.native_blocked_reason = "当前打印板没有可切片模型，或打印板已锁定。";
        else if (!plate->can_slice())
            state.native_blocked_reason = "请先处理模型摆放、耗材或打印配置中的错误。";
        else state.can_start_native = true;
        const auto& preflight = coordinator->snapshot();
        state.preflight = preflight.report;
        if (preflight.context) {
            state.machine_reasons = preflight.context->machine_capability.reasons;
            state.material_reasons = preflight.context->material_compatibility.reasons;
        }
        if (preflight.state == WorkflowState::AwaitingRiskDecision && state.preflight) {
            state.can_keep_current_mesh = state.preflight->has_blocking_issue() &&
                std::all_of(state.preflight->issues.begin(), state.preflight->issues.end(), [](const auto& issue) {
                    return !issue.blocks_trial_slice || (issue.code == IssueCode::OpenMesh &&
                        issue.requires_user_decision && std::find(issue.resolution_codes.begin(),
                            issue.resolution_codes.end(), "keep_current_mesh") != issue.resolution_codes.end());
                });
        }
        if (wxGetApp().preset_bundle) {
            const auto& config = wxGetApp().preset_bundle->project_config;
            if (const auto* colors = config.option<ConfigOptionStrings>("filament_colour"))
                state.palette = colors->values;
        }
        std::vector<ConfigPatchEntry> baseline;
        if (candidate_capture.input) for (const auto& parameter : candidate_capture.input->profile_parameters)
            baseline.push_back({parameter.scope, parameter.owner, parameter.target_id, parameter.key,
                parameter.current_value, parameter.current_value, {}});
        for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index) {
            state.candidates[index] = workbench_candidate(RECOMMENDATION_GOALS[index]);
            if (state.candidates[index]) {
                state.effective_parameters[index] = effective_candidate_parameters(
                    baseline, state.candidates[index]->parameters);
                std::lock_guard<std::mutex> lock(recommendation_result_mutex);
                auto found = trial_metrics.find(state.candidates[index]->id);
                if (found != trial_metrics.end()) state.metrics[index] = found->second;
            }
        }
        const auto& candidate = state.candidates[static_cast<size_t>(selected_goal)];
        if (candidate && !native_session.pending() && !versioned_apply_workflow->active_transaction()) {
            try { state.can_start = AI::SmartSlicing::ApplyService().capture_ready_binding(apply_context(*candidate)).accepted(); }
            catch (...) { state.can_start = false; }
        }
        return state;
    }

    void publish_workbench()
    {
        if (workbench_listener) workbench_listener(workbench_snapshot());
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
        if (should_show && !is_shown()) {
            restore_sidebar = !plater.is_sidebar_collapsed();
            if (restore_sidebar) plater.collapse_sidebar(true);
            pane.Left().BestSize(plater.FromDIP(wxSize(380, 520)));
        } else if (!should_show && restore_sidebar) {
            plater.collapse_sidebar(false);
            restore_sidebar = false;
        }
        for (auto* canvas : {plater.get_view3D_canvas3D(), plater.get_preview_canvas3D()}) {
            if (!canvas) continue;
            const auto& color = ModelGenerationInputStyle::background;
            canvas->set_workspace_background(should_show ? std::optional<ColorRGBA>(
                ColorRGBA(color.Red() / 255.f, color.Green() / 255.f, color.Blue() / 255.f, 1.f)) : std::nullopt);
        }
        pane.Show(should_show);
        aui_manager.Update();
    }

    Plater& plater;
    wxAuiManager& aui_manager;
    Sidebar& sidebar;
    std::unique_ptr<OrcaSmartSlicingAdapter> workspace;
    std::unique_ptr<OrcaTrialSliceExecutor> trial_executor;
    std::unique_ptr<OrcaOfficialSliceGateway> official_gateway;
    std::unique_ptr<AI::SmartSlicing::VersionedApplyWorkflow> versioned_apply_workflow;
    std::unique_ptr<AI::SmartSlicing::SmartSlicingCoordinator> coordinator;
    AI::SmartSlicing::RecommendationSessionCoordinator recommendation_coordinator;
    std::unique_ptr<OrcaWorkflowRuntimeStore> runtime_store;
    std::unique_ptr<SmartSlicingPresenter> presenter;
    SmartSlicingPanel* panel { nullptr };
    wxWeakRef<wxWindow> selection_canvas;
    wxWeakRef<wxWindow> selection_sidebar;
    wxButton* entry_button { nullptr };
    bool restore_sidebar { false };
    std::unique_ptr<wxTimer> recommendation_revision_timer;
    RecommendationHostEventBindings recommendation_event_bindings;
    std::thread recommendation_worker;
    AI::SmartSlicing::RecommendationWorkerState recommendation_worker_state;
    mutable std::mutex recommendation_result_mutex;
    std::deque<AI::SmartSlicing::RecommendationTaskResult> recommendation_results;
    std::optional<size_t> applied_snapshot_time;
    OrcaCandidateSearchCaptureResult candidate_capture;
    std::optional<AI::SmartSlicing::CandidateSearchResult> candidate_search;
    std::optional<AI::SmartSlicing::CandidateSearchSessionPlan> candidate_session_plan;
    AI::SmartSlicing::AttemptId last_recommendation_attempt{0};
    std::string candidate_session_diagnostic;
    std::map<std::string, AI::SmartSlicing::TrialMetrics> trial_metrics;
    AI::SmartSlicing::RecommendationGoal selected_goal {AI::SmartSlicing::RecommendationGoal::Balanced};
    AI::SmartSlicing::OfficialSliceResult workbench_official;
    SmartSlicingWorkbenchListener workbench_listener;
    StartOfficialSliceFn native_slice;
    bool workbench_active {false};
    NativeSliceSession native_session;
    uint64_t workbench_command_sequence {0};
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
    BOOST_LOG_TRIVIAL(info) << "[ModelWorkflow] slice completed success=" << success
        << " native_pending=" << m_impl->native_session.pending();
    if (m_impl->versioned_apply_workflow->active_transaction())
        m_impl->workbench_official = m_impl->versioned_apply_workflow->notify_slice_completed(success, failure_code);
    else if (m_impl->native_session.pending()) {
        m_impl->native_session.complete(success, m_impl->current_native_revision(), failure_code);
        m_impl->workbench_official = m_impl->native_session.result();
    }
    else
        m_impl->official_gateway->notify_slice_completed(success, failure_code);
    BOOST_LOG_TRIVIAL(info) << "[ModelWorkflow] completion phase=" << int(m_impl->workbench_official.phase)
        << " diagnostic=" << m_impl->workbench_official.diagnostic_code;
    m_impl->publish_workbench();
}

SmartSlicingWorkbenchState SmartSlicingFeatureHost::workbench_snapshot() const
{
    m_impl->refresh_native_slice_revision();
    return m_impl->workbench_snapshot();
}

void SmartSlicingFeatureHost::set_workbench_listener(SmartSlicingWorkbenchListener listener)
{
    m_impl->workbench_listener = std::move(listener);
    m_impl->publish_workbench();
}

void SmartSlicingFeatureHost::set_workbench_active(bool active)
{
    if (m_impl->workbench_active == active) return;
    m_impl->workbench_active = active;
    if (active) {
        m_impl->show(false);
        m_impl->recommendation_revision_timer->Start(1000);
    } else cancel_workbench_analysis();
}

void SmartSlicingFeatureHost::cancel_workbench_analysis()
{
    m_impl->trial_executor->cancel_trial_slice();
    m_impl->cancel_recommendation_session(AI::SmartSlicing::RecommendationCancellationReason::ModeChanged);
    if (m_impl->workbench_active || m_impl->native_session.pending() ||
        m_impl->versioned_apply_workflow->active_transaction()) m_impl->recommendation_revision_timer->Start(1000);
}

bool SmartSlicingFeatureHost::analyze_workbench()
{
    using namespace AI::SmartSlicing;
    if (!workbench_snapshot().can_analyze) return false;
    if (!m_impl->versioned_apply_workflow->retire_completed_transaction()) return false;
    if (!m_impl->native_session.reset()) return false;
    m_impl->workbench_official = {};
    m_impl->workspace->set_usage_purpose(UsagePurpose::General);
    m_impl->cancel_recommendation_session(RecommendationCancellationReason::ModeChanged);
    m_impl->candidate_session_plan.reset();
    m_impl->candidate_search.reset();
    m_impl->candidate_capture = {};
    m_impl->candidate_session_diagnostic.clear();
    m_impl->coordinator->cancel();
    m_impl->coordinator->start();
    const auto& preflight = m_impl->coordinator->snapshot();
    if (preflight.state != WorkflowState::ReadyForCandidatePlanning) {
        m_impl->candidate_session_diagnostic = preflight.detail;
        m_impl->publish_workbench();
        return false;
    }
    m_impl->start_candidate_search_session();
    return m_impl->candidate_session_plan.has_value();
}

bool SmartSlicingFeatureHost::keep_current_mesh_and_analyze(
    const AI::SmartSlicing::WorkspaceRevision& reviewed_revision)
{
    if (!workbench_snapshot().can_keep_current_mesh ||
        !m_impl->coordinator->keep_current_mesh(reviewed_revision)) {
        m_impl->candidate_session_diagnostic = m_impl->coordinator->snapshot().detail;
        m_impl->publish_workbench();
        return false;
    }
    m_impl->start_candidate_search_session();
    return m_impl->candidate_session_plan.has_value();
}

bool SmartSlicingFeatureHost::select_goal(AI::SmartSlicing::RecommendationGoal goal)
{
    if (m_impl->native_session.pending() || m_impl->workbench_official.phase == AI::SmartSlicing::OfficialSlicePhase::Slicing ||
        m_impl->versioned_apply_workflow->active_transaction()) return false;
    if (std::find(AI::SmartSlicing::RECOMMENDATION_GOALS.begin(), AI::SmartSlicing::RECOMMENDATION_GOALS.end(), goal) ==
        AI::SmartSlicing::RECOMMENDATION_GOALS.end()) return false;
    m_impl->selected_goal = goal;
    m_impl->publish_workbench();
    return true;
}

AI::SmartSlicing::OfficialSliceResult SmartSlicingFeatureHost::start_workbench_slice(
    const SmartSlicingWorkbenchState& reviewed,
    const std::vector<AI::SmartSlicing::RiskConfirmationKind>& confirmations)
{
    using namespace AI::SmartSlicing;
    if (m_impl->versioned_apply_workflow->active_transaction()) {
        const auto current = workbench_snapshot();
        if (!current.can_retry) return {OfficialSlicePhase::Rejected, "slice_retry_not_allowed"};
        m_impl->workbench_official = m_impl->versioned_apply_workflow->retry(
            *m_impl->versioned_apply_workflow->active_transaction());
    } else {
        m_impl->refresh_recommendation_revision();
        const auto current = workbench_snapshot();
        const auto index = static_cast<size_t>(current.selected_goal);
        if (!current.can_start || current.selected_goal != reviewed.selected_goal ||
            current.session.publication_revision != reviewed.session.publication_revision ||
            current.session.workflow_id != reviewed.session.workflow_id ||
            current.session.attempt_id != reviewed.session.attempt_id ||
            current.session.workspace_revision != reviewed.session.workspace_revision ||
            !reviewed.candidates[index] || !current.candidates[index] ||
            apply_candidate_payload_digest(*current.candidates[index]) != apply_candidate_payload_digest(*reviewed.candidates[index]))
            return {OfficialSlicePhase::Rejected, "reviewed_candidate_changed"};
        VersionedApplyRequest request;
        request.command_id = "workbench-" + std::to_string(current.session.workflow_id) + "-" +
            std::to_string(current.session.attempt_id) + "-" + std::to_string(++m_impl->workbench_command_sequence);
        request.confirmation.confirmed_risks = confirmations;
        m_impl->workbench_official = m_impl->versioned_apply_workflow->start(
            request, m_impl->apply_context(*current.candidates[index]));
    }
    m_impl->recommendation_revision_timer->Start(1000);
    m_impl->publish_workbench();
    return m_impl->workbench_official;
}

bool SmartSlicingFeatureHost::start_native_slice()
{
    const auto state = workbench_snapshot();
    if (!state.can_start_native) {
        if (!m_impl->native_session.pending() && state.official.phase != AI::SmartSlicing::OfficialSlicePhase::Slicing) {
            m_impl->workbench_official = {AI::SmartSlicing::OfficialSlicePhase::Rejected, state.native_blocked_reason};
            m_impl->publish_workbench();
        }
        return false;
    }
    cancel_workbench_analysis();
    if (!m_impl->versioned_apply_workflow->retire_completed_transaction()) {
        m_impl->workbench_official = {AI::SmartSlicing::OfficialSlicePhase::Rejected, "请先完成或撤销已应用的 AI 方案。"};
        m_impl->publish_workbench();
        return false;
    }
    const auto revision = m_impl->current_native_revision();
    if (!revision.valid()) {
        m_impl->workbench_official = {AI::SmartSlicing::OfficialSlicePhase::Rejected, "无法读取有效的工程切片状态。"};
        m_impl->publish_workbench();
        return false;
    }
    m_impl->native_session.start(revision);
    bool started = false;
    std::string failure = "official_slice_not_started";
    try { started = m_impl->native_slice && m_impl->native_slice(); }
    catch (const std::exception& error) { failure = error.what(); }
    catch (...) { failure = "原生切片启动出现未知错误。"; }
    if (!started) m_impl->native_session.complete(false, revision, failure);
    m_impl->workbench_official = m_impl->native_session.result();
    m_impl->recommendation_revision_timer->Start(1000);
    m_impl->publish_workbench();
    return started;
}

bool SmartSlicingFeatureHost::undo_workbench_apply()
{
    if (!m_impl->versioned_apply_workflow->active_transaction()) return false;
    m_impl->workbench_official = m_impl->versioned_apply_workflow->undo(
        *m_impl->versioned_apply_workflow->active_transaction());
    m_impl->publish_workbench();
    return m_impl->workbench_official.diagnostic_code == "apply_undone";
}

} // namespace Slic3r::GUI
