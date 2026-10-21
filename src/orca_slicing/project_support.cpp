#include "project_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <set>
#include <map>
#include <cstring>
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "libslic3r/Sha256Digest.hpp"
#include "libslic3r/Utils.hpp"

namespace orca::slicing {
namespace {

using namespace Slic3r;

std::string native_value(const Json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_boolean())
        return value.get<bool>() ? "1" : "0";
    if (value.is_number())
        return value.dump();
    if (value.is_array()) {
        std::string result;
        for (size_t i = 0; i < value.size(); ++i) {
            if (i != 0)
                result.push_back(',');
            result += native_value(value[i]);
        }
        return result;
    }
    throw std::invalid_argument("configuration override must be a scalar or array: " + value.dump());
}

std::string sha256_file(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open project: " + path);

    Sha256Digest digest;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 && !digest.update(buffer.data(), static_cast<size_t>(count)))
            throw std::runtime_error("cannot hash project: " + path);
    }
    if (!input.eof())
        throw std::runtime_error("cannot read project: " + path);
    const std::string result = digest.final_hex();
    if (result.empty())
        throw std::runtime_error("cannot finalize project hash: " + path);
    return result;
}

Vec3d json_vec3(const Json& value, const Vec3d& fallback)
{
    if (!value.is_array() || value.size() != 3)
        throw std::invalid_argument("TRANSFORM_REQUIRES_THREE_COMPONENTS");
    Vec3d result(value[0].get<double>(), value[1].get<double>(), value[2].get<double>());
    if (!result.allFinite()) throw std::invalid_argument("NONFINITE_TRANSFORM");
    return result;
}

void apply_transforms(Model& model, const Json& transforms)
{
    if (transforms.is_null() || transforms.empty())
        return;
    if (!transforms.is_array())
        throw std::invalid_argument("transforms must be an array");

    for (const Json& entry : transforms) {
        if (!entry.is_object())
            throw std::invalid_argument("each transform must be an object");
        const int object_index = entry.value("objectIndex", -1);
        const int instance_index = entry.value("instanceIndex", -1);
        const size_t object_begin = object_index < 0 ? 0 : static_cast<size_t>(object_index);
        const size_t object_end = object_index < 0 ? model.objects.size() : object_begin + 1;
        if (object_end > model.objects.size())
            throw std::out_of_range("transform objectIndex is outside the project");

        for (size_t oi = object_begin; oi < object_end; ++oi) {
            ModelObject* object = model.objects[oi];
            const size_t instance_begin = instance_index < 0 ? 0 : static_cast<size_t>(instance_index);
            const size_t instance_end = instance_index < 0 ? object->instances.size() : instance_begin + 1;
            if (instance_end > object->instances.size())
                throw std::out_of_range("transform instanceIndex is outside the object");
            for (size_t ii = instance_begin; ii < instance_end; ++ii) {
                ModelInstance* instance = object->instances[ii];
                if (entry.contains("translation"))
                    instance->set_offset(json_vec3(entry["translation"], instance->get_offset()));
                if (entry.contains("rotation"))
                    instance->set_rotation(json_vec3(entry["rotation"], instance->get_rotation()));
                if (entry.contains("scale"))
                    {
                    const Vec3d scale = json_vec3(entry["scale"], instance->get_scaling_factor());
                    if (scale.minCoeff() <= 0) throw std::invalid_argument("SCALE_MUST_BE_POSITIVE");
                    instance->set_scaling_factor(scale);
                }
            }
            object->invalidate_bounding_box();
        }
    }
}

Json bounds_json(const BoundingBoxf3& bounds)
{
    if (!bounds.defined)
        return Json{{"min", Json::array({0.0, 0.0, 0.0})}, {"max", Json::array({0.0, 0.0, 0.0})}};
    return Json{{"min", Json::array({bounds.min.x(), bounds.min.y(), bounds.min.z()})},
                {"max", Json::array({bounds.max.x(), bounds.max.y(), bounds.max.z()})}};
}

Json colors_json(const DynamicPrintConfig& config)
{
    Json colors = Json::array();
    if (const auto* option = config.option<ConfigOptionStrings>("filament_colour"))
        for (const auto& color : option->values)
            colors.push_back(color);
    return colors;
}

