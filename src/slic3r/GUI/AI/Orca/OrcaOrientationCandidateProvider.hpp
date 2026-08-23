#pragma once

#include "libslic3r/Orient.hpp"
#include "slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

struct OrcaOrientationCandidateInput
{
    Model model;
    DynamicPrintConfig config;
    std::set<uint64_t> locked_object_ids;
    std::set<uint64_t> locked_instance_ids;
    std::function<bool()> stopcondition;
    bool plate_locked{false};
};

class OrcaOrientationCandidateProvider
{
public:
    std::vector<AI::SmartSlicing::SliceCandidate>
    generate(OrcaOrientationCandidateInput input, const AI::SmartSlicing::WorkspaceRevision& revision) const
    {
        if (input.plate_locked || input.model.objects.empty() || (input.stopcondition && input.stopcondition()))
            return {};

        orientation::OrientMeshs selected;
        orientation::OrientMeshs excluded;
        std::vector<ModelInstance*> selected_instances;
        std::vector<Transform3d> selected_originals;

        try {
            for (ModelObject* object : input.model.objects) {
                if (object == nullptr)
                    continue;
                const bool object_locked = input.locked_object_ids.count(object->id().id) != 0;
                for (ModelInstance* instance : object->instances) {
                    if (instance == nullptr)
                        continue;

                    orientation::OrientMesh orient_mesh;
                    orient_mesh.name = object->name;
                    orient_mesh.mesh = object->mesh();
                    orient_mesh.overhang_angle = object->config.has("support_threshold_angle") ?
                        object->config.opt_int("support_threshold_angle") :
                        input.config.opt_int("support_threshold_angle");

                    const bool is_locked = object_locked ||
                        input.locked_instance_ids.count(instance->id().id) != 0 || !instance->printable;
                    if (is_locked) {
                        excluded.push_back(std::move(orient_mesh));
                        continue;
                    }

                    orient_mesh.setter = [instance](const orientation::OrientMesh& result) {
                        instance->rotate(result.rotation_matrix);
                        ModelObject* target_object = instance->get_object();
                        target_object->invalidate_bounding_box();
                        target_object->ensure_on_bed();
                    };
                    selected.push_back(std::move(orient_mesh));
                    selected_instances.push_back(instance);
                    selected_originals.push_back(instance->get_matrix());
                }
            }
            if (selected.empty())
                return {};

            orientation::OrientParams params;
            params.parallel = false;
            params.progressind = [](unsigned, std::string) {};
            params.stopcondition = input.stopcondition ? std::move(input.stopcondition) : [] { return false; };
            orientation::orient(selected, excluded, params);
            if (params.stopcondition())
                return {};
            for (const orientation::OrientMesh& orient_mesh : selected)
                orient_mesh.apply();

            AI::SmartSlicing::SliceCandidate candidate;
            candidate.id            = "orientation-stability-native-v1";
            candidate.base_revision = revision;
            candidate.goal          = AI::SmartSlicing::CandidateGoal::Stability;
            candidate.explanation   = "native_auto_orientation_stability_candidate";
            candidate.status        = AI::SmartSlicing::CandidateStatus::Draft;
            candidate.placement.transforms.reserve(selected_instances.size());
            for (size_t index = 0; index < selected_instances.size(); ++index) {
                ModelInstance* instance = selected_instances[index];
                const Transform3d matrix = instance->get_matrix();
                if (matrix.isApprox(selected_originals[index]))
                    continue;

                AI::SmartSlicing::ObjectTransform transform;
                transform.object_id   = instance->get_object()->id().id;
                transform.instance_id = instance->id().id;
                for (Eigen::Index row = 0; row < matrix.rows(); ++row)
                    for (Eigen::Index column = 0; column < matrix.cols(); ++column)
                        transform.matrix[static_cast<size_t>(row * matrix.cols() + column)] = matrix(row, column);
                candidate.placement.transforms.push_back(std::move(transform));
            }
            if (candidate.placement.transforms.empty())
                return {};
            return {std::move(candidate)};
        } catch (...) {
            return {};
        }
    }
};

} // namespace Slic3r::GUI
