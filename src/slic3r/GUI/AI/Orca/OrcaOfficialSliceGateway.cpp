#include "OrcaOfficialSliceGateway.hpp"

#include "OrcaParameterProposalAdapter.hpp"
#include "OrcaPlacementTransformValidator.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Slic3r::GUI {
namespace {

struct SmartSlicingTransformTarget
{
    size_t object_index{0};
    ModelInstance* instance{nullptr};
    Transform3d matrix{Transform3d::Identity()};
};

bool collect_targets(Plater& plater, const AI::SmartSlicing::SliceCandidate& candidate,
                     std::vector<SmartSlicingTransformTarget>& targets, std::string& diagnostic)
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

        SmartSlicingTransformTarget target;
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
                    target = {object_index, instance, matrix};
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            diagnostic = "transform_target_missing";
            return false;
        }
        if (!orca_placement_transform_preserves_geometry(target.instance->get_matrix(), target.matrix)) {
            diagnostic = "transform_changes_geometry";
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

std::string validate_candidate(Plater& plater, const AI::SmartSlicing::SliceCandidate& candidate)
{
    std::vector<SmartSlicingTransformTarget> targets;
    DynamicPrintConfig parameter_patch;
    std::string diagnostic;
    if (!collect_targets(plater, candidate, targets, diagnostic) ||
        !prepare_parameter_patch(plater, candidate, parameter_patch, diagnostic))
        return diagnostic;
    return {};
}

OrcaApplyMutationResult apply_candidate(Plater& plater, const AI::SmartSlicing::SliceCandidate& candidate)
{
    std::vector<SmartSlicingTransformTarget> targets;
    DynamicPrintConfig parameter_patch;
    std::string diagnostic;
    if (!collect_targets(plater, candidate, targets, diagnostic) ||
        !prepare_parameter_patch(plater, candidate, parameter_patch, diagnostic))
        return {false, false, std::move(diagnostic)};

    std::vector<SmartSlicingTransformTarget> changed;
    std::vector<size_t> changed_object_indices;
    for (const SmartSlicingTransformTarget& target : targets) {
        if (!target.instance->get_matrix().isApprox(target.matrix)) {
            changed.push_back(target);
            changed_object_indices.push_back(target.object_index);
        }
    }
    if (changed.empty() && candidate.parameters.entries.empty())
        return {true, false, {}};
    std::sort(changed_object_indices.begin(), changed_object_indices.end());
    changed_object_indices.erase(std::unique(changed_object_indices.begin(), changed_object_indices.end()),
                                 changed_object_indices.end());

    bool transaction_started = false;
    try {
        {
            Plater::TakeSnapshot transaction(&plater, "Apply Smart Slicing Candidate");
            transaction_started = true;
            for (const SmartSlicingTransformTarget& target : changed)
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
        return {true, true, {}};
    } catch (...) {
        if (transaction_started && plater.can_undo())
            plater.undo();
        return {false, false, "candidate_apply_rolled_back"};
    }
}

} // namespace

OrcaOfficialSliceGateway::OrcaOfficialSliceGateway(Plater& plater, RevisionFn revision, ActionFn start_slice)
    : OrcaOfficialSliceGateway(
          std::move(revision),
          [&plater](const AI::SmartSlicing::SliceCandidate& candidate) { return validate_candidate(plater, candidate); },
          [&plater](const AI::SmartSlicing::SliceCandidate& candidate) { return apply_candidate(plater, candidate); },
          std::move(start_slice),
          [&plater] {
              plater.select_view_3D("Preview");
              return plater.is_preview_shown();
          },
          [&plater] {
              if (!plater.can_undo())
                  return false;
              plater.undo();
              return true;
          })
{}

} // namespace Slic3r::GUI