Json filament_map_json(const DynamicPrintConfig& config)
{
    Json result = Json::array();
    if (const auto* option = config.option<ConfigOptionInts>("filament_map"))
        for (const int value : option->values)
            result.push_back(value);
    return result;
}

std::array<std::uint8_t, 4> parse_color(std::string value)
{
    if (!value.empty() && value.front() == '#')
        value.erase(value.begin());
    if (value.size() == 8) {
        // Orca color strings are normally RGB; accept RGBA for imported projects.
    } else if (value.size() == 6) {
        value += "ff";
    } else {
        return {255, 255, 255, 255};
    }
    std::array<std::uint8_t, 4> rgba{};
    for (size_t i = 0; i < rgba.size(); ++i) {
        unsigned int byte = 0;
        std::istringstream stream(value.substr(i * 2, 2));
        stream >> std::hex >> byte;
        if (stream.fail())
            return {255, 255, 255, 255};
        rgba[i] = static_cast<std::uint8_t>(byte);
    }
    return rgba;
}

struct VertexKey {
    std::int64_t x;
    std::int64_t y;
    std::int64_t z;
    bool operator==(const VertexKey& other) const { return x == other.x && y == other.y && z == other.z; }
};
struct VertexKeyHash {
    size_t operator()(const VertexKey& value) const noexcept {
        size_t h = static_cast<size_t>(value.x);
        h = h * 31u + static_cast<size_t>(value.y);
        h = h * 31u + static_cast<size_t>(value.z);
        return h;
    }
};
struct FaceKey {
    std::array<std::uint32_t, 3> value{};
    bool operator==(const FaceKey& other) const { return value == other.value; }
};
struct FaceKeyHash {
    size_t operator()(const FaceKey& value) const noexcept {
        size_t h = value.value[0];
        h = h * 31u + value.value[1];
        h = h * 31u + value.value[2];
        return h;
    }
};

VertexKey vertex_key(const stl_vertex& vertex)
{
    constexpr double scale = 1000000.0;
    return {static_cast<std::int64_t>(std::llround(static_cast<double>(vertex.x()) * scale)),
            static_cast<std::int64_t>(std::llround(static_cast<double>(vertex.y()) * scale)),
            static_cast<std::int64_t>(std::llround(static_cast<double>(vertex.z()) * scale))};
}

FaceKey face_key(std::uint32_t a, std::uint32_t b, std::uint32_t c)
{
    FaceKey result{{a, b, c}};
    std::sort(result.value.begin(), result.value.end());
    return result;
}

std::vector<std::uint16_t> painted_face_palettes(const ModelVolume& volume, size_t palette_count)
{
    const auto& mesh = volume.mesh().its;
    std::vector<std::uint16_t> palettes(mesh.indices.size(), 0);
    const int default_extruder = volume.extruder_id();
    if (default_extruder > 0)
        std::fill(palettes.begin(), palettes.end(), static_cast<std::uint16_t>(std::min<size_t>(palette_count - 1, static_cast<size_t>(default_extruder - 1))));
    if (volume.mmu_segmentation_facets.empty() || palette_count == 0)
        return palettes;

    std::unordered_map<VertexKey, std::vector<std::uint32_t>, VertexKeyHash> vertices;
    vertices.reserve(mesh.vertices.size());
    for (std::uint32_t i = 0; i < mesh.vertices.size(); ++i)
        vertices[vertex_key(mesh.vertices[i])].push_back(i);

    std::unordered_map<FaceKey, size_t, FaceKeyHash> faces;
    faces.reserve(mesh.indices.size());
    for (size_t i = 0; i < mesh.indices.size(); ++i) {
        const auto& triangle = mesh.indices[i];
        faces.emplace(face_key(triangle(0), triangle(1), triangle(2)), i);
    }

    for (int state = static_cast<int>(EnforcerBlockerType::Extruder1);
         state <= static_cast<int>(EnforcerBlockerType::ExtruderMax); ++state) {
        const auto selected = volume.mmu_segmentation_facets.get_facets(volume, static_cast<EnforcerBlockerType>(state));
        const std::uint16_t palette = static_cast<std::uint16_t>(std::min<size_t>(palette_count - 1, static_cast<size_t>(state - 1)));
        for (const auto& triangle : selected.indices) {
            const auto i0 = vertices.find(vertex_key(selected.vertices[triangle(0)]));
            const auto i1 = vertices.find(vertex_key(selected.vertices[triangle(1)]));
            const auto i2 = vertices.find(vertex_key(selected.vertices[triangle(2)]));
            if (i0 == vertices.end() || i1 == vertices.end() || i2 == vertices.end())
                continue;
            bool mapped = false;
            for (const std::uint32_t a : i0->second) {
                for (const std::uint32_t b : i1->second) {
                    for (const std::uint32_t c : i2->second) {
                        const auto found = faces.find(face_key(a, b, c));
                        if (found != faces.end()) {
                            palettes[found->second] = palette;
                            mapped = true;
                            break;
                        }
                    }
                    if (mapped) break;
                }
                if (mapped) break;
            }
        }
    }
    return palettes;
}

} // namespace

