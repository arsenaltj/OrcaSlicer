#pragma once

#include "LocalPrintColorCommit.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include <functional>

namespace Slic3r::GUI::LocalPrintModelImport {

// This route owns one already decoded/color-assigned model, not the ordinary
// file importer. Placement is completed before taking the project's snapshot.
inline bool prepare(const ModelObject& source, const Vec2d& placement, const Vec2d& bed_size,
    std::unique_ptr<Model>& destination, std::string& error)
{
    auto fail = [&](const char* message) { error = message; return false; };
    error.clear();
    if (source.volumes.size() != 1 || !source.volumes.front()->is_model_part() ||
        !source.volumes.front()->material_id().empty() || source.input_file.empty() ||
        source.volumes.front()->source.input_file.empty() || !source.instances.empty())
        return fail("Color import requires one new model with its preserved source reference.");
    const auto& mesh = source.volumes.front()->mesh().its;
    if (mesh.indices.empty() || mesh.vertices.empty() || !placement.allFinite() ||
        !bed_size.allFinite() || (bed_size.array() <= 0).any())
        return fail("The model or build area is empty or invalid.");
    for (const auto& vertex : mesh.vertices) if (!vertex.allFinite()) return fail("The model contains invalid coordinates.");
    for (const auto& face : mesh.indices) for (int c = 0; c < 3; ++c)
        if (face[c] < 0 || size_t(face[c]) >= mesh.vertices.size()) return fail("The model contains invalid face indices.");
    auto prepared = std::make_unique<Model>();
    auto* object = prepared->add_object(source);
    object->center_around_origin();
    auto* instance = object->add_instance();
    const auto size = object->instance_bounding_box(0).size();
    // Scaling after recipe validation invalidates its region and Z evidence.
    // Ordinary file import retains its existing scale prompt and behavior.
    if (!size.allFinite() || (size.head<2>().array() / bed_size.array() > 10).any())
        return fail("Scale this model before matching colors; confirmed color import does not rescale it.");
    object->ensure_on_bed(false);
    auto offset = instance->get_offset(); offset.x() = placement.x(); offset.y() = placement.y();
    instance->set_offset(offset);
    instance->set_assemble_transformation(instance->get_transformation());
    destination = std::move(prepared);
    return true;
}

inline bool place(ModelObject& object, const Polygon& bed, const arrangement::ArrangePolygons& obstacles,
    const DynamicPrintConfig& config, double height, std::string& error)
{
    error.clear();
    if (object.instances.size() != 1 || bed.points.size() < 3 || !std::isfinite(height) || height <= 0.) {
        error = "The target plate is unavailable for placement."; return false;
    }
    auto* instance = object.instances.front();
    if (object.instance_bounding_box(0).max.z() > height) {
        error = "The new model exceeds the target plate's printable height."; return false;
    }
    arrangement::ArrangePolygon polygon;
    instance->get_arrange_polygon(&polygon, config);
    // FirstFit skips negative bin IDs as unfit, even for a new moving item.
    polygon.bed_idx = 0;
    polygon.height = object.instance_bounding_box(0).size().z();
    arrangement::ArrangePolygons moving {polygon};
    arrangement::ArrangeParams params(scale_(2.));
    params.allow_rotations = false;
    params.do_final_align = false;
    params.parallel = false;
    params.printable_height = float(height);
    params.progressind = [](unsigned, std::string) {};
    // Orca's arranger expects the spacing to be applied to the input polygons.
    moving.front().inflation = params.min_obj_distance / 2;
    auto fixed = obstacles;
    arrangement::update_unselected_items_inflation(fixed, &config, params);
    arrangement::arrange(moving, fixed, bed.points, params);
    const auto& placed = moving.front();
    if (placed.bed_idx != 0 || !diff_ex({placed.transformed_poly()}, {ExPolygon(bed)}).empty()) {
        error = "The model does not fit on the current plate. The working copy has been retained."; return false;
    }
    for (const auto& obstacle : obstacles)
        if (obstacle.bed_idx == 0 && !intersection_ex({placed.transformed_poly()}, {obstacle.transformed_poly()}).empty()) {
            error = "The new model overlaps an existing object or excluded area."; return false;
        }
    auto offset = instance->get_offset();
    offset.x() = unscale<double>(placed.translation.x());
    offset.y() = unscale<double>(placed.translation.y());
    instance->set_offset(offset);
    instance->set_assemble_transformation(instance->get_transformation());
    return true;
}

struct PlacementSnapshot {
    Vec2d placement;
    Vec2d bed_size;
    Polygon bed;
    arrangement::ArrangePolygons obstacles;
    DynamicPrintConfig config;
    double height {0.};

    bool prepare_model(const ModelObject& source, std::unique_ptr<Model>& model, std::string& error) const
    {
        return prepare(source, placement, bed_size, model, error) &&
            place(*model->objects.front(), bed, obstacles, config, height, error);
    }
};

// The optional finalizer runs under the caller's snapshot suppression before
// registration. A failed refresh restores only this attempt's model and config.
template<class Record>
bool adopt(Model& model, const ModelObject& source, UndoRedo::ProjectConfigUndo::Prepared& config,
    ProjectConfigRestore::ColorCache& cache, PresetBundle& bundle, Record&& record,
    size_t& index, std::string& error, std::function<bool(size_t)> finalize = {})
{
    const size_t before = model.objects.size();
    const bool changed = config.changed;
    bool committed = false;
    auto rollback = [&] {
        if (committed && changed) {
            config.changed = true;
            ProjectConfigRestore::commit(config, cache, bundle);
            config.changed = true;
        }
        while (model.objects.size() > before) model.delete_object(model.objects.size() - 1);
    };
    try {
        model.add_object(source);
        if (finalize) {
            ProjectConfigRestore::commit(config, cache, bundle);
            committed = true;
            if (!finalize(before)) {
                rollback(); error = "Unable to finish the color import."; return false;
            }
        }
        if (!record()) {
            rollback(); error = "Unable to record the color import in project history."; return false;
        }
    } catch (...) { rollback(); throw; }
    if (!committed) ProjectConfigRestore::commit(config, cache, bundle);
    index = before;
    return true;
}
} // namespace Slic3r::GUI::LocalPrintModelImport
