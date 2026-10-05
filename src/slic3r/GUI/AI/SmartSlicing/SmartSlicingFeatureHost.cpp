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
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"

#include <wx/aui/framemanager.h>
#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/weakref.h>
#include <wx/glcanvas.h>

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
            auto candidates = workspace->candidate_proposals(snapshot.context->revision);
            trial_executor->prepare_session_input(workspace->capture_trial_slice_input(), candidates);
            return candidates;
        }, [this] {
            trial_executor->cancel_trial_slice();
        }, [this] {
            this->plater.add_file();
        }, &plater, [this] {
            show(false);
            this->plater.collapse_sidebar(false);
            // The mode switch hides its focused action. Hand keyboard input
            // to the existing native preset after the AUI layout settles.
            wxWeakRef<wxWindow> native_printer(this->sidebar.printer_combox());
            wxGetApp().CallAfter([native_printer] {
                if (!wxGetApp().is_closing() && native_printer &&
                    native_printer->IsShownOnScreen() && native_printer->IsEnabled())
                    native_printer->SetFocus();
            });
        });
        presenter->set_view_changed([this](const SmartSlicingViewModel& view) { render(view); });
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

    ~Impl()
    {
        if (selection_canvas)
            selection_canvas->Unbind(EVT_GLCANVAS_OBJECT_SELECT, &Impl::on_selection_changed, this);
        if (selection_sidebar)
            selection_sidebar->Unbind(EVT_OBJ_LIST_OBJECT_SELECT, &Impl::on_selection_changed, this);
        plater.Unbind(wxEVT_AUI_PANE_CLOSE, &Impl::on_pane_close, this);
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

    void render(const SmartSlicingViewModel& view)
    {
        if (panel != nullptr)
            panel->render(view);
        if (view.is_stale || view.summary_key == "official_slice_complete" || view.summary_key == "canceled" ||
            view.summary_key == "preflight_failed")
            trial_executor->clear_session_input();
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
    std::unique_ptr<AI::SmartSlicing::SmartSlicingCoordinator> coordinator;
    std::unique_ptr<OrcaWorkflowRuntimeStore> runtime_store;
    std::unique_ptr<SmartSlicingPresenter> presenter;
    SmartSlicingPanel* panel { nullptr };
    wxWeakRef<wxWindow> selection_canvas;
    wxWeakRef<wxWindow> selection_sidebar;
    wxButton* entry_button { nullptr };
    bool restore_sidebar { false };
    std::optional<size_t> applied_snapshot_time;
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