LoadedProject load_project(const std::string& inputPath, const Json& configOverrides,
                           const Json& transforms, const std::string& workspaceRoot, const Json& projectCommands)
{
    if (inputPath.empty())
        throw std::invalid_argument("inputPath is empty");
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(inputPath)))
        throw std::runtime_error("project file does not exist: " + inputPath);

    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::EnableSilent};
    Model model;
    if (!workspaceRoot.empty())
        set_temporary_dir((std::filesystem::u8path(workspaceRoot) / "native-temp").u8string());
    try {
        model = Model::read_from_file(inputPath, &config, &substitutions,
                                      LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel |
                                      LoadStrategy::LoadConfig | LoadStrategy::LoadAuxiliary);
    } catch (const std::exception& error) {
        const std::string detail = bbs_3mf_last_error();
        throw std::runtime_error(std::string(error.what()) + (detail.empty() ? std::string() : "; " + detail));
    }
    if (model.objects.empty()) {
        const std::string detail = bbs_3mf_last_error();
        throw std::runtime_error("project contains no model objects: " + inputPath + (detail.empty() ? std::string() : "; " + detail));
    }

    DynamicPrintConfig complete = DynamicPrintConfig::full_print_config();
    complete.apply(config);
    config = std::move(complete);
    const Json sourceColors = colors_json(config);
    apply_config_overrides(config, configOverrides);
    apply_transforms(model, transforms);

    LoadedProject project{std::move(model), std::move(config), inputPath, sha256_file(inputPath), Json::object()};
    project.metadata["sourceColors"] = sourceColors;
    project.metadata["workspaceRoot"] = workspaceRoot;
    project.metadata["transforms"] = transforms.is_null() ? Json::array() : transforms;
    project.metadata["sourceFormat"] = std::filesystem::u8path(inputPath).extension().u8string();
    project.metadata["colors"] = colors_json(project.config);
    project.metadata["filamentMap"] = filament_map_json(project.config);
    apply_project_commands(project, projectCommands);
    return project;
}

Json config_json(const DynamicPrintConfig& config)
{
    Json result = Json::object();
    for (const auto& key : config.keys()) {
        if (const ConfigOption* option = config.option(key))
            result[key] = option->serialize();
    }
    return result;
}

