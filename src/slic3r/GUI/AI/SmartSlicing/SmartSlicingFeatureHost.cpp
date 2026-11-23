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
#include "libslic3r/Utils.hpp"

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

AI::SmartSlicing::MachinePreset capture_machine_preset(const DynamicPrintConfig& config,
                                                       const std::vector<int>& used_filaments)
{
    AI::SmartSlicing::MachinePreset machine;
    if (const auto* nozzle = config.option<ConfigOptionFloats>("nozzle_diameter");
        nozzle != nullptr && !nozzle->values.empty())
        machine.nozzle_diameter_mm = nozzle->values.front();
    const auto* nozzles = config.option<ConfigOptionFloats>("nozzle_diameter");
    const auto* lows = config.option<ConfigOptionFloats>("min_layer_height");
    const auto* highs = config.option<ConfigOptionFloats>("max_layer_height");
    if (nozzles && lows && highs && !nozzles->values.empty() && !lows->values.empty() && !highs->values.empty()) {
        const auto* map = config.option<ConfigOptionInts>("filament_map");
        machine.minimum_layer_height_mm = 0.;
        machine.maximum_layer_height_mm = 0.8;
        machine.nozzle_diameter_mm = std::numeric_limits<double>::max();
        const auto restrict_to_nozzle = [&](size_t slot) {
            const double diameter = nozzles->get_at(slot);
            const double minimum = lows->get_at(slot);
            const double maximum = highs->get_at(slot);
            machine.nozzle_diameter_mm = std::min(machine.nozzle_diameter_mm, diameter);
            machine.minimum_layer_height_mm = std::max(machine.minimum_layer_height_mm,
                minimum > 0. ? minimum : 0.07);
            machine.maximum_layer_height_mm = std::min(machine.maximum_layer_height_mm,
                maximum > 0. ? maximum : diameter * 0.75);
        };
        if (used_filaments.empty()) restrict_to_nozzle(0);
        for (const int id : used_filaments) {
            const int nozzle = map && !map->values.empty() ? map->get_at(std::max(0, id - 1)) : 1;
            restrict_to_nozzle(size_t(std::max(0, nozzle - 1)));
        }
    }
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
    struct TestedOption
    {
        AI::SmartSlicing::SliceRecommendation recommendation;
        AI::SmartSlicing::TrialSliceResult result;
        OrcaTrialSliceDetails details;
    };

    struct ExactEstimateResult
    {
        uint64_t attempt{0};
        AI::SmartSlicing::RecommendationGoal goal{AI::SmartSlicing::RecommendationGoal::Balanced};
        AI::SmartSlicing::WorkspaceRevision revision;
        TestedOption tested;
        bool orientation{false};
    };

    struct LightweightInputs
    {
        AI::SmartSlicing::WorkspaceRevision process_revision;
        AI::SmartSlicing::ModelMetrics metrics;
        AI::SmartSlicing::MachinePreset machine;
        std::optional<AI::SmartSlicing::MachinePreset> machine_with_support;
        AI::SmartSlicing::FilamentPreset filament;
        size_t object_count{0};
    };

    struct LocalPlateCache
    {
        OrcaSliceInputIdentity input;
        std::optional<LightweightInputs> features;
        std::optional<AI::SmartSlicing::SmartAnalysisResult> analysis;
        std::array<ExactEstimateView, 3> estimates;
        AI::SmartSlicing::RecommendationGoal selected_goal{AI::SmartSlicing::RecommendationGoal::Balanced};
        bool defer_estimate{false};
    };

    struct QuickApply
    {
        UndoRedo::ActionSnapshotIdentity snapshot;
        AI::SmartSlicing::WorkspaceRevision applied_revision;
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
        // On a concurrent edit, let the running worker publish before taking
        // a fresh snapshot. The owner-thread timer consumes the latest input.
        drain_lightweight_worker();
        if (lightweight_worker.joinable()) {
            lightweight_refresh_pending = true;
            return;
        }
        lightweight_refresh_pending = false;
        const auto input = current_slice_input();
        std::vector<TriangleMeshSnapshot> meshes;
        MachinePreset machine;
        std::optional<MachinePreset> machine_with_support;
        FilamentPreset filament;
        if (wxGetApp().preset_bundle != nullptr) {
            PartPlate* plate = plater.get_partplate_list().get_curr_plate();
            auto& bundle = *wxGetApp().preset_bundle;
            plater.get_partplate_list().prepare_workbench_smart_baseline(bundle);
            DynamicPrintConfig config = plate ? plate->smart_slice_baseline(bundle) : bundle.full_config(false);
            const auto used = plate ? plate->get_extruders(true, config, config) : std::vector<int>{};
            machine = capture_machine_preset(config, used);
            filament = capture_filament_preset(config);
            // A valid native result for the current plate is the most useful
            // calibration anchor available without running three extra full
            // slices.  It is read-only and ignored when the plate is stale.
            if (plate != nullptr && plate->is_slice_result_valid() && plate->get_slice_result() != nullptr) {
                const auto& statistics = plate->get_slice_result()->print_statistics;
                machine.native_baseline_time_seconds = statistics.modes[
                    static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
                machine.native_baseline_material_grams = plate->get_slice_result()->material_weight_grams(
                    statistics.total_volumes_per_extruder).value_or(0.);
            }
            if (plate != nullptr && !config.opt_bool("enable_support")) {
                // A recommended support process may activate a nozzle that
                // is idle in the current process. Resolve it using the same
                // per-object, plate, and filament rules as the normal input.
                auto support_config = config;
                support_config.set_key_value("enable_support", new ConfigOptionBool(true));
                const auto used_with_support = plate->get_extruders(true, support_config, support_config);
                if (used_with_support != used) {
                    const auto bounds = capture_machine_preset(config, used_with_support);
                    machine_with_support = machine;
                    machine_with_support->nozzle_diameter_mm = bounds.nozzle_diameter_mm;
                    machine_with_support->minimum_layer_height_mm = bounds.minimum_layer_height_mm;
                    machine_with_support->maximum_layer_height_mm = bounds.maximum_layer_height_mm;
                }
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
            lightweight_inputs.reset();
            lightweight_analyzing = !meshes.empty();
            lightweight_stale = false;
            lightweight_diagnostic = meshes.empty() ? "smart_slice_geometry_unavailable" : std::string{};
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

        const size_t object_count = meshes.size();
        lightweight_worker_done = false;
        try {
            lightweight_worker = std::thread([this, meshes = std::move(meshes), machine, machine_with_support,
                                               filament, input, object_count, attempt]() mutable {
                ScopeGuard finished([this] { lightweight_worker_done = true; });
                try {
                    const SmartSlicingEngine engine;
                    auto metrics = engine.MeasureModel(meshes);
                    std::lock_guard<std::mutex> lock(lightweight_mutex);
                    if (attempt == last_lightweight_attempt) {
                        lightweight_inputs = LightweightInputs{input.process, std::move(metrics),
                            machine, machine_with_support, filament, object_count};
                        lightweight_analyzing = false;
                        cloud_start_pending = recommendation_source == SlicingRecommendationSource::Cloud;
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

    bool ensure_local_goal(AI::SmartSlicing::RecommendationGoal goal)
    {
        using namespace AI::SmartSlicing;
        std::lock_guard<std::mutex> lock(lightweight_mutex);
        if (recommendation_source != SlicingRecommendationSource::Local || !lightweight_inputs ||
            !observed_slice_input || lightweight_inputs->process_revision != observed_slice_input->process)
            return false;
        const auto mode = slice_mode_for_goal(goal);
        if (local_lightweight_analysis && local_lightweight_analysis->find(mode)) return true;
        const auto& inputs = *lightweight_inputs;
        const SmartSlicingEngine engine;
        auto analysis = engine.AnalyzeAndPrecompute(inputs.metrics, inputs.machine, inputs.filament);
        if (!analysis.completed) {
            lightweight_diagnostic = analysis.diagnostic_code;
            return false;
        }
        if (inputs.machine_with_support && std::any_of(analysis.recommendations.begin(), analysis.recommendations.end(),
                [](const SliceRecommendation& plan) { return plan.enable_support; })) {
            const auto with_support = engine.AnalyzeAndPrecompute(inputs.metrics, *inputs.machine_with_support, inputs.filament);
            for (size_t i = 0; i < analysis.recommendations.size(); ++i) {
                auto& plan = analysis.recommendations[i];
                if (!plan.enable_support) continue;
                if (const auto* support_plan = with_support.find(plan.mode)) {
                    plan = *support_plan;
                } else {
                    analysis.recommendation_available[i] = false;
                    analysis.diagnostic_code = with_support.diagnostic_code;
                }
            }
        }
        for (size_t i = 0; i < analysis.recommendations.size(); ++i) {
            auto& plan = analysis.recommendations[i];
            // A reviewed plan for this input can contain accepted native effects.
            if (local_lightweight_analysis) {
                if (const auto* cached = local_lightweight_analysis->find(plan.mode)) {
                    plan = *cached;
                    analysis.recommendation_available[i] = true;
                    continue;
                }
            }
            if (!analysis.recommendation_available[i]) continue;
            plan.wall_loops = std::max(plan.wall_loops, inputs.machine.baseline_wall_loops);
            plan.sparse_infill_density_percent = std::max(plan.sparse_infill_density_percent,
                inputs.machine.baseline_infill_density_percent);
            BOOST_LOG_TRIVIAL(info) << "[SmartSlicingParameters] goal=" << static_cast<int>(RECOMMENDATION_GOALS[i])
                << " plate=" << observed_slice_input->plate_id << " input=" << observed_slice_input->exact.fingerprint
                << " height=" << plan.layer_height_mm;
        }
        const bool selected_ready = analysis.find(mode) != nullptr;
        lightweight_diagnostic = selected_ready ? std::string{} : analysis.diagnostic_code;
        local_lightweight_analysis = std::move(analysis);
        lightweight_analysis = local_lightweight_analysis;
        return selected_ready;
    }

    const ExactEstimateView* current_local_completion(const AI::SmartSlicing::WorkspaceRevision& revision) const
    {
        if (recommendation_source != SlicingRecommendationSource::Local || native_session.pending() ||
            plater.is_background_process_slicing()) return nullptr;
        auto* plate = plater.get_partplate_list().get_curr_plate();
        if (!plate || !plate->is_slice_result_valid() || !plate->get_slice_result()) return nullptr;
        const auto result_id = plate->get_slice_result()->id;
        const auto local_end = estimate_cache.begin() + 3;
        const auto committed = std::find_if(estimate_cache.begin(), local_end, [&](const auto& estimate) {
            return committed_estimate_matches(estimate, revision, result_id);
        });
        return committed != local_end ? &*committed : nullptr;
    }

    bool has_current_official_statistics(std::optional<AI::SmartSlicing::RecommendationGoal> goal) const
    {
        const auto current = current_native_revision();
        if (const auto* committed = current_local_completion(current))
            return !goal || committed->goal == *goal;
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
        {
            std::lock_guard<std::mutex> lock(exact_estimate_mutex);
            ++last_exact_estimate_attempt;
        }
        exact_estimate = {};
        exact_estimate.diagnostic = "smart_slice_preview_canceled";
        orientation_estimate.reset();
        orientation_diagnostic.clear();
        trial_executor->cancel_trial_slice();
        // This mailbox also signals that the worker can be joined. Drain the
        // completion and discard its obsolete attempt instead of losing it.
    }

    // Owner thread: at most one native worker. The previous attempt drains
    // before any newly selected goal or explicitly requested comparison runs.
    void start_exact_estimate(AI::SmartSlicing::RecommendationGoal goal, bool compare_orientation = false)
    {
        using namespace AI::SmartSlicing;
        if (!workbench_active || !workbench_ai_mode || recommendation_worker_state.running() ||
            exact_worker.joinable() || native_session.pending() || plater.is_background_process_slicing() ||
            workbench_official.phase == OfficialSlicePhase::Slicing || !observed_slice_input)
            return;
        if (!compare_orientation && has_current_official_statistics(goal)) return;

        const auto current = current_slice_input();
        if (!current.exact.valid() || current.exact != observed_slice_input->exact) return;
        const bool local = recommendation_source == SlicingRecommendationSource::Local;
        if (local) ensure_local_goal(goal);
        SliceRecommendation recommendation;
        {
            std::lock_guard<std::mutex> lock(lightweight_mutex);
            if (lightweight_analyzing || !lightweight_analysis || !lightweight_analysis->completed ||
                (!local && cloud_view.status != CloudRecommendationStatus::Ready)) return;
            const auto* found = lightweight_analysis->find(slice_mode_for_goal(goal));
            if (!found) return;
            recommendation = *found;
        }
        const size_t cache_index = static_cast<size_t>(goal) + (local ? 0 : 3);
        const auto& cached = estimate_cache[cache_index];
        const auto respects_current_position = [&](const ExactEstimateView& result) {
            return current.automatic_tower || !result.native_details || !result.native_details->tower_position ||
                (*result.native_details->tower_position - current.tower_position).norm() <= 1e-6;
        };
        if (!compare_orientation) {
            if (exact_estimate_matches(exact_estimate, current.exact, goal, recommendation) &&
                exact_estimate.status != ExactEstimateStatus::None && respects_current_position(exact_estimate)) return;
            if (exact_estimate_matches(cached, current.exact, goal, recommendation) &&
                (cached.status == ExactEstimateStatus::Ready || cached.status == ExactEstimateStatus::Failed) &&
                respects_current_position(cached)) {
                exact_estimate = cached;
                BOOST_LOG_TRIVIAL(info) << "[SmartSlicingEstimate] cache goal=" << static_cast<int>(goal)
                    << " plate=" << current.plate_id << " input=" << current.exact.fingerprint;
                return;
            }
            if (local && defer_estimate_after_tower_move) {
                show_moved_tower_estimate(current, goal);
                return;
            }
        } else if (!local || !exact_estimate.native_details || exact_estimate.status != ExactEstimateStatus::Ready ||
                   !exact_estimate_matches(exact_estimate, current.exact, goal, recommendation)) {
            return;
        }

        OrcaTrialSliceInput input;
        try { input = workspace->capture_trial_slice_input(); }
        catch (...) {
            if (compare_orientation) orientation_diagnostic = "smart_slice_preview_input_failed";
            else {
                exact_estimate = {};
                exact_estimate.status = ExactEstimateStatus::Failed;
                exact_estimate.goal = goal;
                exact_estimate.revision = current.exact;
                exact_estimate.parameters = recommendation;
                exact_estimate.diagnostic = "smart_slice_preview_input_failed";
            }
            return;
        }

        if (local && !compare_orientation) {
            const auto same_input = std::find_if(estimate_cache.begin(), estimate_cache.begin() + 3,
                [&](const ExactEstimateView& verified) {
                    return verified.revision == current.exact && verified.status == ExactEstimateStatus::Ready &&
                        verified.parameters && verified.native_details && verified.native_details->placement.transforms.empty() &&
                        respects_current_position(verified) &&
                        OrcaTrialSliceExecutor::same_native_recommendation_input(input, *verified.parameters, recommendation);
                });
            if (same_input != estimate_cache.begin() + 3) {
                const auto original_goal = same_input->goal;
                exact_estimate = *same_input;
                exact_estimate.goal = goal;
                exact_estimate.parameters = recommendation;
                exact_estimate.official_result_id.reset();
                estimate_cache[cache_index] = exact_estimate;
                BOOST_LOG_TRIVIAL(info) << "[SmartSlicingEstimate] reused identical plan goal=" << static_cast<int>(goal)
                    << " from_goal=" << static_cast<int>(original_goal) << " plate=" << current.plate_id
                    << " input=" << current.exact.fingerprint;
                return;
            }
        }

        uint64_t attempt;
        {
            std::lock_guard<std::mutex> lock(exact_estimate_mutex);
            attempt = ++last_exact_estimate_attempt;
        }
        orientation_diagnostic.clear();
        if (compare_orientation) {
            orientation_running = true;
        } else {
            exact_estimate = {};
            exact_estimate.status = ExactEstimateStatus::Running;
            exact_estimate.goal = goal;
            exact_estimate.revision = current.exact;
            exact_estimate.source = "native_trial";
            exact_estimate.parameters = recommendation;
        }
        const auto deadline = std::chrono::steady_clock::now() + WorkflowResourceBudget{}.maximum_elapsed;
        const auto reference = compare_orientation ? std::optional<ExactEstimateView>(exact_estimate) : std::nullopt;
        try {
            exact_worker = std::thread([this, attempt, goal, current, deadline, compare_orientation, reference,
                                        recommendation = std::move(recommendation), input = std::move(input)]() mutable {
                ExactEstimateResult pending;
                pending.attempt = attempt;
                pending.goal = goal;
                pending.revision = current.exact;
                pending.orientation = compare_orientation;
                auto& tested = pending.tested;
                tested.recommendation = recommendation;
                const auto canceled = [&] {
                    std::lock_guard<std::mutex> lock(exact_estimate_mutex);
                    return attempt != last_exact_estimate_attempt;
                };
                const auto run = [&](unsigned ordinal) {
                    const OrcaTrialSliceControl control{deadline, canceled, ordinal == 1};
                    BOOST_LOG_TRIVIAL(info) << "[SmartSlicingTrial] started goal=" << static_cast<int>(goal)
                        << " attempt=" << attempt << " plate=" << current.plate_id << " input=" << current.exact.fingerprint
                        << " run=" << ordinal << " orientation=" << compare_orientation;
                    tested.result = trial_executor->execute_recommendation_slice(input, recommendation,
                        current.exact, tested.details, control);
                    const auto& stats = tested.result.metrics;
                    BOOST_LOG_TRIVIAL(info) << "[SmartSlicingTrial] finished goal=" << static_cast<int>(goal)
                        << " attempt=" << attempt << " plate=" << current.plate_id << " input=" << current.exact.fingerprint
                        << " run=" << ordinal << " status=" << static_cast<int>(tested.result.status)
                        << " height=" << tested.details.effective_layer_height << " layers=" << tested.details.layer_count
                        << " seconds=" << (stats ? stats->estimated_time_seconds.value_or(0.) : 0.)
                        << " grams=" << (stats ? stats->material_grams.value_or(0.) : 0.)
                        << " diagnostic=" << tested.result.diagnostic_code;
                };
                try {
                    OrcaTrialSliceDetails orientation_preview;
                    if (compare_orientation && !OrcaTrialSliceExecutor::prepare_orientation(input, orientation_preview, canceled)) {
                        tested.result.diagnostic_code = "smart_slice_orientation_unavailable";
                    } else if (!canceled()) {
                        run(1);
                        // The one full retry uses a position derived from the generated base.
                        if (input.automatic_tower && tested.details.tower_retry_position && !canceled() &&
                            std::chrono::steady_clock::now() < deadline) {
                            const Vec2d position = *tested.details.tower_retry_position;
                            BOOST_LOG_TRIVIAL(info) << "[SmartSlicingTower] generated footprint relocation goal="
                                << static_cast<int>(goal) << " attempt=" << attempt << " plate=" << current.plate_id
                                << " position=" << position.transpose();
                            const ConfigOptionFloat x(position.x()), y(position.y());
                            input.config.option<ConfigOptionFloats>("wipe_tower_x")->set_at(&x, input.plate_index, 0);
                            input.config.option<ConfigOptionFloats>("wipe_tower_y")->set_at(&y, input.plate_index, 0);
                            tested.details = {};
                            run(2);
                        }
                        if (compare_orientation && tested.result.status == TrialSliceStatus::Succeeded) {
                            const auto grams = tested.result.metrics ? tested.result.metrics->material_grams : std::nullopt;
                            if (!grams || !reference->material_grams || *grams + 0.05 >= *reference->material_grams ||
                                tested.details.support_grams.value_or(0.) > reference->native_details->support_grams.value_or(0.)) {
                                tested.result.status = TrialSliceStatus::Failed;
                                tested.result.diagnostic_code = "smart_slice_orientation_no_benefit";
                            } else {
                                tested.details.placement = std::move(orientation_preview.placement);
                                tested.details.orientation_degrees = orientation_preview.orientation_degrees;
                                tested.details.original_preview = std::move(orientation_preview.original_preview);
                                tested.details.oriented_preview = std::move(orientation_preview.oriented_preview);
                            }
                        }
                    }
                } catch (const std::exception& error) {
                    tested.result.status = TrialSliceStatus::Failed;
                    tested.result.diagnostic_code = "trial_slice_exception";
                    tested.result.diagnostic_message = error.what();
                } catch (...) {
                    tested.result.status = TrialSliceStatus::Failed;
                    tested.result.diagnostic_code = "trial_slice_exception";
                }
                std::lock_guard<std::mutex> lock(exact_estimate_mutex);
                exact_estimate_pending = std::move(pending);
            });
        } catch (...) {
            orientation_running = false;
            if (compare_orientation) orientation_diagnostic = "smart_slice_preview_worker_start_failed";
            else {
                exact_estimate.status = ExactEstimateStatus::Failed;
                exact_estimate.diagnostic = "smart_slice_preview_worker_start_failed";
            }
        }
    }

    void drain_exact_estimate()
    {
        using namespace AI::SmartSlicing;
        std::optional<ExactEstimateResult> pending;
        {
            std::lock_guard<std::mutex> lock(exact_estimate_mutex);
            pending = std::exchange(exact_estimate_pending, std::nullopt);
        }
        if (!pending) return;
        if (exact_worker.joinable()) exact_worker.join();
        orientation_running = false;
        if (pending->attempt != last_exact_estimate_attempt || pending->goal != selected_goal ||
            pending->revision != current_native_revision()) return;

        auto& tested = pending->tested;
        ExactEstimateView view;
        view.goal = pending->goal;
        view.revision = pending->revision;
        view.parameters = tested.recommendation;
        view.source = "native_trial";
        if (tested.result.status == TrialSliceStatus::Succeeded && tested.result.metrics) {
            view.status = ExactEstimateStatus::Ready;
            view.estimated_time_seconds = tested.result.metrics->estimated_time_seconds;
            view.material_volume_mm3 = tested.result.metrics->filament_volume_mm3;
            view.material_grams = tested.result.metrics->material_grams;
            view.tool_changes = tested.result.metrics->tool_changes;
            view.native_details = std::make_shared<OrcaTrialSliceDetails>(std::move(tested.details));
        } else {
            view.status = ExactEstimateStatus::Failed;
            view.diagnostic = tested.result.diagnostic_code;
            view.diagnostic_message = tested.result.diagnostic_message;
        }
        if (pending->orientation) {
            if (view.status == ExactEstimateStatus::Ready) orientation_estimate = std::move(view);
            else orientation_diagnostic = view.diagnostic;
        } else {
            exact_estimate = std::move(view);
            const auto offset = recommendation_source == SlicingRecommendationSource::Cloud ? 3 : 0;
            estimate_cache[static_cast<size_t>(selected_goal) + offset] = exact_estimate;
        }
        BOOST_LOG_TRIVIAL(info) << "[SmartSlicingEstimate] completed goal=" << static_cast<int>(selected_goal)
            << " attempt=" << pending->attempt << " input=" << pending->revision.fingerprint
            << " status=" << static_cast<int>(pending->orientation ? view.status : exact_estimate.status)
            << " orientation=" << pending->orientation
            << " diagnostic=" << (pending->orientation ? orientation_diagnostic : exact_estimate.diagnostic);
    }

    void drain_lightweight_worker()
    {
        if (lightweight_worker.joinable() && lightweight_worker_done)
            lightweight_worker.join();
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
            if (!cloud_start_pending || cloud_request_handle || recommendation_source != SlicingRecommendationSource::Cloud ||
                !lightweight_inputs || !observed_slice_input) return;
            cloud_start_pending = false;
            if (!current.valid() || observed_slice_input->exact != current ||
                lightweight_inputs->process_revision != observed_slice_input->process) {
                cloud_view.status = CloudRecommendationStatus::Failed;
                cloud_view.diagnostic = "workspace_changed";
                return;
            }
            const auto& inputs = *lightweight_inputs;
            auto machine = inputs.machine;
            const AI::SmartSlicing::SmartSlicingEngine engine;
            auto analysis = engine.AnalyzeAndPrecompute(inputs.metrics, machine, inputs.filament);
            if (inputs.machine_with_support && analysis.completed &&
                std::any_of(analysis.recommendations.begin(), analysis.recommendations.end(),
                    [](const auto& candidate) { return candidate.enable_support; })) {
                machine = *inputs.machine_with_support;
                analysis = engine.AnalyzeAndPrecompute(inputs.metrics, machine, inputs.filament);
            }
            if (!analysis.completed) {
                cloud_view.status = CloudRecommendationStatus::Failed;
                cloud_view.diagnostic = analysis.diagnostic_code;
                return;
            }
            attempt = cloud_gate.begin(current);
            request = SlicingRecommendationRequest{{}, inputs.object_count, inputs.metrics, machine, inputs.filament, analysis};
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
        workbench_publication.begin();
        ScopeGuard publish_final_state([this] {
            if (workbench_publication.finish()) publish_workbench();
        });
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
        refresh_workbench_input();
        drain_exact_estimate();
        drain_lightweight_worker();
        if (lightweight_refresh_pending && workbench_active && workbench_ai_mode)
            start_lightweight_only_analysis();
        if (workbench_active && workbench_ai_mode && observed_slice_input && !native_session.pending() &&
            !plater.is_background_process_slicing() && !recommendation_worker_state.running()) {
            if (recommendation_source == SlicingRecommendationSource::Local) ensure_local_goal(selected_goal);
            start_exact_estimate(selected_goal);
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

    OrcaSliceInputIdentity current_slice_input() const
    {
        try { return workspace->current_slice_input(); }
        catch (...) { return {}; }
    }

    void show_moved_tower_estimate(const OrcaSliceInputIdentity& input, AI::SmartSlicing::RecommendationGoal goal)
    {
        using namespace AI::SmartSlicing;
        exact_estimate = {};
        exact_estimate.status = ExactEstimateStatus::PositionChanged;
        exact_estimate.goal = goal;
        exact_estimate.revision = input.exact;
        exact_estimate.moved_tower_position = std::array<double, 2>{input.tower_position.x(), input.tower_position.y()};
        if (!lightweight_analysis) return;
        const auto* plan = lightweight_analysis->find(slice_mode_for_goal(goal));
        if (!plan) return;
        exact_estimate.parameters = *plan;
        const auto& previous = estimate_cache[static_cast<size_t>(goal)];
        if (previous.status != ExactEstimateStatus::Ready || !previous.parameters || !previous.native_details ||
            !same_slice_parameters(*previous.parameters, *plan) || !previous.native_details->placement.transforms.empty()) {
            auto* plate = plater.get_partplate_list().get_curr_plate();
            const auto* print = plate ? plate->fff_print() : nullptr;
            if (print && !plater.is_background_process_slicing()) {
                const auto estimated = print->check_wipe_tower_position(input.tower_position);
                if (estimated == WipeTowerPositionStatus::Outside || estimated == WipeTowerPositionStatus::Exclusion ||
                    estimated == WipeTowerPositionStatus::ModelOverlap)
                    exact_estimate.diagnostic = tower_position_diagnostic(estimated);
                BOOST_LOG_TRIVIAL(info) << "[SmartSlicingEstimate] tower position check goal=" << static_cast<int>(goal)
                    << " plate=" << input.plate_id << " input=" << input.exact.fingerprint
                    << " position=" << input.tower_position.transpose() << " status=" << static_cast<int>(estimated);
            }
            return;
        }

        const auto& effect = *previous.native_details;
        const auto status = effect.tower_geometry ? effect.tower_geometry->check(input.tower_position) :
            (effect.tower_position ? WipeTowerPositionStatus::Unavailable : WipeTowerPositionStatus::NotRequired);
        if (status == WipeTowerPositionStatus::Valid || status == WipeTowerPositionStatus::NotRequired) {
            auto updated = std::make_shared<OrcaTrialSliceDetails>(effect);
            if (updated->tower_position) updated->tower_position = input.tower_position;
            exact_estimate.native_details = std::move(updated);
        } else {
            exact_estimate.diagnostic = tower_position_diagnostic(status);
        }
        BOOST_LOG_TRIVIAL(info) << "[SmartSlicingEstimate] tower position check goal=" << static_cast<int>(goal)
            << " plate=" << input.plate_id << " input=" << input.exact.fingerprint
            << " position=" << input.tower_position.transpose() << " status=" << static_cast<int>(status);
    }

    void refresh_workbench_input()
    {
        using namespace AI::SmartSlicing;
        if (!workbench_active || !workbench_ai_mode || native_session.pending() ||
            workbench_official.phase == OfficialSlicePhase::Slicing) return;
        const auto input = current_slice_input();
        if (!input.exact.valid() || (observed_slice_input && observed_slice_input->exact == input.exact &&
            observed_slice_input->process == input.process)) return;
        const auto previous = std::exchange(observed_slice_input, input);
        cancel_exact_estimate();
        goal_selection.select(selected_goal, input.exact);
        observed_workbench_revision = input.exact;
        const bool local = recommendation_source == SlicingRecommendationSource::Local;
        const bool position_only = previous && previous->process == input.process &&
            previous->tower_position != input.tower_position;
        if (local && position_only) {
            defer_estimate_after_tower_move = !input.automatic_tower || defer_estimate_after_tower_move;
            native_session.reset();
            workbench_official = {};
            slice_estimate = {};
            if (defer_estimate_after_tower_move) show_moved_tower_estimate(input, selected_goal);
            return;
        }

        cancel_recommendation_session(RecommendationCancellationReason::WorkspaceChanged);
        native_session.reset();
        workbench_official = {};
        slice_estimate = {};
        bool cached = false;
        if (!previous || previous->plate_id != input.plate_id) {
            std::lock_guard<std::mutex> lock(lightweight_mutex);
            if (previous && local_lightweight_analysis) {
                // An applied process can keep its verified plan after its
                // earlier feature-based recommendation inputs become stale.
                const auto reusable_features = lightweight_inputs && lightweight_inputs->process_revision == previous->process ?
                    lightweight_inputs : std::nullopt;
                LocalPlateCache old{*previous, reusable_features, local_lightweight_analysis, {},
                    selected_goal, defer_estimate_after_tower_move};
                std::copy_n(estimate_cache.begin(), old.estimates.size(), old.estimates.begin());
                local_plate_cache[previous->plate_id] = std::move(old);
                // Removed plates may still be recovered by Undo. Keep the cache bounded.
                while (local_plate_cache.size() > PartPlateList::MAX_PLATES_COUNT)
                    local_plate_cache.erase(local_plate_cache.begin());
            }
            estimate_cache = {};
            const auto* plate = plater.get_partplate_list().get_curr_plate();
            const int saved_goal = plate ? plate->workbench_smart_goal() : -1;
            selected_goal = saved_goal >= 0 && saved_goal < 3 ? RECOMMENDATION_GOALS[saved_goal] : RecommendationGoal::Balanced;
            const auto found = local_plate_cache.find(input.plate_id);
            if (local && found != local_plate_cache.end() && found->second.input.process == input.process) {
                auto entry = std::move(found->second);
                local_plate_cache.erase(found);
                lightweight_inputs = std::move(entry.features);
                local_lightweight_analysis = std::move(entry.analysis);
                lightweight_analysis = local_lightweight_analysis;
                selected_goal = entry.selected_goal;
                std::move(entry.estimates.begin(), entry.estimates.end(), estimate_cache.begin());
                defer_estimate_after_tower_move = entry.defer_estimate ||
                    (entry.input.exact != input.exact && !input.automatic_tower);
                lightweight_diagnostic.clear();
                lightweight_stale = false;
                lightweight_refresh_pending = false;
                cached = true;
            }
            goal_selection.select(selected_goal, input.exact);
        }
        if (cached) return;
        estimate_cache = {};
        defer_estimate_after_tower_move = false;
        {
            std::lock_guard<std::mutex> lock(lightweight_mutex);
            lightweight_analysis.reset();
            local_lightweight_analysis.reset();
            lightweight_inputs.reset();
            cloud_view = {};
        }
        start_lightweight_only_analysis();
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
        cancel_exact_estimate();
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

    bool apply_lightweight_plan(const ExactEstimateView& evidence, std::string& diagnostic)
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

        if (!exact_estimate_can_apply(evidence)) {
            diagnostic = "smart_slice_preview_required";
            return false;
        }
        const auto& effect = *evidence.native_details;
        const auto config = plate->effective_slice_config(*wxGetApp().preset_bundle);
        const int plate_index = plate->get_index();
        const Vec2d saved(config.opt_float("wipe_tower_x", plate_index),
                          config.opt_float("wipe_tower_y", plate_index));
        const bool tower_changed = effect.tower_position && (*effect.tower_position - saved).norm() > 1e-6;
        if (tower_changed && !plate->automatic_wipe_tower_position()) {
            diagnostic = "reviewed_candidate_changed";
            return false;
        }
        const bool commit_tower = effect.tower_position && plate->automatic_wipe_tower_position();
        std::vector<TransformTarget> targets;
        if (!effect.placement.transforms.empty()) {
            if (workspace->current_plate_has_user_marked_surface()) {
                diagnostic = "smart_slice_orientation_surface_protected";
                return false;
            }
            AI::SmartSlicing::SliceCandidate candidate;
            candidate.placement = effect.placement;
            if (!collect_transform_targets(plater, candidate, targets, diagnostic)) return false;
        }
        static constexpr auto action = "Apply SmartSlice Recommendation";
        std::optional<UndoRedo::ActionSnapshotIdentity> snapshot;
        try {
            plater.select_view_3D("3D");
            {
                Plater::TakeSnapshot transaction(&plater, action);
                UndoRedo::ActionSnapshotIdentity identity;
                if (!plater.latest_main_snapshot_identity(action, identity))
                    throw std::runtime_error("smart_slice_snapshot_unavailable");
                snapshot = identity;
                plate->set_workbench_smart_process(static_cast<int>(evidence.goal), effect.plate_patch, effect.layer_profiles);
                std::vector<size_t> changed_objects;
                for (const auto& target : targets) {
                    target.instance->set_transformation(Geometry::Transformation(target.matrix));
                    plater.model().objects[target.object_index]->invalidate_bounding_box();
                    changed_objects.push_back(target.object_index);
                }
                if (commit_tower)
                    plater.get_partplate_list().set_wipe_tower_position_for_plate(plate_index, *effect.tower_position, true);
                if (!changed_objects.empty()) plater.changed_objects(changed_objects);
                plate->update_slice_result_valid_state(false);
                plater.update_title_dirty_status();
            }
            quick_apply = QuickApply{*snapshot, current_native_revision()};
            return true;
        } catch (const std::exception& error) {
            diagnostic = error.what();
        } catch (...) {
            diagnostic = "smart_slice_apply_exception";
        }
        if (snapshot) {
            std::string reason;
            if (!plater.rollback_main_snapshot_exact(*snapshot, reason))
                diagnostic += " (" + reason + ")";
        }
        return false;
    }

    void finish_lightweight_apply(bool success)
    {
        auto transaction = std::exchange(quick_apply, std::nullopt);
        auto reviewed = std::exchange(official_apply_estimate, std::nullopt);
        if (success || (!transaction && !reviewed)) return;
        std::string reason = "workspace_changed";
        if (transaction && (current_native_revision() != transaction->applied_revision ||
            !plater.main_snapshot_identity_is_current(transaction->snapshot) ||
            !plater.rollback_main_snapshot_exact(transaction->snapshot, reason))) {
            BOOST_LOG_TRIVIAL(warning) << "[SmartSlicing] rollback skipped: " << reason;
            workbench_official.diagnostic_code = "smart_slice_rollback_unavailable";
            return;
        }
        const auto restored = current_slice_input();
        observed_workbench_revision = restored.exact;
        observed_slice_input = restored;
        goal_selection.select(selected_goal, restored.exact);
        slice_estimate = {};
        sliced_recommendation.reset();
        // Native startup cancels recommendation workers and clears their view.
        // Restore only the proof the user reviewed for the recovered workspace.
        if (reviewed && reviewed->revision == restored.exact && reviewed->goal == selected_goal) {
            exact_estimate = std::move(*reviewed);
            BOOST_LOG_TRIVIAL(info) << "[SmartSlicingEstimate] restored after official attempt goal="
                << static_cast<int>(selected_goal) << " input=" << restored.exact.fingerprint
                << " tower=" << restored.tower_position.transpose();
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
        const auto current_revision = current_native_revision();
        state.session = recommendation_coordinator.snapshot();
        state.session.workspace_revision = current_revision;
        if (state.session.state == RecommendationSessionState::Canceled || !candidate_session_plan)
            state.session.recommendation = {};
        state.selected_goal = selected_goal;
        state.has_selected_goal = versioned_apply_workflow->active_transaction() ? goal_selection.has_selection() :
            goal_selection.selected(current_revision).has_value();
        state.pending_slice_goal = slice_goal_history.pending_goal();
        const auto* committed = current_local_completion(current_revision);
        state.last_sliced_goal = committed ? std::optional<RecommendationGoal>(committed->goal) :
            slice_goal_history.last_sliced_goal(current_revision);
        state.official = workbench_official;
        state.official_statistics_ready = has_current_official_statistics(workbench_ai_mode ?
            std::optional<RecommendationGoal>(selected_goal) : std::nullopt);
        if (committed && state.official_statistics_ready)
            state.official = {OfficialSlicePhase::Completed, {}};
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
            if (exact_estimate.revision == current_revision && exact_estimate.goal == selected_goal)
                state.exact_estimate = exact_estimate;
            state.orientation_running = orientation_running;
            state.orientation_diagnostic = orientation_diagnostic;
            if (orientation_estimate && !workspace->current_plate_has_user_marked_surface() &&
                orientation_estimate->revision == current_revision &&
                state.recommendation_source == SlicingRecommendationSource::Local)
                state.orientation_estimate = orientation_estimate;
        }
        if (committed) {
            if (state.lightweight_analysis && state.lightweight_analysis->completed) {
                const size_t index = static_cast<size_t>(committed->goal);
                state.lightweight_analysis->recommendations[index] = *committed->parameters;
                state.lightweight_analysis->recommendation_available[index] = true;
            }
            if (state.official_statistics_ready && selected_goal == committed->goal)
                state.slice_estimate = *committed;
        } else if (native_session.has_result() && native_session.revision() == current_revision &&
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
        else {
            state.native_workspace_available = true;
            state.can_start_native = plate->can_slice();
            if (!state.can_start_native)
                state.native_blocked_reason = "请先处理模型摆放、耗材或打印配置中的错误。";
        }
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
        if (quick_plan_ready && !state.analyzing && !state.orientation_running && state.native_workspace_available && !native_session.pending() &&
            !versioned_apply_workflow->active_transaction()) {
            const auto* plan = state.lightweight_analysis->find(selected_mode);
            state.can_start = state.official_statistics_ready || (plan &&
                exact_estimate_can_apply(state.exact_estimate) &&
                exact_estimate_matches(state.exact_estimate, current_revision, selected_goal, *plan));
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
        if (workbench_publication.request() && workbench_listener)
            workbench_listener(workbench_snapshot());
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
    bool lightweight_refresh_pending{false};
    std::thread exact_worker;
    std::optional<QuickApply> quick_apply;
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
    WorkbenchPublicationBatch workbench_publication;
    StartOfficialSliceFn native_slice;
    bool workbench_active {false};
    bool custom_slice_job{false};
    bool custom_tower_retried{false};
    bool workbench_ai_mode {true};
    WorkbenchGoalSelection goal_selection;
    std::optional<AI::SmartSlicing::WorkspaceRevision> observed_workbench_revision;
    std::optional<AI::SmartSlicing::SliceRecommendation> sliced_recommendation;
    WorkbenchSliceEstimate slice_estimate;
    std::optional<ExactEstimateView> official_apply_estimate;
    std::function<void()> auto_analyze;
    NativeSliceSession native_session;
    uint64_t workbench_command_sequence {0};
    mutable std::mutex lightweight_mutex;
    SlicingRecommendationSource recommendation_source {SlicingRecommendationSource::Local};
    CloudRecommendationView cloud_view;
    std::optional<AI::SmartSlicing::SmartAnalysisResult> local_lightweight_analysis;
    std::optional<AI::SmartSlicing::SmartAnalysisResult> lightweight_analysis;
    std::optional<LightweightInputs> lightweight_inputs;
    std::atomic<bool> lightweight_worker_done{false};
    std::optional<OrcaSliceInputIdentity> observed_slice_input;
    std::map<uint64_t, LocalPlateCache> local_plate_cache;
    bool defer_estimate_after_tower_move{false};
    bool orientation_running{false};
    std::string orientation_diagnostic;
    bool cloud_start_pending {false};
    std::string lightweight_diagnostic;
    bool lightweight_analyzing {false};
    bool lightweight_stale {false};
    uint64_t last_lightweight_attempt {0};
    mutable std::mutex exact_estimate_mutex;
    ExactEstimateView exact_estimate;
    std::optional<ExactEstimateView> orientation_estimate;
    std::array<ExactEstimateView, 6> estimate_cache;
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
    const auto reviewed = m_impl->official_apply_estimate;
    m_impl->custom_slice_job = false;
    if (m_impl->versioned_apply_workflow->active_transaction())
        m_impl->workbench_official = m_impl->versioned_apply_workflow->notify_slice_completed(success, failure_code);
    else if (m_impl->native_session.pending()) {
        m_impl->native_session.complete(success, m_impl->current_native_revision(), failure_code);
        m_impl->workbench_official = m_impl->native_session.result();
        m_impl->finish_lightweight_apply(success &&
            m_impl->workbench_official.phase == AI::SmartSlicing::OfficialSlicePhase::Completed);
    }
    else
        m_impl->official_gateway->notify_slice_completed(success, failure_code);
    BOOST_LOG_TRIVIAL(info) << "[ModelWorkflow] completion phase=" << int(m_impl->workbench_official.phase)
        << " diagnostic=" << m_impl->workbench_official.diagnostic_code;
    m_impl->slice_goal_history.complete(
        success && m_impl->workbench_official.phase == AI::SmartSlicing::OfficialSlicePhase::Completed,
        m_impl->current_native_revision());
    if (success && reviewed && m_impl->observed_slice_input && m_impl->sliced_recommendation &&
        m_impl->recommendation_source == SlicingRecommendationSource::Local &&
        m_impl->slice_goal_history.last_sliced_goal(m_impl->current_native_revision()) == m_impl->selected_goal) {
        const auto input = m_impl->current_slice_input();
        const size_t index = static_cast<size_t>(m_impl->selected_goal);
        auto proven = *reviewed;
        if (proven.native_details && proven.parameters) {
            auto apply_effect = std::make_shared<OrcaTrialSliceDetails>(*proven.native_details);
            apply_effect->placement = {};
            proven.native_details = std::move(apply_effect);
            m_impl->estimate_cache = {};
            // The process was committed by the official transaction; its old
            // recommendation proof can serve the next X/Y-only edit.
            proven.status = ExactEstimateStatus::Ready;
            proven.moved_tower_position.reset();
            auto* plate = m_impl->plater.get_partplate_list().get_curr_plate();
            const auto* result = plate ? plate->get_slice_result() : nullptr;
            proven.official_result_id.reset();
            if (result && plate->is_slice_result_valid()) {
                proven.official_result_id = result->id;
                const auto& statistics = result->print_statistics;
                proven.estimated_time_seconds = statistics.modes[
                    static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
                proven.material_grams = result->material_weight_grams(statistics.total_volumes_per_extruder);
            }
            proven.revision = input.exact;
            m_impl->estimate_cache[index] = std::move(proven);
        }
        const bool process_changed = m_impl->observed_slice_input->process != input.process;
        m_impl->observed_slice_input = input;
        m_impl->defer_estimate_after_tower_move = false;
        if (process_changed) {
            // Refresh suggestions for the committed inputs while the verified
            // native result still supplies the active plan and statistics.
            m_impl->lightweight_refresh_pending = true;
            std::lock_guard<std::mutex> lock(m_impl->lightweight_mutex);
            if (m_impl->local_lightweight_analysis) {
                m_impl->local_lightweight_analysis->recommendation_available.fill(false);
                m_impl->local_lightweight_analysis->recommendation_available[index] = true;
                m_impl->lightweight_analysis = m_impl->local_lightweight_analysis;
            }
        }
    }
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

void SmartSlicingFeatureHost::notify_plate_selected()
{
    // Project the new plate in the same event that swaps the native preview.
    // This also invalidates pending work before a later timer can accept it.
    if (m_impl->workbench_active && !wxGetApp().is_closing())
        m_impl->process_recommendation_timer();
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
    if (active && wxGetApp().preset_bundle)
        m_impl->plater.get_partplate_list().prepare_workbench_smart_baseline(*wxGetApp().preset_bundle);
    if (m_impl->entry_button) m_impl->entry_button->Show(!active);
    if (active) {
        m_impl->show(false);
        m_impl->recommendation_revision_timer->Start(1000);
        if (ai_mode) {
            m_impl->cancel_exact_estimate();
            m_impl->refresh_workbench_input();
            m_impl->start_exact_estimate(m_impl->selected_goal);
            m_impl->publish_workbench();
        }
        else cancel_workbench_analysis();
    } else cancel_workbench_analysis();
}

void SmartSlicingFeatureHost::cancel_workbench_analysis()
{
    m_impl->cancel_exact_estimate();
    m_impl->cancel_recommendation_session(AI::SmartSlicing::RecommendationCancellationReason::ModeChanged);
    m_impl->cancel_cloud_request();
    const auto revision = m_impl->current_native_revision();
    {
        std::lock_guard<std::mutex> lock(m_impl->exact_estimate_mutex);
        m_impl->exact_estimate.status = ExactEstimateStatus::Failed;
        m_impl->exact_estimate.goal = m_impl->selected_goal;
        m_impl->exact_estimate.revision = revision;
        std::lock_guard<std::mutex> analysis_lock(m_impl->lightweight_mutex);
        if (m_impl->lightweight_analysis) {
            if (const auto* plan = m_impl->lightweight_analysis->find(Impl::slice_mode_for_goal(m_impl->selected_goal)))
                m_impl->exact_estimate.parameters = *plan;
        }
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
    const size_t index = static_cast<size_t>(m_impl->selected_goal) +
        (m_impl->recommendation_source == SlicingRecommendationSource::Cloud ? 3 : 0);
    m_impl->estimate_cache[index] = {};
    m_impl->orientation_estimate.reset();
    m_impl->defer_estimate_after_tower_move = false;
    m_impl->observed_slice_input = m_impl->current_slice_input();
    m_impl->observed_workbench_revision = m_impl->observed_slice_input->exact;
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

    m_impl->refresh_workbench_input();
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
            if (m_impl->lightweight_inputs && m_impl->observed_slice_input &&
                m_impl->lightweight_inputs->process_revision == m_impl->observed_slice_input->process) {
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
    using namespace AI::SmartSlicing;
    if (m_impl->native_session.pending() || m_impl->workbench_official.phase == OfficialSlicePhase::Slicing ||
        m_impl->versioned_apply_workflow->active_transaction() ||
        std::find(RECOMMENDATION_GOALS.begin(), RECOMMENDATION_GOALS.end(), goal) == RECOMMENDATION_GOALS.end()) return false;
    m_impl->refresh_workbench_input();
    const auto input = m_impl->current_slice_input();
    const auto before = m_impl->workbench_snapshot();
    if (!before.native_workspace_available || (m_impl->recommendation_source == SlicingRecommendationSource::Cloud &&
        before.cloud_recommendation.status != CloudRecommendationStatus::Ready)) return false;
    if (m_impl->goal_selection.selected(input.exact) == goal) return true;
    if (!m_impl->goal_selection.select(goal, input.exact)) return false;
    m_impl->cancel_exact_estimate();
    m_impl->native_session.reset();
    m_impl->slice_estimate = {};
    m_impl->workbench_official = {};
    m_impl->selected_goal = goal;
    if (m_impl->recommendation_source == SlicingRecommendationSource::Local) {
        if (!m_impl->ensure_local_goal(goal) && !before.lightweight_analyzing)
            m_impl->start_lightweight_only_analysis();
        const auto& cached = m_impl->estimate_cache[static_cast<size_t>(goal)];
        if (cached.status != ExactEstimateStatus::Ready)
            m_impl->defer_estimate_after_tower_move = false;
    }
    m_impl->start_exact_estimate(goal);
    m_impl->publish_workbench();
    return true;
}

bool SmartSlicingFeatureHost::request_orientation_comparison()
{
    m_impl->refresh_workbench_input();
    const auto current = workbench_snapshot();
    if (!current.can_start || current.orientation_running ||
        current.recommendation_source != SlicingRecommendationSource::Local ||
        current.exact_estimate.status != ExactEstimateStatus::Ready) return false;
    m_impl->start_exact_estimate(current.selected_goal, true);
    m_impl->publish_workbench();
    return m_impl->orientation_running;
}

bool SmartSlicingFeatureHost::select_orientation(const ExactEstimateView& reviewed)
{
    const auto current = workbench_snapshot();
    if (!current.can_start || !current.orientation_estimate || !reviewed.parameters ||
        reviewed.native_details != current.orientation_estimate->native_details ||
        reviewed.revision != current.session.workspace_revision)
        return false;
    auto confirmed = reviewed;
    confirmed.goal = current.selected_goal;
    confirmed.parameters->mode = Impl::slice_mode_for_goal(confirmed.goal);
    {
        std::lock_guard<std::mutex> lock(m_impl->lightweight_mutex);
        if (!m_impl->lightweight_analysis) return false;
        m_impl->lightweight_analysis->recommendations[static_cast<size_t>(confirmed.goal)] = *confirmed.parameters;
        m_impl->local_lightweight_analysis = m_impl->lightweight_analysis;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->exact_estimate_mutex);
        m_impl->exact_estimate = confirmed;
        m_impl->estimate_cache[static_cast<size_t>(confirmed.goal)] = std::move(confirmed);
        m_impl->orientation_estimate.reset();
    }
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
        m_impl->refresh_workbench_input();
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
            if (!reviewed.lightweight_analysis ||
                !reviewed.lightweight_analysis->find(selected_mode) ||
                !same_slice_parameters(*reviewed.lightweight_analysis->find(selected_mode), *lightweight_plan) ||
                reviewed.exact_estimate.native_details != current.exact_estimate.native_details)
                return {OfficialSlicePhase::Rejected, "reviewed_candidate_changed"};
            if (!m_impl->apply_lightweight_plan(current.exact_estimate, diagnostic))
                return {OfficialSlicePhase::Rejected,
                        diagnostic.empty() ? "smart_slice_plan_apply_failed" : std::move(diagnostic)};
            // Applying the reviewed plan changes the plate config and may
            // reposition the wipe tower. It is not a new recommendation input.
            const auto applied_revision = m_impl->current_native_revision();
            m_impl->official_apply_estimate = current.exact_estimate;
            m_impl->slice_estimate.capture(current.exact_estimate, current.session.workspace_revision,
                applied_revision, current.selected_goal, *lightweight_plan);
            m_impl->goal_selection.select(current.selected_goal, applied_revision);
            m_impl->observed_workbench_revision = applied_revision;
            m_impl->sliced_recommendation = *lightweight_plan;
            m_impl->slice_goal_history.begin(current.selected_goal, m_impl->current_native_revision());
            m_impl->workbench_official = {};
            if (!start_native_slice()) {
                m_impl->slice_goal_history.cancel();
                m_impl->finish_lightweight_apply(false);
                m_impl->publish_workbench();
            }
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

bool SmartSlicingFeatureHost::workbench_active() const
{
    return m_impl->workbench_active;
}

bool SmartSlicingFeatureHost::workbench_tower_pinned() const
{
    return m_impl->native_session.pending() &&
        ((m_impl->official_apply_estimate && m_impl->official_apply_estimate->native_details &&
          m_impl->official_apply_estimate->native_details->tower_position) ||
         (m_impl->custom_slice_job && m_impl->custom_tower_retried));
}

bool SmartSlicingFeatureHost::can_retry_custom_tower() const
{
    const auto* plate = m_impl->plater.get_partplate_list().get_curr_plate();
    return m_impl->custom_slice_job && !m_impl->custom_tower_retried &&
        m_impl->native_session.pending() && m_impl->native_session.revision() &&
        *m_impl->native_session.revision() == m_impl->current_native_revision() &&
        plate && plate->automatic_wipe_tower_position();
}

void SmartSlicingFeatureHost::rebind_custom_tower_position()
{
    if (!m_impl->custom_slice_job || !m_impl->native_session.pending() || !m_impl->quick_apply) return;
    const auto revision = m_impl->current_native_revision();
    m_impl->quick_apply->applied_revision = revision;
    m_impl->native_session.start(revision);
}

bool SmartSlicingFeatureHost::retry_custom_tower(const Vec2d& position)
{
    if (!position.allFinite() || !can_retry_custom_tower()) return false;
    auto& plates = m_impl->plater.get_partplate_list();
    plates.set_wipe_tower_position_for_plate(plates.get_curr_plate_index(), position, true);
    m_impl->custom_tower_retried = true;
    rebind_custom_tower_position();
    return m_impl->native_slice && m_impl->native_slice();
}

bool SmartSlicingFeatureHost::start_custom_slice()
{
    using namespace AI::SmartSlicing;
    auto* plate = m_impl->plater.get_partplate_list().get_curr_plate();
    if (!plate || !plate->has_printable_instances() || m_impl->native_session.pending() ||
        m_impl->plater.is_background_process_slicing() || m_impl->plater.inside_snapshot_capture())
        return false;

    constexpr auto action = "Apply Custom Slicing Process";
    UndoRedo::ActionSnapshotIdentity snapshot;
    {
        Plater::TakeSnapshot transaction(&m_impl->plater, action);
        if (!m_impl->plater.latest_main_snapshot_identity(action, snapshot))
            return false;
        plate->use_workbench_smart_process(false);
    }
    m_impl->slice_goal_history.cancel();
    m_impl->quick_apply = Impl::QuickApply{snapshot, m_impl->current_native_revision()};
    m_impl->custom_slice_job = true;
    m_impl->custom_tower_retried = false;
    if (start_native_slice())
        return true;
    m_impl->custom_slice_job = false;
    m_impl->finish_lightweight_apply(false);
    m_impl->publish_workbench();
    return false;
}

bool SmartSlicingFeatureHost::start_native_slice()
{
    const auto state = workbench_snapshot();
    const bool verified_apply = state.native_workspace_available && m_impl->quick_apply &&
        m_impl->quick_apply->applied_revision == m_impl->current_native_revision();
    if (!state.can_start_native && !verified_apply) {
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
