#pragma once

#include <memory>
#include <optional>
#include <string>
#include "libslic3r/Model.hpp"

namespace Slic3r::GUI {

enum class ModelBaseTemplate : unsigned char
{
    Round,
    Oval,
    Rectangle,
};

struct ModelPreparationOptions
{
    double total_height_mm {120.0};
    bool add_round_base {false};
    double base_height_mm {3.0};
    ModelBaseTemplate base_template {ModelBaseTemplate::Round};
};

struct ModelPreparation
{
    Geometry::Transformation instance;
    std::unique_ptr<Model> base;
};

struct WorkbenchRoundBaseLayout
{
    Transform3d body_transform {Transform3d::Identity()};
    Transform3d base_transform {Transform3d::Identity()};
    BoundingBoxf3 bounds;
    double radius {0.0};
    double height {3.0};
};

std::optional<WorkbenchRoundBaseLayout> workbench_round_base_layout(
    const BoundingBoxf3& body_bounds, std::string* reason = nullptr);

// Compute a detached proposal without touching the source or its autosave state.
ModelPreparation prepare_model(const ModelObject& source, const ModelPreparationOptions& options);
ModelPreparation prepare_workbench_round_base(const ModelObject& source);
// Call inside a native snapshot. Existing volume IDs, meshes and painting stay intact.
void apply_model_preparation(ModelObject& source, const ModelPreparation& proposal);

} // namespace Slic3r::GUI