Json describe_project(const LoadedProject& project)
{
    size_t vertices = 0;
    size_t triangles = 0;
    for (const ModelObject* object : project.model.objects) {
        for (const ModelVolume* volume : object->volumes) {
            if (!volume->is_model_part())
                continue;
            vertices += volume->mesh().its.vertices.size();
            triangles += volume->mesh().its.indices.size();
        }
    }

    Json result = project.metadata.is_object() ? project.metadata : Json::object();
    result["schemaVersion"] = 2;
    result["inputPath"] = project.inputPath;
    result["inputSha256"] = project.inputSha256;
    result["name"] = project.model.objects.empty() ? std::filesystem::u8path(project.inputPath).stem().u8string()
                                                    : project.model.objects.front()->name;
    result["model"] = Json{{"vertices", vertices}, {"triangles", triangles},
                            {"bounds", bounds_json(project.model.bounding_box_exact())},
                            {"colors", project.metadata.value("sourceColors",colors_json(project.config))},
                            {"filamentMap", filament_map_json(project.config)}};
    result["objects"] = Json::array();
    for (size_t oi = 0; oi < project.model.objects.size(); ++oi) {
        const auto* object = project.model.objects[oi];
        Json item{{"objectId", "object-" + std::to_string(oi)}, {"objectIndex", oi},
                  {"name", object->name}, {"bounds", bounds_json(object->bounding_box_exact())},
                  {"parts", Json::array()}, {"instances", Json::array()}};
        for (size_t vi = 0; vi < object->volumes.size(); ++vi) {
            const auto* volume = object->volumes[vi];
            item["parts"].push_back(Json{{"volumeIndex", vi}, {"name", volume->name},
                {"material", volume->extruder_id()}, {"isModelPart", volume->is_model_part()},
                {"painted", !volume->mmu_segmentation_facets.empty()}});
        }
        for (size_t ii = 0; ii < object->instances.size(); ++ii) {
            const auto* instance = object->instances[ii];
            auto vector_json = [](const Vec3d& v) { return Json::array({v.x(),v.y(),v.z()}); };
            item["instances"].push_back(Json{{"instanceIndex", ii},
                {"instanceId", "object-" + std::to_string(oi) + "-instance-" + std::to_string(ii)},
                {"translation", vector_json(instance->get_offset())},
                {"rotation", vector_json(instance->get_rotation())},
                {"scale", vector_json(instance->get_scaling_factor())},
                {"bounds", bounds_json(object->instance_bounding_box(ii))}});
        }
        result["objects"].push_back(std::move(item));
    }
    result["config"] = config_json(project.config);
    result["configSha256"] = json_sha256(result["config"]);
    if (!result.contains("transforms"))
        result["transforms"] = Json::array();
    return result;
}

Json write_scene(const LoadedProject& project, const std::string& outputPath)
{
    if (outputPath.empty())
        throw std::invalid_argument("outputPath is empty");
    const Json colors = project.metadata.value("sourceColors",colors_json(project.config));
    const size_t palette_count = std::max<size_t>(1, colors.size());
    std::vector<std::array<std::uint8_t, 4>> palette;
    palette.reserve(palette_count);
    for (const auto& color : colors)
        palette.push_back(parse_color(color.get<std::string>()));
    while (palette.size() < palette_count)
        palette.push_back({255, 255, 255, 255});

    std::vector<stl_vertex> vertices;
    std::vector<stl_triangle_vertex_indices> triangles;
    std::vector<std::uint16_t> face_palettes;
    for (const ModelObject* object : project.model.objects) {
        const size_t instance_count = object->instances.empty() ? 1 : object->instances.size();
        for (size_t ii = 0; ii < instance_count; ++ii) {
            const Transform3d instance_matrix = object->instances.empty() ? Transform3d::Identity() : object->instances[ii]->get_matrix();
            for (const ModelVolume* volume : object->volumes) {
                if (!volume->is_model_part())
                    continue;
                const auto& mesh = volume->mesh().its;
                const auto painted = painted_face_palettes(*volume, palette_count);
                const Transform3d matrix = instance_matrix * volume->get_matrix();
                const std::uint32_t vertex_offset = static_cast<std::uint32_t>(vertices.size());
                vertices.reserve(vertices.size() + mesh.vertices.size());
                for (const auto& vertex : mesh.vertices) {
                    const Vec3d transformed = matrix * vertex.cast<double>();
                    vertices.emplace_back(static_cast<float>(transformed.x()), static_cast<float>(transformed.y()), static_cast<float>(transformed.z()));
                }
                triangles.reserve(triangles.size() + mesh.indices.size());
                face_palettes.reserve(face_palettes.size() + mesh.indices.size());
                for (size_t fi = 0; fi < mesh.indices.size(); ++fi) {
                    const auto& triangle = mesh.indices[fi];
                    { stl_triangle_vertex_indices output_triangle; output_triangle << static_cast<int>(triangle(0) + vertex_offset), static_cast<int>(triangle(1) + vertex_offset), static_cast<int>(triangle(2) + vertex_offset); triangles.push_back(output_triangle); }
                    face_palettes.push_back(fi < painted.size() ? painted[fi] : 0);
                }
            }
        }
    }
    if (vertices.size() > std::numeric_limits<std::uint32_t>::max() || triangles.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("scene exceeds ORCASC01 uint32 limits");

    const std::filesystem::path path = std::filesystem::u8path(outputPath);
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot create scene: " + outputPath);
    const char magic[8] = {'O', 'R', 'C', 'A', 'S', 'C', '0', '1'};
    output.write(magic, sizeof(magic));
    const std::uint32_t vertex_count = static_cast<std::uint32_t>(vertices.size());
    const std::uint32_t triangle_count = static_cast<std::uint32_t>(triangles.size());
    const std::uint32_t palette_size = static_cast<std::uint32_t>(palette.size());
    const std::uint32_t flags = 1u;
    const std::uint64_t reserved = 0;
    output.write(reinterpret_cast<const char*>(&vertex_count), sizeof(vertex_count));
    output.write(reinterpret_cast<const char*>(&triangle_count), sizeof(triangle_count));
    output.write(reinterpret_cast<const char*>(&palette_size), sizeof(palette_size));
    output.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
    output.write(reinterpret_cast<const char*>(&reserved), sizeof(reserved));
    for (const auto& vertex : vertices)
        output.write(reinterpret_cast<const char*>(vertex.data()), sizeof(float) * 3);
    for (const auto& triangle : triangles)
        output.write(reinterpret_cast<const char*>(triangle.data()), sizeof(std::uint32_t) * 3);
    for (const std::uint16_t value : face_palettes)
        output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    for (const auto& color : palette)
        output.write(reinterpret_cast<const char*>(color.data()), color.size());
    if (!output)
        throw std::runtime_error("cannot write scene: " + outputPath);

    return Json{{"scenePath", outputPath}, {"vertexCount", vertex_count}, {"triangleCount", triangle_count},
                {"paletteCount", palette_size}, {"flags", flags}, {"bounds", bounds_json(project.model.bounding_box_exact())}};
}


std::string file_sha256(const std::string& path) { return sha256_file(path); }
std::string json_sha256(const Json& value)
{
    const std::string text = value.dump();
    Sha256Digest digest;
    if (!digest.update(text.data(), text.size())) throw std::runtime_error("HASH_FAILED");
    const auto result = digest.final_hex();
    if (result.empty()) throw std::runtime_error("HASH_FAILED");
    return result;
}

void apply_config_overrides(DynamicPrintConfig& config, const Json& overrides)
{
    if (overrides.is_null()) return;
    if (!overrides.is_object()) throw std::invalid_argument("CONFIG_MUST_BE_OBJECT");
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        if (!config.option(it.key(), true)) throw std::invalid_argument("UNKNOWN_CONFIG:" + it.key());
        config.set_deserialize(it.key(), native_value(it.value()), substitutions);
    }
}

