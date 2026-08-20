#pragma once

#include "libslic3r/ModelArrange.hpp"
#include "slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

struct OrcaPlacementCandidateInput
{
    Model model;
    DynamicPrintConfig config;
    arrangement::ArrangeParams arrange_params;
    std::set<uint64_t> locked_object_ids;
    std::set<uint64_t> locked_instance_ids;
    bool plate_locked{false};
};

class OrcaPlacementCandidateProvider
{
public:
    std::vector<AI::SmartSlicing::SliceCandidate>
    generate(OrcaPlacementCandidateInput input, const AI::SmartSlicing::WorkspaceRevision& revision) const
    {
        if (input.plate_locked || input.model.objects.empty())
            return {};

        arrangement::ArrangePolygons selected;
        arrangement::ArrangePolygons fixed;
        std::vector<ModelInstance*> selected_instances;
        std::vector<Transform3d> selected_originals;
        std::vector<std::pair<ModelInstance*, Transform3d>> fixed_instances;

        try {
            for (ModelObject* object : input.model.objects) {
                if (object == nullptr)
                    continue;
                const bool object_locked = input.locked_object_ids.count(object->id().id) != 0;
                for (ModelInstance* instance : object->instances) {
                    if (instance == nullptr)
                        continue;
                    arrangement::ArrangePolygon polygon = get_instance_arrange_poly(instance, input.config);
                    const bool instance_locked = object_locked ||
                        input.locked_instance_ids.count(instance->id().id) != 0 || !instance->printable;
                    if (instance_locked) {
                        polygon.itemid = static_cast<int>(fixed.size());
                        fixed.push_back(std::move(polygon));
                        fixed_instances.emplace_back(instance, instance->get_matrix());
                    } else {
                        polygon.itemid = static_cast<int>(selected.size());
                        selected.push_back(std::move(polygon));
                        selected_instances.push_back(instance);
                        selected_originals.push_back(instance->get_matrix());
                    }
                }
            }
            if (selected.empty())
                return {};

            arrangement::ArrangeParams params = std::move(input.arrange_params);
            params.parallel                    = false;
            params.progressind                 = [](unsigned, std::string) {};
            arrangement::update_arrange_params(params, &input.config, selected);
            arrangement::update_selected_items_inflation(selected, &input.config, params);
            arrangement::update_unselected_items_inflation(fixed, &input.config, params);
            arrangement::update_selected_items_axis_align(selected, &input.config, params);
            const Points bed = arrangement::get_shrink_bedpts(&input.config, params);
            if (bed.size() < 3)
                return {};

            arrangement::arrange(selected, fixed, bed, params);
            if (std::any_of(selected.begin(), selected.end(), [](const arrangement::ArrangePolygon& polygon) {
                    return polygon.bed_idx != 0;
                }))
                return {};
            for (arrangement::ArrangePolygon& polygon : selected)
                polygon.apply();

            for (const auto& [instance, original] : fixed_instances)
                if (!instance->get_matrix().isApprox(original))
                    return {};
            bool changed = false;
            for (size_t index = 0; index < selected_instances.size(); ++index)
                changed = changed || !selected_instances[index]->get_matrix().isApprox(selected_originals[index]);
            if (!changed)
                return {};

            AI::SmartSlicing::SliceCandidate candidate;
            candidate.id               = "placement-stability-native-v1";
            candidate.base_revision    = revision;
            candidate.goal             = AI::SmartSlicing::CandidateGoal::Stability;
            candidate.explanation      = "native_arrange_stability_candidate";
            candidate.status           = AI::SmartSlicing::CandidateStatus::Draft;
            candidate.placement.transforms.reserve(selected_instances.size());
            for (ModelInstance* instance : selected_instances) {
                AI::SmartSlicing::ObjectTransform transform;
                transform.object_id   = instance->get_object()->id().id;
                transform.instance_id = instance->id().id;
                const Transform3d matrix = instance->get_matrix();
                for (Eigen::Index row = 0; row < matrix.rows(); ++row)
                    for (Eigen::Index column = 0; column < matrix.cols(); ++column)
                        transform.matrix[static_cast<size_t>(row * matrix.cols() + column)] = matrix(row, column);
                candidate.placement.transforms.push_back(std::move(transform));
            }
            return {std::move(candidate)};
        } catch (...) {
            return {};
        }
    }
};

} // namespace Slic3r::GUI
