#include "OrcaModelPreparation.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Slic3r::GUI {

namespace {
bool is_generated_base_name(const std::string& name)
{
    return name == "AI round base" || name == "AI oval base" || name == "AI rectangle base";
}

const ModelVolume& preparation_body(const ModelObject& source, bool add_base)
{
    if (source.instances.size() != 1 || source.is_cut())
        throw std::invalid_argument("requires_single_uncut_instance");
    const ModelVolume* first_part = nullptr;
    for (const auto* volume : source.volumes) {
        if (volume->is_model_part() && first_part == nullptr)
            first_part = volume;
        if (add_base && is_generated_base_name(volume->name))
            throw std::invalid_argument("base_already_exists");
    }
    if (first_part == nullptr)
        throw std::invalid_argument("empty_model");
    return *first_part;
}

void set_preparation_base(ModelPreparation& result, TriangleMesh mesh, const char* name,
    const ModelVolume& body, const Transform3d& world)
{
    result.base = std::make_unique<Model>();
    auto* base = result.base->add_object()->add_volume(std::move(mesh), ModelVolumeType::MODEL_PART, false);
    base->name = name;
    base->config.set_key_value("extruder", new ConfigOptionInt(std::max(1, body.extruder_id())));
    base->set_transformation(result.instance.get_matrix().inverse() * world);
}
}

std::optional<WorkbenchRoundBaseLayout> workbench_round_base_layout(
    const BoundingBoxf3& body_bounds, std::string* reason)
{
    const auto fail = [&](const char* message) -> std::optional<WorkbenchRoundBaseLayout> {
        if (reason) *reason = message;
        return std::nullopt;
    };
    if (!body_bounds.defined || !body_bounds.min.allFinite() || !body_bounds.max.allFinite())
        return fail("当前模型边界无效，无法安全生成底座。");
    const Vec3d size = body_bounds.size();
    if (!size.allFinite() || (size.array() <= 1e-6).any())
        return fail("当前模型尺寸无效，无法安全生成底座。");

    WorkbenchRoundBaseLayout layout;
    layout.radius = std::max(4.0, std::max(size.x(), size.y()) / 2.0 + 2.0);
    const double overlap = std::min(0.2, size.z() / 2.0);
    layout.body_transform.translation().z() = layout.height - overlap;

    Vec3d origin = body_bounds.min + size / 2.0;
    origin.z() = body_bounds.min.z();
    layout.base_transform.translation() = origin;
    layout.bounds = body_bounds;
    layout.bounds.translate(layout.body_transform.translation());
    layout.bounds.merge(origin + Vec3d(-layout.radius, -layout.radius, 0.0));
    layout.bounds.merge(origin + Vec3d(layout.radius, layout.radius, layout.height));
    if (!layout.bounds.min.allFinite() || !layout.bounds.max.allFinite() || !layout.bounds.size().allFinite())
        return fail("当前模型尺寸无效，无法安全生成底座。");
    return layout;
}

ModelPreparation prepare_workbench_round_base(const ModelObject& source)
{
    const auto& body = preparation_body(source, true);
    ModelPreparation result;
    result.instance = source.instances.front()->get_transformation();
    const auto& matrix = result.instance.get_matrix();
    if (!matrix.matrix().allFinite() || std::abs(matrix.linear().determinant()) < 1e-12)
        throw std::invalid_argument("invalid_transform");
    const auto layout = workbench_round_base_layout(source.instance_bounding_box(0));
    if (!layout) throw std::invalid_argument("invalid_dimensions");

    result.instance.set_offset(result.instance.get_offset() + layout->body_transform.translation());
    set_preparation_base(result, TriangleMesh(its_make_cylinder(layout->radius, layout->height)),
        "AI round base", body, layout->base_transform);
    return result;
}

ModelPreparation prepare_model(const ModelObject& source, const ModelPreparationOptions& options)
{
    const double height = options.total_height_mm;
    const double base_height = options.base_height_mm;
    if (!std::isfinite(height) || height < 1.0 || height > 1000.0 ||
        (options.add_round_base && (!std::isfinite(base_height) || base_height < 0.5 || base_height >= height)))
        throw std::invalid_argument("invalid_dimensions");
    const auto& body = preparation_body(source, options.add_round_base);

    Transform3d matrix = source.instances.front()->get_matrix();
    if (!matrix.matrix().allFinite() || std::abs(matrix.linear().determinant()) < 1e-12)
        throw std::invalid_argument("invalid_transform");
    auto bounds = source.instance_bounding_box(0);
    if (!bounds.defined || !bounds.size().allFinite() || bounds.size().z() <= 1e-6)
        throw std::invalid_argument("empty_model");
    const Vec3d original_center = bounds.center();

    const double overlap = options.add_round_base ? std::min(0.2, base_height / 2.0) : 0.0;
    const double body_height = height - (options.add_round_base ? base_height - overlap : 0.0);
    matrix.linear() *= body_height / bounds.size().z();
    bounds = BoundingBoxf3();
    for (const auto* volume : source.volumes)
        if (volume->is_model_part())
            bounds.merge(volume->mesh().transformed_bounding_box(matrix * volume->get_matrix()));
    matrix.translation() += Vec3d(
        original_center.x() - bounds.center().x(), original_center.y() - bounds.center().y(),
        (options.add_round_base ? base_height - overlap : 0.0) - bounds.min.z());

    ModelPreparation result;
    result.instance = Geometry::Transformation(matrix);

    if (options.add_round_base) {
        // Cover the complete XY footprint, with a 2 mm margin. The small
        // overlap joins the base to the body during native multipart slicing.
        const double width = std::max(8.0, double(bounds.size().x()) + 4.0);
        const double depth = std::max(8.0, double(bounds.size().y()) + 4.0);
        TriangleMesh mesh;
        switch (options.base_template) {
        case ModelBaseTemplate::Oval: {
            const double radius = std::max(width, depth) / 2.0;
            mesh = TriangleMesh(its_make_cylinder(radius, base_height));
            Transform3d oval_scale = Transform3d::Identity();
            oval_scale.linear()(0, 0) = width / (2.0 * radius);
            oval_scale.linear()(1, 1) = depth / (2.0 * radius);
            // Use the mesh transform API so cached statistics and bounds are
            // refreshed along with the vertex coordinates.
            mesh.transform(oval_scale);
            break;
        }
        case ModelBaseTemplate::Rectangle:
            mesh = TriangleMesh(its_make_cube(width, depth, base_height));
            mesh.translate(float(-width / 2.0), float(-depth / 2.0), 0.0f);
            break;
        case ModelBaseTemplate::Round:
        default:
            // Cover the longest XY span and retain the requested 2 mm radial
            // margin on every side of the transformed body footprint.
            mesh = TriangleMesh(its_make_cylinder(std::max(4.0, std::max(width, depth) / 2.0), base_height));
            break;
        }
        const char* name = options.base_template == ModelBaseTemplate::Oval ? "AI oval base" :
            options.base_template == ModelBaseTemplate::Rectangle ? "AI rectangle base" : "AI round base";
        Transform3d world = Transform3d::Identity();
        world.translation() = Vec3d(original_center.x(), original_center.y(), 0.0);
        set_preparation_base(result, std::move(mesh), name, body, world);
    }
    return result;
}

void apply_model_preparation(ModelObject& source, const ModelPreparation& proposal)
{
    source.instances.front()->set_transformation(proposal.instance);
    if (proposal.base)
        source.add_volume(*proposal.base->objects.front()->volumes.front());
    source.invalidate_bounding_box();
}

} // namespace Slic3r::GUI