namespace {
void place_on_bed(ModelObject& object, size_t ii)
{
    const auto bounds = object.instance_bounding_box(ii);
    auto* instance = object.instances.at(ii);
    Vec3d offset = instance->get_offset();
    offset.z() -= bounds.min.z();
    instance->set_offset(offset);
    object.invalidate_bounding_box();
}

bool inside_polygon(const Vec2d& p, const Pointfs& polygon)
{
    bool inside = false;
    if (polygon.size() < 3) return false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const auto& a = polygon[i]; const auto& b = polygon[j];
        const Vec2d ab = b-a, ap = p-a;
        const double cross = ab.x()*ap.y()-ab.y()*ap.x();
        if (std::abs(cross) < 1e-6 && ap.dot(ab) >= -1e-6 && ap.dot(ab) <= ab.squaredNorm()+1e-6) return true;
        if ((a.y() > p.y()) != (b.y() > p.y()) &&
            p.x() < (b.x()-a.x())*(p.y()-a.y())/(b.y()-a.y())+a.x()) inside = !inside;
    }
    return inside;
}

void arrange_project(LoadedProject& project, double gap)
{
    const auto* bed = project.config.option<ConfigOptionPoints>("printable_area");
    if (!bed || bed->values.size() < 3 || !std::isfinite(gap) || gap < 0)
        throw std::invalid_argument("ARRANGE_REQUIRES_VALID_BED_AND_GAP");
    Vec2d lo = bed->values.front(), hi = lo;
    for (const auto& p : bed->values) { lo = lo.cwiseMin(p); hi = hi.cwiseMax(p); }
    double x = lo.x()+gap, y = lo.y()+gap, row = 0;
    struct Placement { ModelObject* object; size_t instance; Vec3d offset; };
    std::vector<Placement> placements;
    for (auto* object : project.model.objects) for (size_t ii=0; ii<object->instances.size(); ++ii) {
        const auto bb = object->instance_bounding_box(ii);
        const Vec3d size = bb.size();
        if (x+size.x() > hi.x()-gap) { x=lo.x()+gap; y+=row+gap; row=0; }
        if (y+size.y() > hi.y()-gap) throw std::runtime_error("ARRANGE_BED_OVERFLOW");
        for (const Vec2d& corner : std::array<Vec2d,4>{{{x,y},{x+size.x(),y},{x,y+size.y()},{x+size.x(),y+size.y()}}})
            if (!inside_polygon(corner, bed->values)) throw std::runtime_error("ARRANGE_BED_OVERFLOW");
        // Rectangular shelf packing is deterministic; reject bed exclusion regions too.
        if (const auto* excluded = project.config.option<ConfigOptionPoints>("bed_exclude_area")) {
            if (excluded->values.size() >= 3) {
                Vec2d elo=excluded->values.front(), ehi=elo;
                for (const auto& p : excluded->values) { elo=elo.cwiseMin(p); ehi=ehi.cwiseMax(p); }
                if (x<ehi.x() && x+size.x()>elo.x() && y<ehi.y() && y+size.y()>elo.y())
                    throw std::runtime_error("ARRANGE_EXCLUDED_AREA");
            }
        }
        placements.push_back({object,ii,object->instances[ii]->get_offset()+Vec3d(x-bb.min.x(),y-bb.min.y(),-bb.min.z())});
        x+=size.x()+gap; row=std::max(row,size.y());
    }
    for (const auto& p : placements) {
        p.object->instances[p.instance]->set_offset(p.offset); p.object->invalidate_bounding_box();
    }
}
}

