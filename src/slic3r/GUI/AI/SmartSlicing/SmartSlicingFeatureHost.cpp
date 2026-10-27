#include "SmartSlicingFeatureHost.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationInputStyle.hpp"

#include "slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.hpp"
#include "slic3r/GUI/AI/Orca/OrcaParameterProposalAdapter.hpp"
#include "slic3r/GUI/AI/Orca/OrcaSmartSlicingAdapter.hpp"
#include "slic3r/GUI/AI/Orca/OrcaTrialSliceExecutor.hpp"
#include "slic3r/GUI/AI/Orca/OrcaWorkflowRuntimeStore.hpp"
#include "slic3r/GUI/AI/Orca/SmartSliceApplier.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingPanel.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingPresenter.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SlicingRecommendationClient.hpp"
#include "slic3r/GUI/AISidecarClient.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/PartPlate.hpp"
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
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingEngine.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/aui/framemanager.h>
#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/weakref.h>
#include <wx/glcanvas.h>
#include <wx/timer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
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

double config_speed_or(const DynamicPrintConfig& config, const char* key, double fallback)
{
    if (const auto* option = config.option<ConfigOptionFloatsNullable>(key);
        option != nullptr && !option->values.empty() && std::isfinite(option->values.front()) &&
        option->values.front() > 0.0)
        return option->values.front();
    return fallback;
}

AI::SmartSlicing::MachinePreset capture_machine_preset(const DynamicPrintConfig& config)
{
    AI::SmartSlicing::MachinePreset machine;
    if (const auto* nozzle = config.option<ConfigOptionFloats>("nozzle_diameter");
        nozzle != nullptr && !nozzle->values.empty())
        machine.nozzle_diameter_mm = nozzle->values.front();
    if (const auto* minimum = config.option<ConfigOptionFloat>("min_layer_height"); minimum != nullptr)
        machine.minimum_layer_height_mm = minimum->value;
    if (const auto* maximum = config.option<ConfigOptionFloat>("max_layer_height"); maximum != nullptr)
        machine.maximum_layer_height_mm = maximum->value;
    if (const auto* speed = config.option<ConfigOptionFloatsNullable>("outer_wall_speed");
        speed != nullptr && !speed->values.empty() && std::isfinite(speed->values.front()) &&
        speed->values.front() > 0.0)
        machine.maximum_outer_wall_speed_mm_s = speed->values.front();
    if (const auto* flow = config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
        flow != nullptr && !flow->values.empty())
        machine.maximum_volumetric_speed_mm3_s = flow->values.front();
    machine.inner_wall_speed_mm_s = config_speed_or(config, "inner_wall_speed", machine.inner_wall_speed_mm_s);
    machine.sparse_infill_speed_mm_s = config_speed_or(config, "sparse_infill_speed", machine.sparse_infill_speed_mm_s);
    machine.internal_solid_infill_speed_mm_s = config_speed_or(
        config, "internal_solid_infill_speed", machine.internal_solid_infill_speed_mm_s);
    machine.top_surface_speed_mm_s = config_speed_or(config, "top_surface_speed", machine.top_surface_speed_mm_s);
    machine.support_speed_mm_s = config_speed_or(config, "support_speed", machine.support_speed_mm_s);
    machine.travel_speed_mm_s = config_speed_or(config, "travel_speed", machine.travel_speed_mm_s);
    machine.default_acceleration_mm_s2 = config_speed_or(
        config, "default_acceleration", machine.default_acceleration_mm_s2);
    machine.outer_wall_acceleration_mm_s2 = config_speed_or(
        config, "outer_wall_acceleration", machine.outer_wall_acceleration_mm_s2);
    if (const auto* layer = config.option<ConfigOptionFloat>("layer_height"); layer != nullptr)
        machine.baseline_layer_height_mm = layer->value;
    if (const auto* walls = config.option<ConfigOptionInt>("wall_loops"); walls != nullptr)
        machine.baseline_wall_loops = walls->value;
    if (const auto* infill = config.option<ConfigOptionPercent>("sparse_infill_density"); infill != nullptr)
        machine.baseline_infill_density_percent = infill->value;
    if (const auto* support = config.option<ConfigOptionBool>("enable_support"); support != nullptr)
        machine.baseline_support_enabled = support->value;
    return machine;
}

