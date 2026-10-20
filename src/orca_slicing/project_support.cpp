#include "project_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "libslic3r/Sha256Digest.hpp"

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
        return fallback;
    return Vec3d(value[0].get<double>(), value[1].get<double>(), value[2].get<double>());
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
                    instance->set_scaling_factor(json_vec3(entry["scale"], instance->get_scaling_factor()));
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
                           const Json& transforms, const std::string& workspaceRoot)
{
    if (inputPath.empty())
        throw std::invalid_argument("inputPath is empty");
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(inputPath)))
        throw std::runtime_error("project file does not exist: " + inputPath);

    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::EnableSilent};
    Model model = Model::read_from_file(inputPath, &config, &substitutions,
                                        LoadStrategy::AddDefaultInstances);
    if (model.objects.empty())
        throw std::runtime_error("project contains no model objects: " + inputPath);

    if (!configOverrides.is_null()) {
        if (!configOverrides.is_object())
            throw std::invalid_argument("configOverrides must be an object");
        for (auto it = configOverrides.begin(); it != configOverrides.end(); ++it) {
            if (config.option(it.key(), true) == nullptr)
                throw std::invalid_argument("unknown configuration override: " + it.key());
            config.set_deserialize(it.key(), native_value(it.value()), substitutions);
        }
    }
    apply_transforms(model, transforms);

    LoadedProject project{std::move(model), std::move(config), inputPath, sha256_file(inputPath), Json::object()};
    project.metadata["workspaceRoot"] = workspaceRoot;
    project.metadata["transforms"] = transforms.is_null() ? Json::array() : transforms;
    project.metadata["sourceFormat"] = "3mf";
    project.metadata["colors"] = colors_json(project.config);
    project.metadata["filamentMap"] = filament_map_json(project.config);
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
    result["inputPath"] = project.inputPath;
    result["inputSha256"] = project.inputSha256;
    result["name"] = project.model.objects.empty() ? std::filesystem::u8path(project.inputPath).stem().u8string()
                                                    : project.model.objects.front()->name;
    result["model"] = Json{{"vertices", vertices}, {"triangles", triangles},
                            {"bounds", bounds_json(project.model.bounding_box_exact())},
                            {"colors", colors_json(project.config)},
                            {"filamentMap", filament_map_json(project.config)}};
    result["config"] = config_json(project.config);
    if (!result.contains("transforms"))
        result["transforms"] = Json::array();
    return result;
}

Json write_scene(const LoadedProject& project, const std::string& outputPath)
{
    if (outputPath.empty())
        throw std::invalid_argument("outputPath is empty");
    const Json colors = colors_json(project.config);
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

DynamicPrintConfig quality_config(const DynamicPrintConfig& base, const std::string& quality)
{
    if (quality != "standard" && quality != "fine" && quality != "fast" && quality != "smart")
        throw std::invalid_argument("quality must be standard, fine, fast or smart");
    const double layer_height = quality == "fine" ? 0.12 : quality == "fast" ? 0.28 : 0.20;
    DynamicPrintConfig result(base);
    result.set("layer_height", layer_height, true);
    return result;
}

} // namespace orca::slicing