void apply_project_commands(LoadedProject& project, const Json& commands)
{
    if (!commands.is_array()) throw std::invalid_argument("PROJECT_COMMANDS_MUST_BE_ARRAY");
    project.metadata["projectCommands"] = commands;
    project.metadata["materialAssignments"] = Json::array();
    for (const auto& raw : commands) {
        const Json cmd = raw.is_string() ? Json{{"type",raw}} : raw;
        if (!cmd.is_object()) throw std::invalid_argument("INVALID_PROJECT_COMMAND");
        const std::string type = cmd.value("type", std::string());
        if (type == "arrange") { arrange_project(project,cmd.value("gapMm",5.0)); continue; }
        const int oi=cmd.value("objectIndex",0);
        if (oi<0 || size_t(oi)>=project.model.objects.size()) throw std::out_of_range("COMMAND_OBJECT_NOT_FOUND");
        auto& object=*project.model.objects[oi];
        const int ii=cmd.value("instanceIndex",0);
        if (ii<0 || size_t(ii)>=object.instances.size()) throw std::out_of_range("COMMAND_INSTANCE_NOT_FOUND");
        auto* instance=object.instances[ii];
        if (type == "transform") {
            Json transform=cmd; transform["objectIndex"]=oi; transform["instanceIndex"]=ii;
            apply_transforms(project.model,Json::array({transform}));
            if (cmd.contains("sizeMm")) {
                const Vec3d wanted=json_vec3(cmd["sizeMm"],Vec3d::Zero());
                const Vec3d current=object.instance_bounding_box(ii).size();
                if (wanted.minCoeff()<=0 || current.minCoeff()<=0) throw std::invalid_argument("INVALID_SIZE_MM");
                // World-axis dimensions are only independent for cardinal rotations.
                const auto r=instance->get_rotation();
                constexpr double halfPi=1.57079632679489661923;
                for (int axis=0;axis<3;++axis)
                    if (std::abs(r[axis]/halfPi-std::round(r[axis]/halfPi))>1e-6)
                        throw std::invalid_argument("SIZE_REQUIRES_CARDINAL_ROTATION_USE_SCALE");
                Vec3d factors=wanted.cwiseQuotient(current);
                const Matrix3d rotation=instance->get_transformation().get_rotation_matrix().linear().cwiseAbs();
                instance->set_scaling_factor(instance->get_scaling_factor().cwiseProduct(rotation.transpose()*factors));
            }
        } else if (type == "duplicate") {
            Vec3d shift(object.instance_bounding_box(ii).size().x()+5.0,0,0);
            if (cmd.contains("offset")) shift=json_vec3(cmd["offset"],shift);
            auto* copy=object.add_instance(*instance);
            copy->set_offset(instance->get_offset()+shift);
        } else if (type == "orient") {
            // Supported geometric orientation: six cardinal faces, minimize height,
            // deterministic ties keep the earliest candidate; finally put on bed.
            constexpr double pi=3.14159265358979323846;
            const std::array<Vec3d,6> rotations{{{0,0,0},{pi/2,0,0},{-pi/2,0,0},{pi,0,0},{0,pi/2,0},{0,-pi/2,0}}};
            Vec3d best=rotations.front(); double height=std::numeric_limits<double>::infinity();
            for (const auto& rotation:rotations) {
                instance->set_rotation(rotation); object.invalidate_bounding_box();
                const double z=object.instance_bounding_box(ii).size().z();
                if (z<height-1e-6) { height=z; best=rotation; }
            }
            instance->set_rotation(best); object.invalidate_bounding_box(); place_on_bed(object,ii);
        } else if (type == "assignMaterial" || type == "material") {
            const int vi=cmd.value("volumeIndex",-1), filament=cmd.value("filamentIndex",-1);
            const auto* colors=project.config.option<ConfigOptionStrings>("filament_colour");
            if (filament<0 || (colors && size_t(filament)>=colors->values.size())) throw std::out_of_range("INVALID_FILAMENT_INDEX");
            if (vi < -1 || (vi>=0 && size_t(vi)>=object.volumes.size())) throw std::out_of_range("COMMAND_VOLUME_NOT_FOUND");
            for (size_t v=0;v<object.volumes.size();++v) if (vi<0 || int(v)==vi) {
                object.volumes[v]->config.set("extruder",filament+1);
                project.metadata["materialAssignments"].push_back(Json{{"objectIndex",oi},{"volumeIndex",v},{"filamentIndex",filament}});
            }
        } else throw std::invalid_argument("PROJECT_COMMAND_UNSUPPORTED:"+type);
        object.invalidate_bounding_box();
    }
}