AI::SmartSlicing::FilamentPreset capture_filament_preset(const DynamicPrintConfig& config)
{
    AI::SmartSlicing::FilamentPreset filament;
    filament.material_types.clear();
    if (const auto* types = config.option<ConfigOptionStrings>("filament_type"); types != nullptr) {
        static const std::array<std::string, 16> known {
            "PCTG", "PETG", "PEEK", "PEI", "PPS", "PLA", "ABS", "ASA",
            "TPU", "TPE", "PA", "PC", "PVA", "HIPS", "PP", "PET",
        };
        for (std::string raw : types->values) {
            std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char ch) {
                return static_cast<char>(std::toupper(ch));
            });
            const auto found = std::find_if(known.begin(), known.end(), [&](const std::string& type) {
                return raw == type || raw.rfind(type + "-", 0) == 0 || raw.rfind(type + "_", 0) == 0;
            });
            const std::string safe = found == known.end() ? "OTHER" : *found;
            if (std::find(filament.material_types.begin(), filament.material_types.end(), safe) ==
                filament.material_types.end())
                filament.material_types.push_back(safe);
        }
    }
    if (filament.material_types.empty()) filament.material_types.push_back("UNKNOWN");
    if (const auto* density = config.option<ConfigOptionFloats>("filament_density");
        density != nullptr && !density->values.empty())
        filament.density_g_cm3 = density->values.front();
    if (const auto* flow = config.option<ConfigOptionFloats>("filament_max_volumetric_speed");
        flow != nullptr && !flow->values.empty())
        filament.maximum_volumetric_speed_mm3_s = flow->values.front();
    return filament;
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
    struct ExactEstimateResult
    {
        uint64_t attempt{0};
        AI::SmartSlicing::RecommendationGoal goal{AI::SmartSlicing::RecommendationGoal::Balanced};
        AI::SmartSlicing::WorkspaceRevision revision;
        AI::SmartSlicing::TrialSliceResult result;
    };

    struct CloudCompletion
    {
        uint64_t attempt {0};
        std::optional<SlicingRecommendationResult> result;
        std::optional<SlicingRecommendationClient::Error> error;
    };

    struct CloudMailbox
    {
        std::mutex mutex;
        std::optional<CloudCompletion> completion;
    };

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
        cloud_client = std::make_unique<SlicingRecommendationClient>(AISidecarClient::default_endpoint());
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

    void start_lightweight_only_analysis()
    {
        using namespace AI::SmartSlicing;

        // A printability preflight may intentionally block the formal trial
        // candidates (for example, an open mesh), but it must not suppress
        // the independent, read-only geometry recommendations.  Keep this
        // worker separate from the trial worker so entering this path never
        // blocks the UI while the quick metrics are computed.
        if (lightweight_worker.joinable())
            lightweight_worker.join();

        std::vector<TriangleMeshSnapshot> meshes;
        MachinePreset machine;
        FilamentPreset filament;
        if (wxGetApp().preset_bundle != nullptr) {
            DynamicPrintConfig config = wxGetApp().preset_bundle->full_config();
            PartPlate* plate = plater.get_partplate_list().get_curr_plate();
            if (plate != nullptr)
                config.apply(*plate->config(), true);
            machine = capture_machine_preset(config);
            filament = capture_filament_preset(config);
            // A valid native result for the current plate is the most useful
            // calibration anchor available without running three extra full
            // slices.  It is read-only and ignored when the plate is stale.
            if (plate != nullptr && plate->is_slice_result_valid() && plate->get_slice_result() != nullptr) {
                const auto& statistics = plate->get_slice_result()->print_statistics;
                machine.native_baseline_time_seconds = statistics.modes[
                    static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
                double total_volume_mm3 = 0.0;
                for (const auto& [_, volume] : statistics.total_volumes_per_extruder)
                    if (std::isfinite(volume) && volume > 0.0)
                        total_volume_mm3 += volume;
                machine.native_baseline_material_grams = total_volume_mm3 * filament.density_g_cm3 / 1000.0;
            }
            ModelFeatureAnalysisLimits limits;
            limits.maximum_working_memory_bytes = 256ull * 1024ull * 1024ull;
            const auto captured = workspace->capture_model_feature_inputs({}, limits);
            if (captured.completed()) {
                meshes.reserve(captured.inputs.size());
                for (const OrcaModelFeatureInput& input : captured.inputs)
                    meshes.push_back(input.mesh);
            }
        }

        uint64_t attempt = 0;
        cancel_cloud_request();
        {
            std::lock_guard<std::mutex> lock(lightweight_mutex);
            attempt = ++last_lightweight_attempt;
            local_lightweight_analysis.reset();
            lightweight_analysis.reset();
            lightweight_analyzing = !meshes.empty();
            lightweight_stale = false;
            lightweight_diagnostic = meshes.empty() ? "smart_slice_geometry_unavailable" : std::string{};
            cloud_request_seed.reset();
            cloud_start_pending = false;
            if (recommendation_source == SlicingRecommendationSource::Cloud)
                cloud_view.status = CloudRecommendationStatus::Preparing;
        }
        if (meshes.empty()) {
            if (recommendation_source == SlicingRecommendationSource::Cloud) {
                cloud_view.status = CloudRecommendationStatus::Failed;
                cloud_view.diagnostic = "smart_slice_geometry_unavailable";
            }
            publish_workbench();
            return;
        }

        const auto revision = current_native_revision();
        const size_t object_count = meshes.size();
        try {
            lightweight_worker = std::thread([this, meshes = std::move(meshes), machine, filament,
                                               revision, object_count, attempt]() mutable {
                try {
                    const SmartAnalysisResult analysis =
                        SmartSlicingEngine().AnalyzeAndPrecompute(meshes, machine, filament);
                    std::lock_guard<std::mutex> lock(lightweight_mutex);
                    if (attempt == last_lightweight_attempt) {
                        local_lightweight_analysis = analysis;
                        lightweight_analysis = analysis;
                        lightweight_analyzing = false;
                        lightweight_diagnostic = analysis.completed ? std::string{} : analysis.diagnostic_code;
                        if (analysis.completed) {
                            cloud_request_seed = SlicingRecommendationRequest{
                                {}, object_count, analysis.metrics, machine, filament, analysis};
                            cloud_seed_revision = revision;
                            cloud_start_pending = recommendation_source == SlicingRecommendationSource::Cloud;
                            if (cloud_start_pending) cloud_view.status = CloudRecommendationStatus::Preparing;
                        } else if (recommendation_source == SlicingRecommendationSource::Cloud) {
                            cloud_view.status = CloudRecommendationStatus::Failed;
                            cloud_view.diagnostic = analysis.diagnostic_code;
                        }
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(lightweight_mutex);
                    if (attempt == last_lightweight_attempt) {
                        lightweight_analyzing = false;
                        lightweight_diagnostic = "smart_slice_analysis_failed";
                        if (recommendation_source == SlicingRecommendationSource::Cloud) {
                            cloud_view.status = CloudRecommendationStatus::Failed;
                            cloud_view.diagnostic = lightweight_diagnostic;
                        }
                    }
                }
            });
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(lightweight_mutex);
                if (attempt == last_lightweight_attempt) {
                    lightweight_analyzing = false;
                    lightweight_diagnostic = "smart_slice_analysis_worker_start_failed";
                    if (recommendation_source == SlicingRecommendationSource::Cloud) {
                        cloud_view.status = CloudRecommendationStatus::Failed;
                        cloud_view.diagnostic = lightweight_diagnostic;
                    }
                }
            }
            publish_workbench();
        }
    }

    static AI::SmartSlicing::SliceMode slice_mode_for_goal(AI::SmartSlicing::RecommendationGoal goal)
    {
        return goal == AI::SmartSlicing::RecommendationGoal::Speed ? AI::SmartSlicing::SliceMode::SPEED :
            goal == AI::SmartSlicing::RecommendationGoal::Quality ? AI::SmartSlicing::SliceMode::QUALITY :
            AI::SmartSlicing::SliceMode::BALANCED;
    }

    bool has_current_official_statistics(std::optional<AI::SmartSlicing::RecommendationGoal> goal) const
    {
        const auto current = current_native_revision();
        auto* plate = plater.get_partplate_list().get_curr_plate();
        if (workbench_official.phase != AI::SmartSlicing::OfficialSlicePhase::Completed ||
            plate == nullptr || !plate->is_slice_result_valid() || plate->get_slice_result() == nullptr)
            return false;
        const bool matches_revision = native_session.completed_for(current) ||
            (workbench_official.apply_transaction &&
             workbench_official.apply_transaction->applied_revision == current);
        if (!matches_revision || (goal && slice_goal_history.last_sliced_goal(current) != goal))
            return false;
        if (goal && native_session.completed_for(current)) {
            std::lock_guard<std::mutex> lock(lightweight_mutex);
            if (!sliced_recommendation || !lightweight_analysis || !lightweight_analysis->completed ||
                (recommendation_source == SlicingRecommendationSource::Cloud &&
                 cloud_view.status != CloudRecommendationStatus::Ready))
                return false;
            const auto* plan = lightweight_analysis->find(slice_mode_for_goal(*goal));
            return plan && same_slice_parameters(*plan, *sliced_recommendation);
        }
        return true;
    }

    void cancel_exact_estimate()
    {
        trial_executor->cancel_trial_slice();
        std::lock_guard<std::mutex> lock(exact_estimate_mutex);
        ++last_exact_estimate_attempt;
        exact_estimate.status = ExactEstimateStatus::None;
        exact_estimate.estimated_time_seconds.reset();
        exact_estimate.material_volume_mm3.reset();
        exact_estimate.material_grams.reset();
        exact_estimate.tool_changes.reset();
        exact_estimate.source.clear();
        exact_estimate.diagnostic = "smart_slice_preview_canceled";
        exact_estimate_pending.reset();
    }

    void start_exact_estimate(AI::SmartSlicing::RecommendationGoal goal)
    {
        bool lightweight_running = false;
        bool cloud_blocking = false;
        {
            std::lock_guard<std::mutex> lock(lightweight_mutex);
            lightweight_running = lightweight_analyzing;
            cloud_blocking = recommendation_source == SlicingRecommendationSource::Cloud &&
                cloud_view.status != CloudRecommendationStatus::Ready;
        }
        if (!workbench_active || !workbench_ai_mode || recommendation_worker_state.running() ||
            lightweight_running || cloud_blocking || native_session.pending() ||
            plater.is_background_process_slicing() || has_current_official_statistics(goal) ||
            workbench_official.phase == AI::SmartSlicing::OfficialSlicePhase::Slicing)
            return;

        {
            std::lock_guard<std::mutex> lock(exact_estimate_mutex);
            if (exact_estimate.status == ExactEstimateStatus::Running && exact_estimate.goal == goal &&
                exact_estimate.revision == current_native_revision())
                return;
        }
        if (exact_worker.joinable())
            return;

        AI::SmartSlicing::SliceRecommendation recommendation;
        {
            std::lock_guard<std::mutex> lock(lightweight_mutex);
            if (!lightweight_analysis || !lightweight_analysis->completed)
                return;
            const auto* found = lightweight_analysis->find(slice_mode_for_goal(goal));
            if (found == nullptr)
                return;
            recommendation = *found;
        }

        const auto revision = current_native_revision();
        if (!revision.valid()) return;
        const size_t cache_index = static_cast<size_t>(goal) +
            (recommendation_source == SlicingRecommendationSource::Cloud ? 3 : 0);
        {
            std::lock_guard<std::mutex> lock(exact_estimate_mutex);
            const auto& cached = estimate_cache[cache_index];
            if (exact_estimate_matches(cached, revision, goal, recommendation) &&
                (cached.status == ExactEstimateStatus::Ready || cached.status == ExactEstimateStatus::Failed)) {
                exact_estimate = cached;
                return;
            }
            active_estimate_cache_index = cache_index;
            exact_estimate.parameters = recommendation;
        }
        if (!goal_selection.selected(revision)) {
            goal_selection.select(goal, revision);
            selected_goal = goal;
        }
        OrcaTrialSliceInput input;
        try { input = workspace->capture_trial_slice_input(); }
        catch (...) {
            std::lock_guard<std::mutex> lock(exact_estimate_mutex);
            exact_estimate.status = ExactEstimateStatus::Failed;
            exact_estimate.goal = goal;
            exact_estimate.revision = revision;
            exact_estimate.diagnostic = "smart_slice_preview_input_failed";
            return;
        }

        uint64_t attempt;
        {
            std::lock_guard<std::mutex> lock(exact_estimate_mutex);
            attempt = ++last_exact_estimate_attempt;
            exact_estimate = {};
            exact_estimate.status = ExactEstimateStatus::Running;
            exact_estimate.goal = goal;
            exact_estimate.revision = revision;
            exact_estimate.source = "native_trial";
            exact_estimate.parameters = recommendation;
            exact_estimate_pending.reset();
        }
        try {
            BOOST_LOG_TRIVIAL(info) << "[SmartSlicingEstimate] started goal=" << static_cast<int>(goal)
                << " attempt=" << attempt;
            exact_worker = std::thread([this, attempt, goal, revision,
                                        recommendation = std::move(recommendation),
                                        input = std::move(input)]() mutable {
                ExactEstimateResult pending;
                pending.attempt = attempt;
                pending.goal = goal;
                pending.revision = revision;
                pending.result = trial_executor->execute_recommendation_slice(
                    std::move(input), recommendation, revision);
                {
                    std::lock_guard<std::mutex> lock(exact_estimate_mutex);
                    exact_estimate_pending = std::move(pending);
                }
            });
        } catch (...) {
            std::lock_guard<std::mutex> lock(exact_estimate_mutex);
            exact_estimate.status = ExactEstimateStatus::Failed;
            exact_estimate.diagnostic = "smart_slice_preview_worker_start_failed";
        }
    }

    void drain_exact_estimate()
    {
        std::optional<ExactEstimateResult> pending;
        {
            std::lock_guard<std::mutex> lock(exact_estimate_mutex);
            if (exact_estimate_pending) {
                pending = std::move(exact_estimate_pending);
                exact_estimate_pending.reset();
            }
        }
        if (pending) {
            if (exact_worker.joinable()) exact_worker.join();
            std::lock_guard<std::mutex> lock(exact_estimate_mutex);
            if (pending->attempt != last_exact_estimate_attempt ||
                pending->revision != current_native_revision())
                return;
            const auto& result = pending->result;
            exact_estimate.goal = pending->goal;
            exact_estimate.revision = pending->revision;
            if (result.status == AI::SmartSlicing::TrialSliceStatus::Succeeded && result.metrics) {
                exact_estimate.status = ExactEstimateStatus::Ready;
                exact_estimate.estimated_time_seconds = result.metrics->estimated_time_seconds;
                exact_estimate.material_volume_mm3 = result.metrics->filament_volume_mm3;
                exact_estimate.material_grams = result.metrics->material_grams;
                exact_estimate.tool_changes = result.metrics->tool_changes;
                exact_estimate.source = "native_trial";
                exact_estimate.diagnostic.clear();
            } else {
                exact_estimate.status = ExactEstimateStatus::Failed;
                exact_estimate.diagnostic = result.diagnostic_code.empty() ?
                    "smart_slice_preview_failed" : result.diagnostic_code;
            }
            estimate_cache[active_estimate_cache_index] = exact_estimate;
            BOOST_LOG_TRIVIAL(info) << "[SmartSlicingEstimate] completed goal=" << static_cast<int>(pending->goal)
                << " attempt=" << pending->attempt << " status=" << static_cast<int>(exact_estimate.status);
        }
    }

    void drain_lightweight_worker()
    {
        if (lightweight_worker.joinable()) {
            bool running = false;
            {
                std::lock_guard<std::mutex> lock(lightweight_mutex);
                running = lightweight_analyzing;
            }
            if (!running)
                lightweight_worker.join();
        }
        start_cloud_request_if_ready();
    }

    void cancel_cloud_request()
    {
        if (cloud_request_handle) cloud_request_handle->cancel();
        cloud_request_handle.reset();
        cloud_gate.cancel();
        {
            std::lock_guard<std::mutex> lock(cloud_mailbox->mutex);
            cloud_mailbox->completion.reset();
        }
        std::lock_guard<std::mutex> lock(lightweight_mutex);
        cloud_start_pending = false;
    }

    void start_cloud_request_if_ready()
    {
        SlicingRecommendationRequest request;
        uint64_t attempt = 0;
        const auto current = current_native_revision();
        {
            std::lock_guard<std::mutex> lock(lightweight_mutex);
            if (!cloud_start_pending || cloud_request_handle ||
                recommendation_source != SlicingRecommendationSource::Cloud ||
                !cloud_request_seed || !local_lightweight_analysis || !local_lightweight_analysis->completed)
                return;
            cloud_start_pending = false;
            if (!current.valid() || cloud_seed_revision != current) {
                cloud_view.status = CloudRecommendationStatus::Failed;
                cloud_view.diagnostic = "workspace_changed";
                return;
            }
            attempt = cloud_gate.begin(current);
            request = *cloud_request_seed;
            request.request_id = "smart-slicing-" + std::to_string(attempt);
            cloud_view = {};
            cloud_view.status = CloudRecommendationStatus::Requesting;
        }
        BOOST_LOG_TRIVIAL(info) << "[SmartSlicingCloud] request=" << request.request_id << " goals=balanced,speed,quality";
        auto mailbox = cloud_mailbox;
        cloud_request_handle = cloud_client->recommend(
            request,
            [mailbox, attempt](SlicingRecommendationResult result) {
                std::lock_guard<std::mutex> lock(mailbox->mutex);
                if (!mailbox->completion || mailbox->completion->attempt <= attempt)
                    mailbox->completion = CloudCompletion{attempt, std::move(result), std::nullopt};
            },
            [mailbox, attempt](SlicingRecommendationClient::Error error) {
                std::lock_guard<std::mutex> lock(mailbox->mutex);
                if (!mailbox->completion || mailbox->completion->attempt <= attempt)
                    mailbox->completion = CloudCompletion{attempt, std::nullopt, std::move(error)};
            });
    }

    bool validate_cloud_analysis(const AI::SmartSlicing::SmartAnalysisResult& analysis, CloudRecommendationView& failure)
    {
        if (!analysis.completed || wxGetApp().preset_bundle == nullptr) {
            failure.diagnostic = "cloud_recommendation_unavailable";
            return false;
        }
        PartPlate* plate = plater.get_partplate_list().get_curr_plate();
        if (plate == nullptr) {
            failure.diagnostic = "current_plate_unavailable";
            return false;
        }
        DynamicPrintConfig base_config = wxGetApp().preset_bundle->full_config();
        base_config.apply(*plate->config(), true);
        for (const auto& recommendation : analysis.recommendations) {
            DynamicPrintConfig validated;
            if (!SmartSliceApplier::BuildConfig(base_config, recommendation, validated, &failure.diagnostic)) {
                failure.failed_goal = AI::SmartSlicing::slice_mode_id(recommendation.mode);
                if (failure.diagnostic == "smart_slice_infill_pattern_unavailable") {
                    failure.failed_parameter = "sparse_infill_pattern";
                    failure.failed_value = nlohmann::json(recommendation.sparse_infill_pattern.substr(0, 64)).dump();
                } else if (failure.diagnostic == "smart_slice_support_type_unavailable") {
                    failure.failed_parameter = "support_type";
                    failure.failed_value = nlohmann::json(recommendation.support_type.substr(0, 64)).dump();
                }
                return false;
            }
        }
        failure.diagnostic.clear();
        return true;
    }

    void drain_cloud_recommendation()
    {
        std::optional<CloudCompletion> completion;
        {
            std::lock_guard<std::mutex> lock(cloud_mailbox->mutex);
            completion = std::move(cloud_mailbox->completion);
            cloud_mailbox->completion.reset();
        }
        if (!completion) return;
        const auto current = current_native_revision();
        {
            std::lock_guard<std::mutex> lock(lightweight_mutex);
            if (recommendation_source != SlicingRecommendationSource::Cloud ||
                !cloud_gate.accepts(completion->attempt, current)) return;
        }
        // A stale callback must never clear a newer active transport.
        cloud_request_handle.reset();
        CloudRecommendationView view;
        view.status = CloudRecommendationStatus::Failed;
        if (completion->error) {
            view.diagnostic = completion->error->code.empty() ?
                "cloud_recommendation_failed" : completion->error->code;
            view.diagnostic_id = completion->error->diagnostic_id;
            view.failed_goal = completion->error->goal;
            view.failed_parameter = completion->error->parameter;
            view.failed_value = completion->error->value;
        } else {
            if (!completion->result || completion->result->request_id !=
                    "smart-slicing-" + std::to_string(completion->attempt) ||
                !validate_cloud_analysis(completion->result->analysis, view)) {
                if (view.diagnostic.empty()) view.diagnostic = "cloud_recommendation_invalid";
            } else {
                view.status = CloudRecommendationStatus::Ready;
                view.summaries = completion->result->summaries;
                view.warnings = completion->result->warnings;
            }
            if (completion->result) view.diagnostic_id = completion->result->diagnostic_id;
        }
        if (view.status == CloudRecommendationStatus::Failed)
            BOOST_LOG_TRIVIAL(warning) << "[SmartSlicingCloud] rejected request=smart-slicing-" << completion->attempt
                << " diagnostic_id=" << view.diagnostic_id << " code=" << view.diagnostic
                << " goal=" << view.failed_goal << " parameter=" << view.failed_parameter
                << " value=" << view.failed_value;
        else
            BOOST_LOG_TRIVIAL(info) << "[SmartSlicingCloud] accepted request=smart-slicing-" << completion->attempt
                << " diagnostic_id=" << view.diagnostic_id << " goals=balanced,speed,quality";
        std::lock_guard<std::mutex> lock(lightweight_mutex);
        cloud_view = std::move(view);
        if (cloud_view.status == CloudRecommendationStatus::Ready)
            lightweight_analysis = std::move(completion->result->analysis);
        else {
            recommendation_source = SlicingRecommendationSource::Local;
            lightweight_analysis = local_lightweight_analysis;
        }
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
        if (recommendation_revision_timer && !workbench_active)
            recommendation_revision_timer->Stop();

        candidate_capture = workspace->capture_candidate_search_input();
        if (!candidate_capture.completed()) {
            candidate_search.reset();
            candidate_session_plan.reset();
            candidate_session_diagnostic = candidate_capture.diagnostic_code;
            clear_recommendation_projection();
            return;
        }

        // Capture and analyze immutable geometry in a dedicated worker. The
        // formal candidate worker below can therefore proceed concurrently;
        // no ModelObject pointer crosses the thread boundary.
        start_lightweight_only_analysis();
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
        drain_lightweight_worker();
        drain_cloud_recommendation();
        publish_recommendation_snapshot();
    }

    void process_recommendation_timer()
    {
        refresh_native_slice_revision();
        if (versioned_apply_workflow->active_transaction()) {
            workbench_official = versioned_apply_workflow->poll();
            if (workbench_official.phase == AI::SmartSlicing::OfficialSlicePhase::Slicing)
                observed_workbench_revision = current_native_revision();
            else if (observed_workbench_revision && *observed_workbench_revision != current_native_revision() &&
                     !workbench_official.can_retry_slice) {
                goal_selection.clear();
                if (workbench_active && workbench_ai_mode && !recommendation_worker_state.running()) {
                    observed_workbench_revision = current_native_revision(); auto_analyze();
                }
            }
            publish_workbench();
            return;
        }
        recommendation_coordinator.check_deadline();
        refresh_recommendation_revision();
        drain_recommendation_results();
        drain_exact_estimate();
        // Process real input edits before scheduling a preview. Internal plan
        // application updates this baseline before starting the native slice.
        if (workbench_active && workbench_ai_mode && !native_session.pending()) {
            const auto current = current_native_revision();
            if (current.valid() && (!observed_workbench_revision || *observed_workbench_revision != current)) {
                goal_selection.select(selected_goal, current);
                cancel_cloud_request();
                cancel_exact_estimate();
                {
                    std::lock_guard<std::mutex> lock(lightweight_mutex);
                    lightweight_analysis.reset();
                    cloud_view = {};
                }
                estimate_cache = {};
                if (recommendation_worker_state.running()) {
                    trial_executor->cancel_trial_slice();
                    cancel_recommendation_session(AI::SmartSlicing::RecommendationCancellationReason::WorkspaceChanged);
                } else { observed_workbench_revision = current; auto_analyze(); }
            }
        }
        if (workbench_active && workbench_ai_mode && !native_session.pending() &&
            !plater.is_background_process_slicing() &&
            !recommendation_worker_state.running()) {
            bool lightweight_ready = false;
            {
                std::lock_guard<std::mutex> lock(lightweight_mutex);
                lightweight_ready = lightweight_analysis && lightweight_analysis->completed &&
                    !lightweight_analyzing;
            }
            if (lightweight_ready) {
                const auto current = current_native_revision();
                const auto goal = goal_selection.selected(current).value_or(
                    AI::SmartSlicing::RecommendationGoal::Balanced);
                bool should_start = false;
                {
                    std::lock_guard<std::mutex> lock(exact_estimate_mutex);
                    const auto mode = slice_mode_for_goal(goal);
                    std::lock_guard<std::mutex> analysis_lock(lightweight_mutex);
                    const auto* parameters = lightweight_analysis ? lightweight_analysis->find(mode) : nullptr;
                    const bool current_result = parameters && exact_estimate_matches(exact_estimate, current, goal, *parameters);
                    should_start = (!current_result || exact_estimate.status == ExactEstimateStatus::None) && !exact_worker.joinable();
                }
                if (should_start && !has_current_official_statistics(goal))
                    start_exact_estimate(goal);
            }
        }
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
        if (workbench_active) publish_workbench();
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
        cancel_exact_estimate();
        cancel_cloud_request();
        {
            std::lock_guard<std::mutex> lock(lightweight_mutex);
            ++last_lightweight_attempt;
            if (lightweight_analyzing) {
                lightweight_analyzing = false;
                lightweight_stale = true;
                lightweight_diagnostic = "smart_slice_analysis_canceled";
            }
        }
        const RecommendationSessionSnapshot& snapshot = recommendation_coordinator.snapshot();
        if (snapshot.workflow_id == 0 || snapshot.attempt_id == 0)
            return;
        recommendation_coordinator.enqueue(
            CancelRecommendationSessionCommand{snapshot.workflow_id, snapshot.attempt_id, reason});
        recommendation_coordinator.process_all();
        publish_recommendation_snapshot();
        if (recommendation_revision_timer && !workbench_active)
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
            // A model, plate, material, or print-parameter edit invalidates
            // the native preview immediately.  Cancel the worker before the
            // coordinator publishes the new workspace revision so the UI
            // cannot briefly reuse the old exact metrics.
            cancel_exact_estimate();
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
        cancel_cloud_request();
        cancel_recommendation_session(AI::SmartSlicing::RecommendationCancellationReason::Shutdown);
        trial_executor->cancel_trial_slice();
        join_recommendation_worker();
        if (lightweight_worker.joinable())
            lightweight_worker.join();
        if (exact_worker.joinable())
            exact_worker.join();
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

    bool apply_lightweight_plan(const AI::SmartSlicing::SliceRecommendation& plan,
                                std::string& diagnostic)
    {
        if (!wxIsMainThread()) {
            diagnostic = "smart_slice_apply_requires_owner_thread";
            return false;
        }
        if (plater.inside_snapshot_capture() ||
            plater.get_view3D_canvas3D()->get_gizmos_manager().is_running()) {
            diagnostic = "smart_slice_apply_busy";
            return false;
        }
        if (wxGetApp().preset_bundle == nullptr) {
            diagnostic = "current_config_unavailable";
            return false;
        }
        PartPlate* plate = plater.get_partplate_list().get_curr_plate();
        if (plate == nullptr) {
            diagnostic = "current_plate_unavailable";
            return false;
        }

        DynamicPrintConfig base_config = wxGetApp().preset_bundle->full_config();
        base_config.apply(*plate->config(), true);
        DynamicPrintConfig patched_config;
        if (!SmartSliceApplier::BuildConfig(base_config, plan, patched_config, &diagnostic))
            return false;

        static constexpr const char* PLAN_KEYS[] = {
            "layer_height", "wall_loops", "sparse_infill_density", "sparse_infill_pattern",
            "outer_wall_line_width", "outer_wall_speed", "enable_support", "support_threshold_angle",
            "support_type", "wall_generator", "adaptive_layer_height"};
        bool changed = false;
        for (const char* key : PLAN_KEYS) {
            const ConfigOption* before = base_config.option(key);
            const ConfigOption* after = patched_config.option(key);
            if (after == nullptr)
                continue;
            if (before == nullptr || before->type() != after->type() || *before != *after) {
                changed = true;
                break;
            }
        }

        try {
            plater.select_view_3D("3D");
            {
                Plater::TakeSnapshot transaction(&plater, "Apply SmartSlice Recommendation");
                for (const char* key : PLAN_KEYS) {
                    const ConfigOption* replacement = patched_config.option(key);
                    if (replacement != nullptr)
                        plate->config()->set_key_value(key, replacement->clone());
                }

                // Recompute the prime tower after the final AI parameters are
                // installed. The initial plate position may have been chosen
                // before the model height and effective layer height were
                // known, which can make the tower/brim grow beyond the bed.
                // Keep project_config and ModelWipeTower identical so the
                // native Print/G-code path consumes the corrected coordinate.
                PartPlateList& plate_list = plater.get_partplate_list();
                plate_list.set_default_wipe_tower_pos_for_plate(plate->get_index(), false);
                plate_list.sync_wipe_tower_positions_to_model();
                if (changed)
                    plate->update_slice_result_valid_state(false);
                plater.update_title_dirty_status();
            }
        } catch (const std::exception& error) {
            if (plater.can_undo())
                plater.undo();
            diagnostic = error.what();
            return false;
        } catch (...) {
            if (plater.can_undo())
                plater.undo();
            diagnostic = "smart_slice_apply_exception";
            return false;
        }
        diagnostic.clear();
        return true;
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
        const auto current_revision = current_native_revision();
        state.session = recommendation_coordinator.snapshot();
        state.session.workspace_revision = current_revision;
        if (state.session.state == RecommendationSessionState::Canceled || !candidate_session_plan)
            state.session.recommendation = {};
        state.selected_goal = selected_goal;
        state.has_selected_goal = versioned_apply_workflow->active_transaction() ? goal_selection.has_selection() :
            goal_selection.selected(current_revision).has_value();
        state.pending_slice_goal = slice_goal_history.pending_goal();
        state.last_sliced_goal = slice_goal_history.last_sliced_goal(current_revision);
        state.official = workbench_official;
        state.official_statistics_ready = has_current_official_statistics(workbench_ai_mode ?
            std::optional<RecommendationGoal>(selected_goal) : std::nullopt);
        state.diagnostic = candidate_session_diagnostic;
        state.analyzing = recommendation_worker_state.running();
        {
            std::lock_guard<std::mutex> lock(lightweight_mutex);
            state.recommendation_source = recommendation_source;
            state.cloud_recommendation = cloud_view;
            state.lightweight_analysis = lightweight_analysis;
            state.lightweight_analyzing = lightweight_analyzing;
            state.lightweight_stale = lightweight_stale;
            if (state.diagnostic.empty() && !lightweight_diagnostic.empty())
                state.diagnostic = lightweight_diagnostic;
            if (state.diagnostic.empty() && cloud_view.status == CloudRecommendationStatus::Failed)
                state.diagnostic = cloud_view.diagnostic;
        }
        {
            std::lock_guard<std::mutex> lock(exact_estimate_mutex);
            state.exact_estimate = exact_estimate;
        }
        if (native_session.has_result() && native_session.revision() == current_revision &&
            sliced_recommendation && state.lightweight_analysis) {
            if (const auto* plan = state.lightweight_analysis->find(slice_mode_for_goal(selected_goal)))
                state.slice_estimate = slice_estimate.for_slice(current_revision, selected_goal, *plan);
        }
        state.analyzing = state.analyzing || state.lightweight_analyzing ||
            state.cloud_recommendation.status == CloudRecommendationStatus::Preparing ||
            state.cloud_recommendation.status == CloudRecommendationStatus::Requesting;
        state.can_analyze = !workbench_analysis_busy(state) && !native_session.pending() &&
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
        // SmartSlice preflight is diagnostic-only in the three-card flow.  It
        // never exposes an "accept risk" action or blocks native slicing.
        state.can_keep_current_mesh = false;
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
        AI::SmartSlicing::SliceMode selected_mode = AI::SmartSlicing::SliceMode::BALANCED;
        if (selected_goal == AI::SmartSlicing::RecommendationGoal::Speed)
            selected_mode = AI::SmartSlicing::SliceMode::SPEED;
        else if (selected_goal == AI::SmartSlicing::RecommendationGoal::Quality)
            selected_mode = AI::SmartSlicing::SliceMode::QUALITY;
        bool quick_plan_ready = false;
        {
            std::lock_guard<std::mutex> lock(lightweight_mutex);
            quick_plan_ready = state.has_selected_goal && lightweight_analysis.has_value() &&
                lightweight_analysis->completed && lightweight_analysis->find(selected_mode) != nullptr &&
                workbench_recommendation_parameters_ready(state);
        }
        if (quick_plan_ready && !state.analyzing && state.can_start_native && !native_session.pending() &&
            !versioned_apply_workflow->active_transaction()) {
            // A selected lightweight plan is intentionally sufficient to enter
            // Orca's native slicer.  SmartSlice capability/preflight evidence
            // remains diagnostic-only and is never an additional gate here.
            state.can_start = true;
        } else if (state.recommendation_source == SlicingRecommendationSource::Local &&
                   state.has_selected_goal && candidate && !native_session.pending() &&
                   !versioned_apply_workflow->active_transaction()) {
            // Keep the legacy formal-candidate route available for older
            // sessions and future experiments, but it is no longer required
            // for the normal three-card workflow.
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
    std::unique_ptr<SlicingRecommendationClient> cloud_client;
    std::shared_ptr<SlicingRecommendationClient::RequestHandle> cloud_request_handle;
    std::shared_ptr<CloudMailbox> cloud_mailbox {std::make_shared<CloudMailbox>()};
    CloudRecommendationGate cloud_gate;
    SmartSlicingPanel* panel { nullptr };
    wxWeakRef<wxWindow> selection_canvas;
    wxWeakRef<wxWindow> selection_sidebar;
    wxButton* entry_button { nullptr };
    bool restore_sidebar { false };
    std::unique_ptr<wxTimer> recommendation_revision_timer;
    RecommendationHostEventBindings recommendation_event_bindings;
    std::thread recommendation_worker;
    std::thread lightweight_worker;
    std::thread exact_worker;
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
    WorkbenchSliceGoalHistory slice_goal_history;
    SmartSlicingWorkbenchListener workbench_listener;
    StartOfficialSliceFn native_slice;
    bool workbench_active {false};
    bool workbench_ai_mode {true};
    WorkbenchGoalSelection goal_selection;
    std::optional<AI::SmartSlicing::WorkspaceRevision> observed_workbench_revision;
    std::optional<AI::SmartSlicing::SliceRecommendation> sliced_recommendation;
    WorkbenchSliceEstimate slice_estimate;
    std::function<void()> auto_analyze;
    NativeSliceSession native_session;
    uint64_t workbench_command_sequence {0};
    mutable std::mutex lightweight_mutex;
    SlicingRecommendationSource recommendation_source {SlicingRecommendationSource::Local};
    CloudRecommendationView cloud_view;
    std::optional<AI::SmartSlicing::SmartAnalysisResult> local_lightweight_analysis;
    std::optional<AI::SmartSlicing::SmartAnalysisResult> lightweight_analysis;
    std::optional<SlicingRecommendationRequest> cloud_request_seed;
    AI::SmartSlicing::WorkspaceRevision cloud_seed_revision;
    bool cloud_start_pending {false};
    std::string lightweight_diagnostic;
    bool lightweight_analyzing {false};
    bool lightweight_stale {false};
    uint64_t last_lightweight_attempt {0};
    mutable std::mutex exact_estimate_mutex;
    ExactEstimateView exact_estimate;
    std::array<ExactEstimateView, 6> estimate_cache;
    size_t active_estimate_cache_index {0};
    std::optional<ExactEstimateResult> exact_estimate_pending;
    uint64_t last_exact_estimate_attempt{0};
};

SmartSlicingFeatureHost::SmartSlicingFeatureHost(Plater& plater, wxAuiManager& aui_manager, Sidebar& sidebar,
                                                 StartOfficialSliceFn start_official_slice)
    : m_impl(std::make_unique<Impl>(plater, aui_manager, sidebar, std::move(start_official_slice)))
{ m_impl->auto_analyze = [this] { analyze_workbench(); }; }

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
    m_impl->slice_goal_history.complete(
        success && m_impl->workbench_official.phase == AI::SmartSlicing::OfficialSlicePhase::Completed,
        m_impl->current_native_revision());
    if (m_impl->has_current_official_statistics(m_impl->workbench_ai_mode ?
            std::optional<AI::SmartSlicing::RecommendationGoal>(m_impl->selected_goal) : std::nullopt)) {
        // Invalidate late preview callbacks and keep native statistics as the
        // authority. No preview is needed for this completed input/goal.
        m_impl->cancel_exact_estimate();
        BOOST_LOG_TRIVIAL(info) << "[SmartSlicingEstimate] using official statistics goal="
            << static_cast<int>(m_impl->selected_goal);
    }
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

void SmartSlicingFeatureHost::set_workbench_active(bool active, bool ai_mode)
{
    if (m_impl->workbench_active == active && m_impl->workbench_ai_mode == ai_mode) return;
    m_impl->workbench_active = active;
    m_impl->workbench_ai_mode = ai_mode;
    if (m_impl->entry_button) m_impl->entry_button->Show(!active);
    if (active) {
        m_impl->show(false);
        m_impl->recommendation_revision_timer->Start(1000);
        if (ai_mode) analyze_workbench();
        else cancel_workbench_analysis();
    } else cancel_workbench_analysis();
}

void SmartSlicingFeatureHost::cancel_workbench_analysis()
{
    m_impl->trial_executor->cancel_trial_slice();
    m_impl->cancel_recommendation_session(AI::SmartSlicing::RecommendationCancellationReason::ModeChanged);
    m_impl->cancel_cloud_request();
    {
        std::lock_guard<std::mutex> lock(m_impl->exact_estimate_mutex);
        m_impl->exact_estimate.status = ExactEstimateStatus::Failed;
        m_impl->exact_estimate.diagnostic = "smart_slice_preview_canceled";
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->lightweight_mutex);
        if (m_impl->recommendation_source == SlicingRecommendationSource::Cloud &&
            m_impl->cloud_view.status != CloudRecommendationStatus::Ready) {
            m_impl->cloud_view.status = CloudRecommendationStatus::Failed;
            m_impl->cloud_view.diagnostic = "cloud_recommendation_canceled";
        }
    }
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
    m_impl->goal_selection.select(m_impl->selected_goal, m_impl->current_native_revision());
    m_impl->slice_estimate = {};
    m_impl->estimate_cache = {};
    m_impl->observed_workbench_revision = m_impl->current_native_revision();
    m_impl->workspace->set_usage_purpose(UsagePurpose::General);
    m_impl->cancel_recommendation_session(RecommendationCancellationReason::ModeChanged);
    m_impl->candidate_session_plan.reset();
    m_impl->candidate_search.reset();
    m_impl->candidate_capture = {};
    m_impl->candidate_session_diagnostic.clear();
    m_impl->coordinator->cancel();
    m_impl->coordinator->start();
    // The three-card workflow is deliberately independent from the legacy
    // formal candidate search.  Preflight is still captured for diagnostics,
    // but it never blocks the native recommendation path.
    m_impl->candidate_session_diagnostic.clear();
    m_impl->start_lightweight_only_analysis();
    m_impl->publish_workbench();
    return true;
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
    m_impl->candidate_session_diagnostic.clear();
    m_impl->start_lightweight_only_analysis();
    m_impl->publish_workbench();
    return true;
}

bool SmartSlicingFeatureHost::set_recommendation_source(SlicingRecommendationSource source)
{
    const auto before = workbench_snapshot();
    if (workbench_slice_running(m_impl->plater.is_background_process_slicing(), before.official.phase) ||
        m_impl->native_session.pending() || m_impl->versioned_apply_workflow->active_transaction())
        return false;
    if (before.recommendation_source == source) return true;

    m_impl->cancel_exact_estimate();
    m_impl->cancel_cloud_request();
    m_impl->slice_estimate = {};
    bool needs_analysis = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->lightweight_mutex);
        const auto current = m_impl->current_native_revision();
        m_impl->recommendation_source = source;
        m_impl->lightweight_analysis = m_impl->local_lightweight_analysis;
        m_impl->cloud_view = {};
        if (source == SlicingRecommendationSource::Cloud) {
            const auto goal = m_impl->goal_selection.selected(current).value_or(AI::SmartSlicing::RecommendationGoal::Balanced);
            m_impl->selected_goal = goal;
            m_impl->goal_selection.select(goal, current);
            if (m_impl->cloud_request_seed && m_impl->cloud_seed_revision == current) {
                m_impl->cloud_start_pending = true;
            } else {
                needs_analysis = !m_impl->lightweight_analyzing;
                m_impl->cloud_view.status = needs_analysis ? CloudRecommendationStatus::Idle :
                    CloudRecommendationStatus::Preparing;
            }
        }
    }
    if (m_impl->recommendation_revision_timer && !m_impl->recommendation_revision_timer->IsRunning())
        m_impl->recommendation_revision_timer->Start(1000);
    if (needs_analysis)
        return analyze_workbench();
    m_impl->start_cloud_request_if_ready();
    m_impl->publish_workbench();
    return true;
}

