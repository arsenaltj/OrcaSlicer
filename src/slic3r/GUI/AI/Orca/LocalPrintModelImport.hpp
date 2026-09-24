#pragma once

#include "LocalPrintColorCommit.hpp"

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

// No UI callback is allowed here. A failed native clone or history registration
// removes only this attempt's appended object, before any config commit. Native
// allocation/backup exceptions are allowed to propagate after the rollback.
template<class Record>
bool adopt(Model& model, const ModelObject& source, UndoRedo::ProjectConfigUndo::Prepared& config,
    ProjectConfigRestore::ColorCache& cache, PresetBundle& bundle, Record&& record,
    size_t& index, std::string& error)
{
    const size_t before = model.objects.size();
    auto rollback = [&] { while (model.objects.size() > before) model.delete_object(model.objects.size() - 1); };
    try {
        model.add_object(source);
        if (!record()) {
            rollback(); error = "Unable to record the color import in project history."; return false;
        }
    } catch (...) { rollback(); throw; }
    ProjectConfigRestore::commit(config, cache, bundle);
    index = before;
    return true;
}
} // namespace Slic3r::GUI::LocalPrintModelImport