Model slicing_model(const LoadedProject& project)
{
    Model model(project.model);
    for (const auto& assignment : project.metadata.value("materialAssignments",Json::array())) {
        auto* volume=model.objects.at(assignment.at("objectIndex").get<size_t>())->volumes.at(assignment.at("volumeIndex").get<size_t>());
        volume->mmu_segmentation_facets.reset();
        volume->config.set("extruder",assignment.at("filamentIndex").get<int>()+1);
    }
    return model;
}

void validate_output_capabilities(const LoadedProject& project)
{
#ifdef ORCA_MOBILE_ANDROID
    if (const auto* prime=project.config.option<ConfigOptionBool>("enable_prime_tower"))
        if (prime->value) throw std::runtime_error("UNSUPPORTED_PRIME_TOWER:Android triangulation is unavailable; explicitly configure a supported single-material job");
    const Model model=slicing_model(project);
    std::set<int> materials;
    for (const auto* object:model.objects) for (const auto* volume:object->volumes) if (volume->is_model_part()) {
        if (!volume->mmu_segmentation_facets.empty())
            throw std::runtime_error("UNSUPPORTED_PAINTED_MATERIAL_SWITCH:Assign an explicit single material to each part to export; original colors are retained");
        int material=volume->extruder_id();
        if (material<=0) material=object->config.extruder();
        materials.insert(std::max(1,material));
    }
    if (materials.size()>1) throw std::runtime_error("UNSUPPORTED_MATERIAL_SWITCH:Android P1 requires explicit single-material assignment");
#endif
}