bool SmartSlicingFeatureHost::select_goal(AI::SmartSlicing::RecommendationGoal goal)
{
    if (workbench_snapshot().analyzing) return false;
    if (m_impl->native_session.pending() || m_impl->workbench_official.phase == AI::SmartSlicing::OfficialSlicePhase::Slicing ||
        m_impl->versioned_apply_workflow->active_transaction()) return false;
    if (std::find(AI::SmartSlicing::RECOMMENDATION_GOALS.begin(), AI::SmartSlicing::RECOMMENDATION_GOALS.end(), goal) ==
        AI::SmartSlicing::RECOMMENDATION_GOALS.end()) return false;
    m_impl->refresh_recommendation_revision();
    const auto revision = m_impl->current_native_revision();
    const bool quick_available = [&] {
        std::lock_guard<std::mutex> lock(m_impl->lightweight_mutex);
        const auto mode = goal == AI::SmartSlicing::RecommendationGoal::Speed ? AI::SmartSlicing::SliceMode::SPEED :
            goal == AI::SmartSlicing::RecommendationGoal::Quality ? AI::SmartSlicing::SliceMode::QUALITY :
            AI::SmartSlicing::SliceMode::BALANCED;
        return m_impl->lightweight_analysis && m_impl->lightweight_analysis->completed &&
               m_impl->lightweight_analysis->find(mode) != nullptr;
    }();
    if (!m_impl->workbench_candidate(goal) && !quick_available) return false;
    if (m_impl->goal_selection.selected(revision) == goal) return true;
    if (!m_impl->goal_selection.select(goal, revision)) return false;
    m_impl->cancel_exact_estimate();
    m_impl->native_session.reset();
    m_impl->slice_estimate = {};
    m_impl->workbench_official = {};
    m_impl->selected_goal = goal;
    m_impl->publish_workbench();
    m_impl->start_exact_estimate(goal);
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
        if (current.has_selected_goal)
            m_impl->slice_goal_history.begin(current.selected_goal, m_impl->current_native_revision());
        m_impl->workbench_official = m_impl->versioned_apply_workflow->retry(
            *m_impl->versioned_apply_workflow->active_transaction());
        if (m_impl->workbench_official.phase != OfficialSlicePhase::Slicing)
            m_impl->slice_goal_history.cancel();
    } else {
        m_impl->refresh_recommendation_revision();
        const auto current = workbench_snapshot();
        const auto index = static_cast<size_t>(current.selected_goal);
        if (!current.has_selected_goal || !reviewed.has_selected_goal || !current.can_start ||
            current.selected_goal != reviewed.selected_goal ||
            current.session.workspace_revision != reviewed.session.workspace_revision)
            return {OfficialSlicePhase::Rejected, "reviewed_candidate_changed"};

        // Native slicing is a no-op when this exact input already has results.
        // Keep the matching completed result instead of treating that as failure.
        if (current.official_statistics_ready)
            return current.official;

        SliceMode selected_mode = SliceMode::BALANCED;
        if (current.selected_goal == RecommendationGoal::Speed)
            selected_mode = SliceMode::SPEED;
        else if (current.selected_goal == RecommendationGoal::Quality)
            selected_mode = SliceMode::QUALITY;

        std::optional<SliceRecommendation> lightweight_plan;
        {
            std::lock_guard<std::mutex> lock(m_impl->lightweight_mutex);
            if (m_impl->lightweight_analysis && m_impl->lightweight_analysis->completed) {
                if (const SliceRecommendation* plan = m_impl->lightweight_analysis->find(selected_mode))
                    lightweight_plan = *plan;
            }
        }
        if (lightweight_plan) {
            // The selected card is a native-process patch, not a second slice
            // engine.  Apply it atomically, then call the same native route
            // used by the Orca tab.  Machine/material capability preflight is
            // deliberately not consulted here.
            std::string diagnostic;
            if (!m_impl->apply_lightweight_plan(*lightweight_plan, diagnostic))
                return {OfficialSlicePhase::Rejected,
                        diagnostic.empty() ? "smart_slice_plan_apply_failed" : std::move(diagnostic)};
            // Applying the reviewed plan changes the plate config and may
            // reposition the wipe tower. It is not a new recommendation input.
            const auto applied_revision = m_impl->current_native_revision();
            m_impl->slice_estimate.capture(current.exact_estimate, current.session.workspace_revision,
                applied_revision, current.selected_goal, *lightweight_plan);
            m_impl->goal_selection.select(current.selected_goal, applied_revision);
            m_impl->observed_workbench_revision = applied_revision;
            m_impl->sliced_recommendation = *lightweight_plan;
            m_impl->slice_goal_history.begin(current.selected_goal, m_impl->current_native_revision());
            m_impl->workbench_official = {};
            if (!start_native_slice())
                m_impl->slice_goal_history.cancel();
            return m_impl->workbench_official;
        }

        if (!reviewed.candidates[index] || !current.candidates[index] ||
            apply_candidate_payload_digest(*current.candidates[index]) != apply_candidate_payload_digest(*reviewed.candidates[index]))
            return {OfficialSlicePhase::Rejected, "reviewed_candidate_changed"};
        VersionedApplyRequest request;
        request.command_id = "workbench-" + std::to_string(current.session.workflow_id) + "-" +
            std::to_string(current.session.attempt_id) + "-" + std::to_string(++m_impl->workbench_command_sequence);
        request.confirmation.confirmed_risks = confirmations;
        m_impl->slice_goal_history.begin(current.selected_goal, m_impl->current_native_revision());
        m_impl->workbench_official = m_impl->versioned_apply_workflow->start(
            request, m_impl->apply_context(*current.candidates[index]));
        if (m_impl->workbench_official.phase != OfficialSlicePhase::Slicing)
            m_impl->slice_goal_history.cancel();
    }
    m_impl->recommendation_revision_timer->Start(1000);
    if (m_impl->workbench_official.workspace_mutated)
        m_impl->observed_workbench_revision = m_impl->current_native_revision();
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
    if (!m_impl->slice_goal_history.pending_goal())
        m_impl->slice_estimate = {};
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