Json write_preview(const GCodeProcessorResult& gcode, const std::string& path)
{
    struct Layer { float z=0; std::vector<std::pair<size_t,size_t>> moves; uint32_t first=0; };
    std::map<unsigned int,Layer> layers;
    for (size_t i=1; i<gcode.moves.size(); ++i) {
        const auto& m=gcode.moves[i];
        if (m.type==EMoveType::Extrude && !m.internal_only && m.extrusion_role!=erCustom &&
            m.extrusion_role!=erNone && m.delta_extruder>0 && m.width>0 && m.position.allFinite()) {
            auto& layer=layers[m.layer_id];
            const float z=m.print_z>0 ? m.print_z : m.position.z();
            if (layer.z==0 || z<layer.z) layer.z=z;
        }
    }
    if (layers.empty()) throw std::runtime_error("PREVIEW_NO_PRINT_LAYERS");
    size_t previousIndex=0;
    for (size_t i=1; i<gcode.moves.size(); ++i) {
        const auto& m=gcode.moves[i];
        if (m.internal_only || (m.type!=EMoveType::Travel && m.type!=EMoveType::Wipe && m.type!=EMoveType::Extrude)) continue;
        const size_t from=previousIndex; previousIndex=i;
        const auto& previous=gcode.moves[from];
        auto layer=layers.find(m.layer_id);
        if (layer==layers.end() || !m.position.allFinite() || !previous.position.allFinite() ||
            (m.position-previous.position).squaredNorm()<1e-12f) continue;
        if (m.type==EMoveType::Travel || m.type==EMoveType::Wipe ||
            (m.type==EMoveType::Extrude && m.extrusion_role!=erNone && m.extrusion_role!=erCustom && m.delta_extruder>0))
            layer->second.moves.emplace_back(i,from);
    }
    uint64_t total=0;
    Json heights=Json::array();
    for (auto& entry:layers) {
        auto& layer=entry.second;
        if (total>UINT32_MAX || layer.moves.size()>UINT32_MAX-total) throw std::overflow_error("PREVIEW_TOO_LARGE");
        layer.first=static_cast<uint32_t>(total); total+=layer.moves.size(); heights.push_back(layer.z);
    }
    const auto outputPath=std::filesystem::u8path(path);
    if (outputPath.has_parent_path()) std::filesystem::create_directories(outputPath.parent_path());
    std::ofstream output(outputPath,std::ios::binary|std::ios::trunc);
    if (!output) throw std::runtime_error("PREVIEW_CREATE_FAILED");
    auto integer=[&](uint64_t value,size_t bytes) { for (size_t i=0;i<bytes;++i) output.put(static_cast<char>((value>>(8*i))&255)); };
    auto real=[&](float value) { uint32_t bits; std::memcpy(&bits,&value,4); integer(bits,4); };
    output.write("ORCAPV01",8); integer(layers.size(),4); integer(total,4); integer(32,8); integer(32+16*layers.size(),8);
    for (const auto& entry:layers) {
        const auto& layer=entry.second;
        real(layer.z); integer(layer.first,4); integer(layer.moves.size(),4); integer(0,4);
    }
    for (const auto& entry:layers) for (const auto& indices:entry.second.moves) {
        const auto& m=gcode.moves[indices.first]; const auto& a=gcode.moves[indices.second].position;
        for (int axis=0;axis<3;++axis) real(a[axis]);
        for (int axis=0;axis<3;++axis) real(m.position[axis]);
        real(m.width); integer(m.extruder_id,2); integer(static_cast<uint8_t>(m.extrusion_role),1);
        integer(m.type==EMoveType::Extrude?1:m.type==EMoveType::Travel?2:4,1);
    }
    output.flush();
    if (!output) throw std::runtime_error("PREVIEW_WRITE_FAILED");
    output.close();
    return Json{{"previewPath",path},{"previewSize",std::filesystem::file_size(outputPath)},
        {"previewSha256",file_sha256(path)},{"layerCount",layers.size()},{"layerHeights",heights},
        {"segmentCount",total},{"previewFlags",{{"extrusion",1},{"travel",2},{"wipe",4}}}};
}

DynamicPrintConfig quality_config(const DynamicPrintConfig& base, const std::string& quality)
{
    if (quality != "standard" && quality != "fine" && quality != "fast")
        throw std::invalid_argument("QUALITY_REQUIRES_CONFIRMED_EFFECTIVE_CONFIG");
    double nozzle = 0.4;
    if (const auto* diameters = base.option<ConfigOptionFloats>("nozzle_diameter"))
        if (!diameters->values.empty()) nozzle = diameters->values.front();
    if (!std::isfinite(nozzle) || nozzle <= 0) throw std::invalid_argument("INVALID_NOZZLE_DIAMETER");
    DynamicPrintConfig result(base);
    result.set("layer_height", nozzle * (quality == "fine" ? 0.30 : quality == "fast" ? 0.70 : 0.50), true);
    return result;
}

} // namespace orca::slicing
