#include "BeautyAppearance.hpp"
#include "ModelArtifact.hpp"
#include "GlbGeometryEditing.hpp"
#include "SurfacePartition.hpp"
#include "libslic3r/Format/AssimpImport.hpp"
#include "libslic3r/TexturePainting.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

namespace Slic3r::AI {
namespace {
using Json = nlohmann::json;
constexpr uint64_t max_bytes = 512ull * 1024 * 1024;
constexpr uint64_t max_pixels = 16ull * 1024 * 1024;
struct Canceled {};
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void checkpoint(const std::function<bool()>& canceled) { if (canceled && canceled()) throw Canceled{}; }
uint32_t u32(const unsigned char* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint32_t be32(const unsigned char* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
void append32(std::vector<unsigned char>& bytes, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<unsigned char>(value >> shift));
}
size_t integer(const Json& object, const char* name) {
    const auto& value = object.at(name);
    require(value.is_number_integer() && value.get<int64_t>() >= 0, "Invalid GLB index or length.");
    return value.get<size_t>();
}
void parameter(double value, double low, double high) {
    require(std::isfinite(value) && value >= low && value <= high, "Appearance parameters are out of range.");
}
struct Document {
    Json json;
    std::vector<unsigned char> binary;
};
Document read_glb(const boost::filesystem::path& path, const std::function<bool()>& canceled) {
    require(model_artifact_format(path) == "glb" && is_model_artifact(path), "Appearance editing requires an existing GLB of at most 512 MB.");
    const auto size = boost::filesystem::file_size(path);
    require(size >= 28, "Incomplete GLB file.");
    std::vector<unsigned char> bytes(size);
    boost::filesystem::ifstream input(path, std::ios::binary);
    require(bool(input), "Cannot open the source GLB.");
    for (size_t offset = 0; offset < bytes.size(); offset += 65536) {
        checkpoint(canceled);
        input.read(reinterpret_cast<char*>(bytes.data() + offset), std::min(size_t(65536), bytes.size() - offset));
        require(bool(input), "The source GLB changed or could not be read.");
    }
    require(u32(bytes.data()) == 0x46546c67 && u32(bytes.data() + 4) == 2 && u32(bytes.data() + 8) == size,
        "Invalid GLB 2.0 header.");
    const size_t json_size = u32(bytes.data() + 12), bin_header = 20 + json_size;
    require(json_size > 0 && json_size % 4 == 0 && json_size <= 32 * 1024 * 1024 &&
        u32(bytes.data() + 16) == 0x4e4f534a && bin_header + 8 <= bytes.size(), "Invalid GLB description.");
    Document doc;
    doc.json = Json::parse(bytes.begin() + 20, bytes.begin() + bin_header);
    require(doc.json.at("asset").value("version", "") == "2.0", "Only GLB 2.0 is supported.");
    require(u32(bytes.data() + bin_header + 4) == 0x004e4942 &&
        bin_header + 8 + u32(bytes.data() + bin_header) == bytes.size(), "GLB must have one embedded binary buffer.");
    const auto& buffers = doc.json.at("buffers");
    require(buffers.size() == 1 && !buffers[0].contains("uri"), "GLB must have one embedded binary buffer.");
    const size_t payload = integer(buffers[0], "byteLength");
    require(payload <= bytes.size() - bin_header - 8 && bytes.size() - bin_header - 8 - payload <= 3,
        "Invalid GLB buffer length.");
    doc.binary.assign(bytes.begin() + bin_header + 8, bytes.begin() + bin_header + 8 + payload);
    for (const auto& extension : doc.json.value("extensionsRequired", Json::array()))
        require(extension == "KHR_materials_unlit" || extension == "KHR_texture_transform" || extension == "KHR_mesh_quantization",
            "The GLB requires an extension that appearance editing does not support.");
    for (const auto& mesh : doc.json.at("meshes")) for (const auto& primitive : mesh.at("primitives"))
        require(primitive.value("mode", 4) == 4 && !primitive.contains("targets"), "Appearance editing requires static triangle meshes.");
    for (const auto& node : doc.json.value("nodes", Json::array()))
        require(!node.contains("skin") && !node.contains("weights"), "Bake animated GLB geometry before editing appearance.");
    for (const auto& image : doc.json.value("images", Json::array()))
        require(!image.contains("uri") && image.contains("bufferView"), "Appearance editing currently requires buffer-embedded PNG/JPEG images.");
    return doc;
}

std::vector<unsigned char> image_bytes(const Document& doc, size_t index) {
    const auto& image = doc.json.at("images").at(index);
    const auto mime = image.value("mimeType", "");
    require(mime == "image/png" || mime == "image/jpeg", "Appearance editing supports PNG and JPEG color textures.");
    const auto& view = doc.json.at("bufferViews").at(integer(image, "bufferView"));
    require(integer(view, "buffer") == 0 && !view.contains("byteStride"), "Invalid embedded GLB image view.");
    const size_t offset = view.contains("byteOffset") ? integer(view, "byteOffset") : 0;
    const size_t length = integer(view, "byteLength");
    require(offset <= doc.binary.size() && length <= doc.binary.size() - offset && length > 0, "Embedded GLB image exceeds the buffer.");
    return {doc.binary.begin() + offset, doc.binary.begin() + offset + length};
}

cv::Mat decode_image(const std::vector<unsigned char>& encoded) {
    cv::Mat result;
    if (encoded.size() >= 2 && encoded[0] == 0xff && encoded[1] == 0xd8) {
        // The bundled OpenCV intentionally excludes JPEG. Reuse Orca's strict
        // libjpeg decoder, which rejects corrupt/truncated images before editing.
        TextureImage texture;
        texture.data = encoded;
        std::vector<unsigned char> pixels;
        int width = 0, height = 0;
        require(decode_texture_to_pixels(texture, pixels, width, height), "Cannot decode the JPEG color texture.");
        require(uint64_t(width) * height <= max_pixels, "Color textures above 16 megapixels are not yet editable.");
        result = cv::Mat(height, width, CV_8UC3, pixels.data()).clone();
    } else {
        static constexpr std::array<unsigned char, 8> signature{137,80,78,71,13,10,26,10};
        require(encoded.size() >= 33 && std::equal(signature.begin(), signature.end(), encoded.begin()) &&
            be32(encoded.data() + 8) == 13 && be32(encoded.data() + 12) == 0x49484452,
            "Invalid PNG color texture.");
        const uint64_t width = be32(encoded.data() + 16), height = be32(encoded.data() + 20);
        require(width > 0 && height > 0 && width * height <= max_pixels, "Color textures above 16 megapixels are not yet editable.");
        result = cv::imdecode(encoded, cv::IMREAD_UNCHANGED);
    }
    require(!result.empty() && result.depth() == CV_8U && (result.channels() == 3 || result.channels() == 4),
        "Appearance editing requires 8-bit RGB or RGBA color textures.");
    return result;
}

struct Material {
    size_t image = 0, texture = 0, document_index = 0;
    int wrap_s = 10497, wrap_t = 10497;
    std::array<double, 2> offset{0,0}, scale{1,1};
    double rotation = 0;
    bool textured = false;
    std::array<double, 4> factor {1, 1, 1, 1};
};

size_t append_color_image(Document& doc, size_t prototype, const std::vector<unsigned char>& png) {
    require(png.size() + 3 <= max_bytes - doc.binary.size(), "The edited GLB exceeds 512 MB.");
    while (doc.binary.size() % 4) doc.binary.push_back(0);
    Json replacement = doc.json["images"].at(prototype);
    replacement["bufferView"] = doc.json["bufferViews"].size();
    replacement["mimeType"] = "image/png";
    doc.json["bufferViews"].push_back({{"buffer",0},{"byteOffset",doc.binary.size()},{"byteLength",png.size()}});
    doc.binary.insert(doc.binary.end(),png.begin(),png.end());
    const size_t index = doc.json["images"].size();
    doc.json["images"].push_back(std::move(replacement));
    return index;
}

// Bake a selected material's linear RGB multiplier into a private base-color
// image, preserving its alpha and all normal/other image references. Materials
// sharing the old texture can retain distinct multipliers and appearance.
void bake_texture_material_factors(Document& doc, std::vector<Material>& material,
    const TexturedMesh& mesh, const BeautyAppearanceOptions& options, const std::function<bool()>& canceled) {
    std::vector<bool> selected(material.size(), false);
    for (size_t face = 0; face < mesh.indices.size(); ++face) if (options.face_weights[face] > 0) {
        const int mi = mesh.material_ids[face];
        if (mi >= 0 && size_t(mi) < material.size() && material[mi].textured) selected[mi] = true;
    }
    auto linear = [](double value) { return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4); };
    auto srgb = [](double value) { return value <= .0031308 ? value * 12.92 : 1.055 * std::pow(value, 1 / 2.4) - .055; };
    for (size_t mi = 0; mi < material.size(); ++mi) {
        if (!selected[mi] || (std::abs(material[mi].factor[0] - 1) <= 1e-6 &&
            std::abs(material[mi].factor[1] - 1) <= 1e-6 && std::abs(material[mi].factor[2] - 1) <= 1e-6)) continue;
        checkpoint(canceled);
        cv::Mat pixels = decode_image(image_bytes(doc,material[mi].image));
        const size_t channels = pixels.channels();
        for (int y = 0; y < pixels.rows; ++y) {
            if ((y & 127) == 0) checkpoint(canceled);
            auto* row = pixels.ptr<unsigned char>(y);
            for (int x = 0; x < pixels.cols; ++x) for (size_t ch = 0; ch < 3; ++ch) {
                auto& value = row[size_t(x) * channels + 2 - ch];
                value = static_cast<unsigned char>(std::lround(255 * std::clamp(srgb(linear(value / 255.) * material[mi].factor[ch]), 0., 1.)));
            }
        }
        std::vector<unsigned char> png;
        require(cv::imencode(".png",pixels,png,{cv::IMWRITE_PNG_COMPRESSION,3}), "Cannot encode the baked material texture.");
        checkpoint(canceled);
        const size_t image = append_color_image(doc,material[mi].image,png);
        Json texture = doc.json["textures"].at(material[mi].texture);
        texture["source"] = image;
        const size_t texture_index = doc.json["textures"].size();
        doc.json["textures"].push_back(std::move(texture));
        Json replacement = doc.json["materials"].at(material[mi].document_index);
        auto factor = material[mi].factor; factor[0] = factor[1] = factor[2] = 1;
        replacement["pbrMetallicRoughness"]["baseColorFactor"] = factor;
        replacement["pbrMetallicRoughness"]["baseColorTexture"]["index"] = texture_index;
        const size_t material_index = doc.json["materials"].size();
        doc.json["materials"].push_back(std::move(replacement));
        for (auto& item : doc.json["meshes"]) for (auto& primitive : item["primitives"])
            if (primitive.contains("material") && integer(primitive,"material") == material[mi].document_index) primitive["material"] = material_index;
        material[mi].image = image; material[mi].texture = texture_index; material[mi].factor = factor;
        material[mi].document_index = material_index;
    }
}

std::vector<Material> materials(const Document& doc) {
    std::vector<Material> result;
    for (const auto& material : doc.json.value("materials", Json::array())) {
        Material value;
        value.document_index = result.size();
        const auto pbr = material.value("pbrMetallicRoughness", Json::object());
        value.factor = pbr.value("baseColorFactor", value.factor);
        for (double channel : value.factor) parameter(channel, 0, 1);
        if (pbr.contains("baseColorTexture")) {
            value.textured = true;
            const auto& info = pbr.at("baseColorTexture");
            const auto transform = info.value("extensions", Json::object()).value("KHR_texture_transform", Json::object());
            require(transform.value("texCoord", info.value("texCoord", 0)) == 0, "Appearance editing currently requires TEXCOORD_0.");
            value.texture = integer(info, "index");
            const auto& texture = doc.json.at("textures").at(value.texture);
            require(!texture.contains("extensions"), "Extended texture formats must be converted to PNG/JPEG before editing.");
            value.image = integer(texture, "source");
            require(value.image < doc.json.at("images").size(), "Invalid GLB color image reference.");
            if (texture.contains("sampler")) {
                const auto& sampler = doc.json.at("samplers").at(integer(texture, "sampler"));
                value.wrap_s = sampler.value("wrapS", 10497); value.wrap_t = sampler.value("wrapT", 10497);
            }
            for (int wrap : {value.wrap_s, value.wrap_t})
                require(wrap == 10497 || wrap == 33071 || wrap == 33648, "Unsupported GLB texture wrap mode.");
            value.offset = transform.value("offset", value.offset); value.scale = transform.value("scale", value.scale);
            value.rotation = transform.value("rotation", 0.0);
        }
        result.push_back(value);
    }
    return result;
}
using Point = std::array<double, 2>;
std::array<Point, 3> face_uvs(const TexturedMesh& mesh, size_t face, const Material& material, int width, int height) {
    std::array<Point, 3> triangle;
    for (size_t corner = 0; corner < 3; ++corner) {
        const int index = mesh.indices[face][corner];
        require(index >= 0 && size_t(index) < mesh.uvs.size(), "GLB texture coordinates are missing.");
        const auto& uv = mesh.uvs[index];
        const double u = uv[0] * material.scale[0], v = uv[1] * material.scale[1];
        const double x = material.offset[0] + std::cos(material.rotation) * u - std::sin(material.rotation) * v;
        const double y = material.offset[1] + std::sin(material.rotation) * u + std::cos(material.rotation) * v;
        require(std::isfinite(x) && std::isfinite(y) && x >= -1e-6 && x <= 1 + 1e-6 && y >= -1e-6 && y <= 1 + 1e-6,
            "Local appearance editing currently requires UVs within one texture tile; unwrap repeated UVs first.");
        triangle[corner] = {std::clamp(x, 0.0, 1.0) * width, std::clamp(y, 0.0, 1.0) * height};
    }
    return triangle;
}
struct TriangleCoverage {
    const std::array<Point, 3>& triangle;
    std::array<std::array<double, 3>, 3> axes;
    double min_x, max_x, min_y, max_y;

    explicit TriangleCoverage(const std::array<Point, 3>& value) : triangle(value),
        min_x(std::min({value[0][0],value[1][0],value[2][0]})),
        max_x(std::max({value[0][0],value[1][0],value[2][0]})),
        min_y(std::min({value[0][1],value[1][1],value[2][1]})),
        max_y(std::max({value[0][1],value[1][1],value[2][1]})) {
        for (size_t edge = 0; edge < axes.size(); ++edge) {
            const auto& a = triangle[edge]; const auto& b = triangle[(edge + 1) % 3];
            const double nx = b[1] - a[1], ny = a[0] - b[0];
            axes[edge] = {nx, ny, std::abs(nx) + std::abs(ny)};
        }
    }

    bool touches(double x, double y) const {
        // Keep bilinear coverage and the projection arithmetic unchanged;
        // only the per-triangle axes and bounds are reused across texels.
        for (const auto& axis : axes) {
            const double nx = axis[0], ny = axis[1], extent = axis[2];
            const double p0 = nx * (triangle[0][0] - x) + ny * (triangle[0][1] - y);
            const double p1 = nx * (triangle[1][0] - x) + ny * (triangle[1][1] - y);
            const double p2 = nx * (triangle[2][0] - x) + ny * (triangle[2][1] - y);
            if (std::min({p0,p1,p2}) > extent || std::max({p0,p1,p2}) < -extent) return false;
        }
        return x + 1 >= min_x && x - 1 <= max_x && y + 1 >= min_y && y - 1 <= max_y;
    }
};
int wrap_pixel(int position, int size, int wrap) {
    if (wrap == 10497) return (position % size + size) % size;
    return std::clamp(position, 0, size - 1); // Only the immediately adjacent tile edge can occur here.
}
struct RasterMask {
    std::vector<float> weights;
    // Only allocated for absolute colors: reference one face instead of storing
    // three floats per texel. -2 denotes conflicting requested colors.
    std::vector<int32_t> target_faces;
    std::vector<std::array<float,3>> targets;
};
RasterMask raster_mask(const TexturedMesh& mesh, const std::vector<Material>& material,
    size_t image, int width, int height, const BeautyAppearanceOptions& options, const std::function<bool()>& canceled,
    const std::vector<std::array<int,3>>& corner_map) {
    RasterMask mask;
    mask.weights.assign(size_t(width) * height, -1.f);
    const bool absolute = !options.face_target_colors.empty() || !options.leaves.empty() || !options.cells.empty();
    if (absolute) mask.target_faces.assign(mask.weights.size(), -1);
    mask.targets=options.face_target_colors;
    mask.targets.resize(mesh.indices.size());
    for (const auto& leaf : options.leaves) mask.targets.push_back(leaf.color);
    for (const auto& cell : options.cells) mask.targets.push_back(cell.color);
    size_t first_leaf=0;
    size_t first_cell=0;
    uint64_t visits = 0;
    for (size_t face = 0; face < mesh.indices.size(); ++face) {
        if ((face & 1023) == 0) checkpoint(canceled);
        const int mi = mesh.material_ids[face];
        if (mi < 0 || size_t(mi) >= material.size() || !material[mi].textured || material[mi].image != image) continue;
        auto root_triangle = face_uvs(mesh, face, material[mi], width, height);
        if (!corner_map.empty()) {
            const auto raw=root_triangle;
            for (size_t c=0;c<3;++c) root_triangle[c]=raw[corner_map[face][c]];
        }
        while (first_leaf<options.leaves.size() && options.leaves[first_leaf].key.source_face_id<face) ++first_leaf;
        const size_t first=first_leaf;
        while (first_leaf<options.leaves.size() && options.leaves[first_leaf].key.source_face_id==face) ++first_leaf;
        const size_t end=first_leaf;
        while(first_cell<options.cells.size() && options.cells[first_cell].source_face_id<face) ++first_cell;
        const size_t cell_begin=first_cell;
        while(first_cell<options.cells.size() && options.cells[first_cell].source_face_id==face) ++first_cell;
        const auto visit=[&](const std::array<std::array<double,2>,3>& triangle,float weight,size_t target) {
        const TriangleCoverage coverage(triangle);
        const int x0 = std::max(-1, int(std::floor(std::min({triangle[0][0],triangle[1][0],triangle[2][0]}) - 1.5)));
        const int x1 = std::min(width, int(std::ceil(std::max({triangle[0][0],triangle[1][0],triangle[2][0]}) + .5)));
        const int y0 = std::max(-1, int(std::floor(std::min({triangle[0][1],triangle[1][1],triangle[2][1]}) - 1.5)));
        const int y1 = std::min(height, int(std::ceil(std::max({triangle[0][1],triangle[1][1],triangle[2][1]}) + .5)));
        visits += uint64_t(x1 - x0 + 1) * (y1 - y0 + 1);
        require(visits <= 300000000, "The UV layout is too expensive for local editing; simplify or unwrap the texture first.");
        for (int y = y0; y <= y1; ++y) {
            if ((y & 127) == 0) checkpoint(canceled);
            for (int x = x0; x <= x1; ++x) if (coverage.touches(x + .5, y + .5)) {
                const size_t pixel = size_t(wrap_pixel(y, height, material[mi].wrap_t)) * width + wrap_pixel(x, width, material[mi].wrap_s);
                mask.weights[pixel] = mask.weights[pixel] < 0 ? weight : std::min(mask.weights[pixel], weight);
                if (absolute && weight > 0) {
                    auto& owner = mask.target_faces[pixel];
                    if (owner == -1) owner = int32_t(target);
                    else if (owner >= 0) {
                        const auto& a = mask.targets[size_t(owner)];
                        const auto& b = mask.targets[target];
                        for (size_t ch = 0; ch < 3; ++ch) if (std::abs(a[ch] - b[ch]) > 1e-6f) { owner = -2; break; }
                    }
                    if (owner == -2) mask.weights[pixel] = 0;
                }
            }
        }
        };
        if(cell_begin!=first_cell) {
            for(size_t i=cell_begin;i<first_cell;++i) for(const auto& corners:options.cells[i].triangles) {
                std::array<std::array<double,2>,3> triangle{};
                for(size_t c=0;c<3;++c) for(size_t k=0;k<3;++k) for(size_t axis=0;axis<2;++axis)
                    triangle[c][axis]+=corners[c][k]*root_triangle[k][axis];
                visit(triangle,options.cells[i].weight,mesh.indices.size()+options.leaves.size()+i);
            }
        }
        else if (first==end) visit(root_triangle,options.face_weights[face],face);
        else for (size_t i=first;i<end;++i) {
            std::array<std::array<double,2>,3> triangle{};
            const auto corners=options.leaves[i].key.barycentric();
            for (size_t c=0;c<3;++c) for (size_t k=0;k<3;++k) for (size_t axis=0;axis<2;++axis)
                triangle[c][axis]+=corners[c][k]*root_triangle[k][axis];
            visit(triangle,options.leaves[i].weight,mesh.indices.size()+i);
        }
    }
    return mask;
}

std::array<double, 3> adjust(std::array<double, 3> rgb, const BeautyAppearanceOptions& options) {
    const double high = std::max({rgb[0],rgb[1],rgb[2]}), low = std::min({rgb[0],rgb[1],rgb[2]}), delta = high - low;
    double hue = 0;
    if (delta > 1e-9) {
        if (high == rgb[0]) hue = 60 * std::fmod((rgb[1] - rgb[2]) / delta, 6.0);
        else if (high == rgb[1]) hue = 60 * ((rgb[2] - rgb[0]) / delta + 2);
        else hue = 60 * ((rgb[0] - rgb[1]) / delta + 4);
    }
    hue = std::fmod(hue + options.hue_degrees + 720, 360.0);
    const double saturation = std::clamp((high > 1e-9 ? delta / high : 0) * options.saturation, 0.0, 1.0);
    const double value = std::clamp(high + options.brightness, 0.0, 1.0), chroma = value * saturation;
    const double x = chroma * (1 - std::abs(std::fmod(hue / 60, 2.0) - 1)), m = value - chroma;
    if (hue < 60) rgb = {chroma,x,0}; else if (hue < 120) rgb = {x,chroma,0};
    else if (hue < 180) rgb = {0,chroma,x}; else if (hue < 240) rgb = {0,x,chroma};
    else if (hue < 300) rgb = {x,0,chroma}; else rgb = {chroma,0,x};
    for (double& channel : rgb) channel += m;
    return rgb;
}
size_t edit_pixels(cv::Mat& pixels, const RasterMask& raster, const BeautyAppearanceOptions& options,
    const std::function<bool()>& canceled) {
    const auto& mask = raster.weights;
    const bool absolute = !options.face_target_colors.empty() || !options.leaves.empty() || !options.cells.empty();
    const cv::Mat original = pixels.clone();
    const int channels = pixels.channels(), width = pixels.cols, height = pixels.rows;
    size_t changed = 0;
    const double sigma = .035 + .22 * (1 - options.detail_preserve);
    for (int y = 0; y < height; ++y) {
        checkpoint(canceled);
        auto* output = pixels.ptr<unsigned char>(y);
        for (int x = 0; x < width; ++x) {
            const float weight = mask[size_t(y) * width + x];
            if (weight <= 0) continue;
            const auto* source = original.ptr<unsigned char>(y) + x * channels;
            std::array<double,3> color{source[2]/255.0,source[1]/255.0,source[0]/255.0}, filtered = color;
            if (!absolute && (options.denoise > 0 || options.soften > 0)) {
                std::array<std::array<double,9>,3> samples{};
                size_t count = 0;
                std::array<double,3> sum{0,0,0}; double total = 0;
                for (int yy = std::max(0,y-2); yy <= std::min(height-1,y+2); ++yy)
                    for (int xx = std::max(0,x-2); xx <= std::min(width-1,x+2); ++xx) {
                        const float neighbor_weight = mask[size_t(yy) * width + xx];
                        if (neighbor_weight <= 0) continue;
                        const auto* neighbor = original.ptr<unsigned char>(yy) + xx * channels;
                        std::array<double,3> c{neighbor[2]/255.0,neighbor[1]/255.0,neighbor[0]/255.0};
                        double distance = 0;
                        for (size_t channel = 0; channel < 3; ++channel) distance += std::pow(c[channel]-color[channel], 2);
                        const double w = neighbor_weight * std::exp(-distance / (2*sigma*sigma) - ((xx-x)*(xx-x)+(yy-y)*(yy-y))/4.0);
                        for (size_t channel = 0; channel < 3; ++channel) sum[channel] += c[channel]*w;
                        total += w;
                        if (std::abs(xx-x) <= 1 && std::abs(yy-y) <= 1) {
                            for (size_t channel = 0; channel < 3; ++channel) samples[channel][count] = c[channel];
                            ++count;
                        }
                    }
                for (size_t channel = 0; channel < 3; ++channel) {
                    if (count) {
                        std::sort(samples[channel].begin(), samples[channel].begin()+count);
                        const double median = samples[channel][count/2];
                        const double preserve = 1 - .65 * options.detail_preserve;
                        filtered[channel] += options.denoise * preserve * (median-filtered[channel]);
                    }
                    if (total > 0) filtered[channel] += options.soften * (sum[channel]/total-filtered[channel]);
                }
            }
            if (absolute) {
                const int32_t face = raster.target_faces[size_t(y) * width + x];
                require(face >= 0, "Missing target color for an editable texture pixel.");
                const auto& target = raster.targets[size_t(face)];
                for (size_t ch = 0; ch < 3; ++ch) filtered[ch] = target[ch];
            } else filtered = adjust(filtered, options);
            bool different = false;
            for (size_t channel = 0; channel < 3; ++channel) {
                double blended = color[channel] + weight * (filtered[channel] - color[channel]);
                if (absolute) {
                    // Absolute RGB follows the same linear blend as native
                    // vertex-color painting; relative appearance filters keep
                    // their existing display-space behavior.
                    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
                    const double v = linear(color[channel]) + weight * (linear(filtered[channel]) - linear(color[channel]));
                    blended = v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - .055;
                }
                const auto value = static_cast<unsigned char>(std::lround(255 * std::clamp(blended, 0.0, 1.0)));
                output[x*channels+2-channel] = value;
                different = different || value != source[2-channel];
            }
            if (different) ++changed;
        }
    }
    return changed;
}

void write_glb(Document& doc, const boost::filesystem::path& source, const boost::filesystem::path& destination,
    BeautyAppearanceResult& result, const std::function<bool()>& canceled) {
    doc.json["buffers"][0]["byteLength"] = doc.binary.size();
    std::string description = doc.json.dump();
    while (description.size() % 4) description += ' ';
    while (doc.binary.size() % 4) doc.binary.push_back(0);
    const uint64_t size = 28 + description.size() + doc.binary.size();
    require(size <= max_bytes && description.size() <= 32 * 1024 * 1024, "The edited GLB exceeds the supported size.");
    auto staging = destination.parent_path() / boost::filesystem::unique_path(".beauty-%%%%-%%%%-%%%%");
    require(boost::filesystem::create_directory(staging), "Cannot create a new GLB staging directory.");
    const auto temporary = staging / "appearance.glb";
    bool published = false;
    try {
        checkpoint(canceled);
        boost::filesystem::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        require(bool(output), "Cannot write the edited GLB.");
        std::vector<unsigned char> header;
        append32(header,0x46546c67); append32(header,2); append32(header,uint32_t(size));
        append32(header,uint32_t(description.size())); append32(header,0x4e4f534a);
        output.write(reinterpret_cast<const char*>(header.data()),header.size());
        output.write(description.data(),description.size());
        header.clear(); append32(header,uint32_t(doc.binary.size())); append32(header,0x004e4942);
        output.write(reinterpret_cast<const char*>(header.data()),header.size());
        for (size_t offset = 0; offset < doc.binary.size(); offset += 65536) {
            checkpoint(canceled);
            output.write(reinterpret_cast<const char*>(doc.binary.data()+offset),std::min(size_t(65536),doc.binary.size()-offset));
        }
        output.close();
        require(bool(output) && boost::filesystem::file_size(temporary) == size, "The edited GLB was not written completely.");
        checkpoint(canceled);
        require(model_artifact_sha256(source) == result.source_sha256, "The source GLB changed during editing; reload it first.");
        result.output_sha256 = model_artifact_sha256(temporary);
        require(!result.output_sha256.empty(), "Cannot verify the edited GLB.");
        checkpoint(canceled);
        // Hard-link publication fails if another writer won the destination;
        // unlike rename on POSIX it never overwrites an existing asset.
        boost::filesystem::create_hard_link(temporary,destination);
        published = true;
        checkpoint(canceled);
        require(model_artifact_sha256(source) == result.source_sha256, "The source GLB changed during editing; reload it first.");
        boost::filesystem::remove(temporary);
        boost::filesystem::remove(staging);
    } catch (...) {
        boost::system::error_code ignored;
        if (published) boost::filesystem::remove(destination,ignored);
        boost::filesystem::remove(temporary,ignored);
        boost::filesystem::remove(staging,ignored);
        throw;
    }
}
std::array<size_t, 3> dense_corner_accessor(const Document& doc, size_t index, const char* type, size_t width, int component, bool normalized = false) {
    const auto& accessor = doc.json.at("accessors").at(index);
    require(!accessor.contains("sparse") && accessor.value("componentType", 0) == component &&
        accessor.value("type", "") == type && accessor.value("normalized", false) == normalized && !accessor.contains("extensions"),
        "区域改色的顶点数据不完整或格式不受支持；草稿已保留。");
    const auto& view = doc.json.at("bufferViews").at(integer(accessor, "bufferView"));
    require(integer(view, "buffer") == 0, "Invalid vertex color buffer.");
    const size_t offset = view.value("byteOffset", size_t(0)), local = accessor.value("byteOffset", size_t(0));
    const size_t length = integer(view, "byteLength"), count = integer(accessor, "count");
    const size_t stride = view.value("byteStride", width);
    const size_t component_width = component == 5121 ? 1 : component == 5123 ? 2 : 4;
    require(count > 0 && count <= 6000000 && stride >= width && stride <= 252 && stride % component_width == 0 &&
        (offset + local) % component_width == 0 && !view.contains("extensions") &&
        offset <= doc.binary.size() && length <= doc.binary.size() - offset && local <= length &&
        width <= length - local && count - 1 <= (length - local - width) / stride,
        "Invalid vertex color accessor bounds.");
    return {offset + local, stride, count};
}

// Preserve independent corners as-is. For shared vertices, duplicate attributes
// by the verified source index sequence so neighboring faces can have different
// colors without changing their ordered positions, normals or UVs.
void edit_corner_colors(Document& doc,
    const TexturedMesh& mesh, const std::vector<Material>& material,
    const BeautyAppearanceOptions& options, BeautyAppearanceResult& result,
    const std::function<bool()>& canceled) {
    indexed_triangle_set geometry;
    geometry.vertices.reserve(mesh.vertices.size()); geometry.indices.reserve(mesh.indices.size());
    for (const auto& v : mesh.vertices) geometry.vertices.emplace_back(v[0] * 1000.f, -v[2] * 1000.f, v[1] * 1000.f);
    for (const auto& f : mesh.indices) geometry.indices.emplace_back(f[0], f[1], f[2]);
    const auto face_counts = verify_glb_appearance_layout(doc.json, doc.binary, geometry, mesh.material_ids,
        [&] { checkpoint(canceled); });
    size_t face_offset = 0;
    for (size_t pi = 0; pi < face_counts.size(); ++pi) {
        const size_t first_face = face_offset;
        face_offset += face_counts[pi];
        auto& primitive = doc.json["meshes"][0]["primitives"][pi];
        const size_t mi = integer(primitive, "material");
        require(mi < material.size(), "Invalid appearance material.");
        if (material[mi].textured || !std::any_of(options.face_weights.begin() + first_face,
            options.face_weights.begin() + face_offset, [](float weight) { return weight > 0; })) continue;
        const auto attributes = primitive.at("attributes");
        const size_t vertex_count = integer(doc.json["accessors"].at(integer(attributes, "POSITION")), "count");
        std::vector<size_t> corners;
        if (primitive.contains("indices")) {
            const size_t index = integer(primitive, "indices");
            const int component = doc.json["accessors"].at(index).value("componentType", 0);
            require(component == 5121 || component == 5123 || component == 5125, "Invalid triangle index type.");
            const size_t width = component == 5121 ? 1 : component == 5123 ? 2 : 4;
            const auto indices = dense_corner_accessor(doc, index, "SCALAR", width, component);
            corners.resize(indices[2]);
            for (size_t i = 0; i < corners.size(); ++i) {
                if ((i & 4095) == 0) checkpoint(canceled);
                const auto* bytes = doc.binary.data() + indices[0] + i * indices[1];
                corners[i] = width == 1 ? bytes[0] : width == 2 ? size_t(bytes[0]) | size_t(bytes[1]) << 8 : u32(bytes);
                require(corners[i] < vertex_count, "Invalid triangle vertex index.");
            }
        } else {
            corners.resize(vertex_count);
            for (size_t i = 0; i < corners.size(); ++i) corners[i] = i;
        }
        require(corners.size() == face_counts[pi] * 3, "Vertex colors do not match the triangles.");
        bool independent = corners.size() == vertex_count;
        for (size_t i = 0; i < corners.size(); ++i) independent = independent && corners[i] == i;
        size_t channels = 4, component_width = 4;
        int color_component = 5126;
        std::array<size_t, 3> colors {};
        if (attributes.contains("COLOR_0")) {
            const size_t old_color = integer(attributes, "COLOR_0");
            const auto& accessor = doc.json["accessors"].at(old_color);
            channels = accessor.value("type", "") == "VEC3" ? 3 : 4;
            color_component = accessor.value("componentType", 0);
            require(color_component == 5126 || color_component == 5121 || color_component == 5123, "Invalid vertex color type.");
            component_width = color_component == 5121 ? 1 : color_component == 5123 ? 2 : 4;
            colors = dense_corner_accessor(doc, old_color, channels == 3 ? "VEC3" : "VEC4", channels * component_width,
                                          color_component, color_component != 5126);
            require(colors[2] == vertex_count, "Vertex colors do not match the positions.");
        }
        auto append_accessor = [&](Json accessor, const std::vector<unsigned char>& bytes) {
            require(bytes.size() + 3 <= max_bytes - doc.binary.size(), "The edited GLB exceeds 512 MB.");
            while (doc.binary.size() % 4) doc.binary.push_back(0);
            const size_t index = doc.json["accessors"].size();
            accessor["bufferView"] = doc.json["bufferViews"].size();
            accessor.erase("byteOffset"); accessor["count"] = corners.size();
            doc.json["bufferViews"].push_back({{"buffer",0},{"byteOffset",doc.binary.size()},{"byteLength",bytes.size()}});
            doc.binary.insert(doc.binary.end(),bytes.begin(),bytes.end());
            doc.json["accessors"].push_back(std::move(accessor));
            return index;
        };
        std::vector<unsigned char> replacement;
        require(corners.size() * 16 + 3 <= max_bytes - doc.binary.size(), "The edited GLB exceeds 512 MB.");
        replacement.reserve(corners.size() * 16);
        auto linear = [](float value) { return value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f); };
        for (size_t vertex = 0; vertex < corners.size(); ++vertex) {
            if ((vertex & 4095) == 0) checkpoint(canceled);
            std::array<float, 4> original {1, 1, 1, 1};
            if (colors[2]) {
                const auto* bytes = doc.binary.data() + colors[0] + corners[vertex] * colors[1];
                for (size_t ch = 0; ch < channels; ++ch) {
                    if (color_component == 5126) std::memcpy(&original[ch], bytes + ch * 4, 4);
                    else if (color_component == 5121) original[ch] = float(bytes[ch]) / 255.f;
                    else original[ch] = float(uint32_t(bytes[ch * 2]) | uint32_t(bytes[ch * 2 + 1]) << 8) / 65535.f;
                }
            }
            for (float value : original) parameter(value, 0, 1);
            const size_t face = first_face + vertex / 3;
            for (size_t ch = 0; ch < 4; ++ch) {
                float value = original[ch];
                if (ch < 3) {
                    // Bake the old material multiplier into every unpainted corner,
                    // allowing absolute colors brighter than that multiplier.
                    value *= float(material[mi].factor[ch]);
                    const float weight = options.face_weights[face];
                    value += weight * (linear(options.face_target_colors[face][ch]) - value);
                    if (weight > 0 && ch == 0) ++result.changed_vertices;
                }
                uint32_t bits; std::memcpy(&bits, &value, 4); append32(replacement, bits);
            }
        }
        primitive["attributes"]["COLOR_0"] = append_accessor({{"componentType",5126},{"type","VEC4"}}, replacement);
        if (!independent) {
            for (auto it = attributes.begin(); it != attributes.end(); ++it) {
                if (it.key() == "COLOR_0") continue;
                Json accessor = doc.json["accessors"].at(it.value().get<size_t>());
                const int component = accessor.value("componentType", 0);
                require(component == 5126 || component == 5121 || component == 5123, "Unsupported shared vertex attribute.");
                const size_t width = component == 5121 ? 1 : component == 5123 ? 2 : 4;
                const std::string type = accessor.value("type", "");
                require(type == "VEC2" || type == "VEC3", "Unsupported shared vertex attribute shape.");
                const size_t element = width * (type == "VEC2" ? 2 : 3);
                const auto layout = dense_corner_accessor(doc, it.value().get<size_t>(), type.c_str(), element, component, accessor.value("normalized",false));
                require(layout[2] == vertex_count, "Shared vertex attributes do not match positions.");
                std::vector<unsigned char> bytes;
                require(corners.size() * element + 3 <= max_bytes - doc.binary.size(), "The edited GLB exceeds 512 MB.");
                bytes.reserve(corners.size() * element);
                for (size_t i = 0; i < corners.size(); ++i) {
                    if ((i & 4095) == 0) checkpoint(canceled);
                    const auto* value = doc.binary.data() + layout[0] + corners[i] * layout[1];
                    bytes.insert(bytes.end(), value, value + element);
                }
                if (component == 5126 && (accessor.contains("min") || accessor.contains("max"))) {
                    std::vector<float> low(element / 4, std::numeric_limits<float>::max()), high(low.size(), std::numeric_limits<float>::lowest());
                    for (size_t i = 0; i < corners.size(); ++i) for (size_t c = 0; c < low.size(); ++c) {
                        float value; std::memcpy(&value, bytes.data() + i * element + c * 4, 4);
                        require(std::isfinite(value), "Invalid shared vertex attribute value.");
                        low[c] = std::min(low[c],value); high[c] = std::max(high[c],value);
                    }
                    accessor["min"] = low; accessor["max"] = high;
                }
                primitive["attributes"][it.key()] = append_accessor(std::move(accessor),bytes);
            }
            std::vector<unsigned char> indices;
            indices.reserve(corners.size() * 4);
            for (size_t i = 0; i < corners.size(); ++i) { if ((i & 4095) == 0) checkpoint(canceled); append32(indices,uint32_t(i)); }
            primitive["indices"] = append_accessor({{"componentType",5125},{"type","SCALAR"}},indices);
        }
        auto factor = material[mi].factor; factor[0] = factor[1] = factor[2] = 1;
        // A primitive may share its material with an untouched primitive. Clone it
        // so baking this primitive's RGB multiplier cannot alter its neighbor.
        Json replacement_material = doc.json["materials"][mi];
        replacement_material["pbrMetallicRoughness"]["baseColorFactor"] = factor;
        primitive["material"] = doc.json["materials"].size();
        doc.json["materials"].push_back(std::move(replacement_material));
    }
}

// Fold a native linear RGB gradient into a private color image for one
// selected face run. Unselected runs retain their original accessor/material.
// Texel centers use barycentric interpolation; edge padding uses the closest
// triangle point. This is an 8-bit bake, not an exact replacement at all scales.
Material bake_gradient_texture_faces(Document& doc, const TexturedMesh& mesh,
    const Material& source, const std::vector<std::array<float, 4>>& colors,
    size_t first, size_t last, const std::function<bool()>& canceled) {
    cv::Mat pixels = decode_image(image_bytes(doc, source.image));
    const size_t count = size_t(pixels.cols) * pixels.rows;
    std::vector<std::array<float, 3>> factors(count);
    std::vector<float> distances(count, std::numeric_limits<float>::infinity());
    uint64_t visits = 0;
    for (size_t face = first; face < last; ++face) {
        checkpoint(canceled);
        const auto triangle = face_uvs(mesh, face, source, pixels.cols, pixels.rows);
        const TriangleCoverage coverage(triangle);
        const auto& a = triangle[0]; const auto& b = triangle[1]; const auto& c = triangle[2];
        const double area = (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1]);
        require(std::abs(area) > 1e-10,
            "这块贴图的渐变颜色使用退化UV，无法可靠保存；草稿已保留。");
        const int x0 = std::max(-1, int(std::floor(std::min({a[0],b[0],c[0]}) - 1.5)));
        const int x1 = std::min(pixels.cols, int(std::ceil(std::max({a[0],b[0],c[0]}) + .5)));
        const int y0 = std::max(-1, int(std::floor(std::min({a[1],b[1],c[1]}) - 1.5)));
        const int y1 = std::min(pixels.rows, int(std::ceil(std::max({a[1],b[1],c[1]}) + .5)));
        visits += uint64_t(x1-x0+1)*(y1-y0+1);
        require(visits <= 300000000, "The gradient UV layout is too expensive for local editing.");
        for (int y = y0; y <= y1; ++y) {
            if ((y & 127) == 0) checkpoint(canceled);
            for (int x = x0; x <= x1; ++x) {
                const double px = x + .5, py = y + .5;
                if (!coverage.touches(px, py)) continue;
                std::array<double, 3> weights {
                    ((b[1]-c[1])*(px-c[0]) + (c[0]-b[0])*(py-c[1])) / area,
                    ((c[1]-a[1])*(px-c[0]) + (a[0]-c[0])*(py-c[1])) / area, 0};
                weights[2] = 1 - weights[0] - weights[1];
                double distance = 0;
                if (*std::min_element(weights.begin(),weights.end()) < -1e-8) {
                    distance = std::numeric_limits<double>::infinity();
                    for (size_t edge = 0; edge < 3; ++edge) {
                        const size_t next = (edge + 1) % 3;
                        const double dx = triangle[next][0]-triangle[edge][0], dy = triangle[next][1]-triangle[edge][1];
                        const double length = dx*dx+dy*dy;
                        const double t = length > 0 ? std::clamp(((px-triangle[edge][0])*dx+(py-triangle[edge][1])*dy)/length,0.,1.) : 0;
                        const double ex = px-triangle[edge][0]-t*dx, ey = py-triangle[edge][1]-t*dy;
                        if (ex*ex+ey*ey < distance) {
                            distance = ex*ex+ey*ey;
                            weights = {0,0,0}; weights[edge] = 1-t; weights[next] = t;
                        }
                    }
                }
                std::array<float,3> factor {0,0,0};
                for (size_t ch = 0; ch < 3; ++ch) for (size_t corner = 0; corner < 3; ++corner)
                    factor[ch] += float(weights[corner]*colors[mesh.indices[face][corner]][ch]);
                const size_t pixel = size_t(wrap_pixel(y,pixels.rows,source.wrap_t))*pixels.cols + wrap_pixel(x,pixels.cols,source.wrap_s);
                if (distance == 0 && distances[pixel] == 0) for (size_t ch = 0; ch < 3; ++ch)
                    require(std::abs(factor[ch]-factors[pixel][ch]) <= 1e-5f,
                        "这块贴图的重叠UV含不同渐变颜色，无法可靠保存；草稿已保留。");
                if (distance < distances[pixel]) { distances[pixel] = float(distance); factors[pixel] = factor; }
            }
        }
    }
    auto linear = [](double v) { return v <= .04045 ? v/12.92 : std::pow((v+.055)/1.055,2.4); };
    auto srgb = [](double v) { return v <= .0031308 ? v*12.92 : 1.055*std::pow(v,1/2.4)-.055; };
    for (int y = 0; y < pixels.rows; ++y) {
        if ((y & 127) == 0) checkpoint(canceled);
        for (int x = 0; x < pixels.cols; ++x) {
            const size_t index = size_t(y)*pixels.cols+x;
            if (!std::isfinite(distances[index])) continue;
            auto* pixel = pixels.ptr<unsigned char>(y)+size_t(x)*pixels.channels();
            for (size_t ch = 0; ch < 3; ++ch)
                pixel[2-ch] = static_cast<unsigned char>(std::lround(255*std::clamp(
                    srgb(linear(pixel[2-ch]/255.)*source.factor[ch]*std::clamp(double(factors[index][ch]),0.,1.)),0.,1.)));
        }
    }
    std::vector<unsigned char> png;
    require(cv::imencode(".png",pixels,png,{cv::IMWRITE_PNG_COMPRESSION,3}), "Cannot encode the baked gradient texture.");
    checkpoint(canceled);
    Material baked = source; baked.image = append_color_image(doc,source.image,png);
    Json texture = doc.json["textures"].at(source.texture); texture["source"] = baked.image;
    baked.texture = doc.json["textures"].size(); doc.json["textures"].push_back(std::move(texture));
    Json replacement = doc.json["materials"].at(source.document_index);
    baked.factor[0] = baked.factor[1] = baked.factor[2] = 1;
    replacement["pbrMetallicRoughness"]["baseColorFactor"] = baked.factor;
    replacement["pbrMetallicRoughness"]["baseColorTexture"]["index"] = baked.texture;
    baked.document_index = doc.json["materials"].size(); doc.json["materials"].push_back(std::move(replacement));
    return baked;
}

// A constant native vertex RGB factor can be folded into a private texture
// without changing interpolation or UVs. Clone per primitive so another user
// of the same material/image keeps its original appearance. Vertex alpha is
// retained in the native accessor rather than baked into the color image.
void bake_texture_vertex_factors(Document& doc, TexturedMesh& mesh,
    std::vector<Material>& material, std::vector<std::array<float, 4>>& raw_colors,
    const std::vector<size_t>& face_counts, const BeautyAppearanceOptions& options,
    const std::function<bool()>& canceled) {
    require(raw_colors.size() == mesh.vertices.size(), "Cannot verify the GLB vertex color multipliers.");
    const auto original_primitives = doc.json["meshes"][0]["primitives"];
    Json replacements = Json::array();
    const auto original_colors = raw_colors;
    size_t first_face = 0;
    for (size_t pi = 0; pi < face_counts.size(); ++pi) {
        const size_t last_face = first_face + face_counts[pi];
        auto primitive = original_primitives[pi];
        const size_t mi = integer(primitive, "material");
        require(mi < material.size(), "Invalid appearance material.");
        bool selected_non_neutral = false;
        if (material[mi].textured) for (size_t f = first_face; f < last_face; ++f)
            if (options.face_weights[f] > 0) for (int32_t v : mesh.indices[f]) for (size_t ch = 0; ch < 3; ++ch)
                selected_non_neutral = selected_non_neutral || std::abs(raw_colors[v][ch] - 1.f) > 1e-6f;
        if (!selected_non_neutral) { replacements.push_back(std::move(primitive)); first_face = last_face; continue; }
        checkpoint(canceled);
        const auto multiplier = raw_colors[mesh.indices[first_face][0]];
        bool gradient = false;
        for (size_t f = first_face; f < last_face; ++f) for (int32_t v : mesh.indices[f]) for (size_t ch = 0; ch < 3; ++ch) {
            parameter(original_colors[v][ch], 0, 1);
            gradient = gradient || std::abs(original_colors[v][ch] - multiplier[ch]) > 1e-6f;
        }
        const auto& attributes = primitive.at("attributes");
        require(attributes.contains("COLOR_0"), "Cannot verify the native vertex color accessor.");
        const size_t color_index = integer(attributes, "COLOR_0");
        const auto accessor = doc.json["accessors"].at(color_index);
        const size_t channels = accessor.value("type", "") == "VEC3" ? 3 : 4;
        const int component = accessor.value("componentType", 0);
        const size_t width = component == 5121 ? 1 : component == 5123 ? 2 : 4;
        const auto colors = dense_corner_accessor(doc, color_index, channels == 3 ? "VEC3" : "VEC4",
            channels * width, component, component != 5126);
        require(colors[2] * 16 + 3 <= max_bytes - doc.binary.size(), "The edited GLB exceeds 512 MB.");
        std::vector<unsigned char> replacement_colors; replacement_colors.reserve(colors[2] * 16);
        for (size_t v = 0; v < colors[2]; ++v) {
            if ((v & 4095) == 0) checkpoint(canceled);
            std::array<float, 4> rgba {1, 1, 1, 1};
            const auto* bytes = doc.binary.data() + colors[0] + v * colors[1];
            if (channels == 4) {
                if (component == 5126) std::memcpy(&rgba[3], bytes + 12, 4);
                else if (component == 5121) rgba[3] = float(bytes[3]) / 255.f;
                else rgba[3] = float(uint32_t(bytes[6]) | uint32_t(bytes[7]) << 8) / 65535.f;
            }
            parameter(rgba[3], 0, 1);
            const auto* begin = reinterpret_cast<const unsigned char*>(rgba.data());
            replacement_colors.insert(replacement_colors.end(), begin, begin + 16);
        }
        while (doc.binary.size() % 4) doc.binary.push_back(0);
        require(replacement_colors.size() <= max_bytes - doc.binary.size(), "The edited GLB exceeds 512 MB.");
        const size_t view = doc.json["bufferViews"].size(), new_color = doc.json["accessors"].size();
        doc.json["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", doc.binary.size()}, {"byteLength", replacement_colors.size()}});
        doc.binary.insert(doc.binary.end(), replacement_colors.begin(), replacement_colors.end());
        doc.json["accessors"].push_back({{"bufferView", view}, {"componentType", 5126}, {"count", colors[2]}, {"type", "VEC4"}});
        if (gradient) {
            // Only selected face runs use the baked image and white vertex RGB.
            // Original native indices/UVs are sliced in order, never reprojected.
            std::vector<uint32_t> native_indices;
            native_indices.reserve(face_counts[pi]*3);
            if (primitive.contains("indices")) {
                const size_t index = integer(primitive,"indices");
                const int component = doc.json["accessors"].at(index).value("componentType",0);
                require(component == 5121 || component == 5123 || component == 5125, "Invalid native triangle index type.");
                const size_t width = component == 5121 ? 1 : component == 5123 ? 2 : 4;
                const auto layout = dense_corner_accessor(doc,index,"SCALAR",width,component);
                require(layout[2] == face_counts[pi]*3, "Native triangle indices no longer match the selection.");
                for (size_t i = 0; i < layout[2]; ++i) {
                    const auto* data = doc.binary.data()+layout[0]+i*layout[1];
                    native_indices.push_back(width == 1 ? data[0] : width == 2 ? uint32_t(data[0])|uint32_t(data[1])<<8 : u32(data));
                }
            } else for (size_t i = 0; i < face_counts[pi]*3; ++i) native_indices.push_back(uint32_t(i));
            for (size_t begin = first_face; begin < last_face;) {
                const bool selected = options.face_weights[begin] > 0;
                size_t end = begin+1;
                while (end < last_face && (options.face_weights[end] > 0) == selected) ++end;
                Json run = primitive;
                if (begin != first_face || end != last_face) {
                    std::vector<unsigned char> indices;
                    for (size_t i = (begin-first_face)*3; i < (end-first_face)*3; ++i) append32(indices,native_indices[i]);
                    require(indices.size()+3 <= max_bytes-doc.binary.size(), "The edited GLB exceeds 512 MB.");
                    while (doc.binary.size()%4) doc.binary.push_back(0);
                    const size_t view = doc.json["bufferViews"].size(), accessor = doc.json["accessors"].size();
                    doc.json["bufferViews"].push_back({{"buffer",0},{"byteOffset",doc.binary.size()},{"byteLength",indices.size()}});
                    doc.binary.insert(doc.binary.end(),indices.begin(),indices.end());
                    doc.json["accessors"].push_back({{"bufferView",view},{"componentType",5125},{"count",(end-begin)*3},{"type","SCALAR"}});
                    run["indices"] = accessor;
                }
                if (selected) {
                    auto baked = bake_gradient_texture_faces(doc,mesh,material[mi],original_colors,begin,end,canceled);
                    require(baked.document_index == material.size(), "Cannot verify the private gradient material mapping.");
                    material.push_back(baked);
                    run["attributes"]["COLOR_0"] = new_color; run["material"] = baked.document_index;
                    for (size_t face = begin; face < end; ++face) {
                        mesh.material_ids[face] = int(baked.document_index);
                        for (int32_t v : mesh.indices[face]) raw_colors[v][0] = raw_colors[v][1] = raw_colors[v][2] = 1;
                    }
                }
                replacements.push_back(std::move(run)); begin = end;
            }
            first_face = last_face; continue;
        }
        cv::Mat pixels = decode_image(image_bytes(doc, material[mi].image));
        auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
        auto srgb = [](double v) { return v <= .0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - .055; };
        for (int y = 0; y < pixels.rows; ++y) {
            if ((y & 127) == 0) checkpoint(canceled);
            auto* row = pixels.ptr<unsigned char>(y);
            for (int x = 0; x < pixels.cols; ++x) for (size_t ch = 0; ch < 3; ++ch) {
                auto& value = row[size_t(x) * pixels.channels() + 2 - ch];
                value = static_cast<unsigned char>(std::lround(255 * std::clamp(
                    srgb(linear(value / 255.) * material[mi].factor[ch] * multiplier[ch]), 0., 1.)));
            }
        }
        std::vector<unsigned char> png;
        require(cv::imencode(".png", pixels, png, {cv::IMWRITE_PNG_COMPRESSION, 3}), "Cannot encode the baked vertex color texture.");
        Material baked = material[mi]; baked.image = append_color_image(doc, baked.image, png);
        Json texture = doc.json["textures"].at(baked.texture); texture["source"] = baked.image;
        baked.texture = doc.json["textures"].size(); doc.json["textures"].push_back(std::move(texture));
        Json replacement = doc.json["materials"].at(baked.document_index);
        baked.factor[0] = baked.factor[1] = baked.factor[2] = 1;
        replacement["pbrMetallicRoughness"]["baseColorFactor"] = baked.factor;
        replacement["pbrMetallicRoughness"]["baseColorTexture"]["index"] = baked.texture;
        baked.document_index = doc.json["materials"].size();
        require(baked.document_index == material.size(), "Cannot verify the private appearance material mapping.");
        doc.json["materials"].push_back(std::move(replacement)); material.push_back(baked);
        primitive["attributes"]["COLOR_0"] = new_color; primitive["material"] = baked.document_index;
        for (size_t f = first_face; f < last_face; ++f) {
            mesh.material_ids[f] = int(baked.document_index);
            for (int32_t v : mesh.indices[f]) raw_colors[v][0] = raw_colors[v][1] = raw_colors[v][2] = 1;
        }
        replacements.push_back(std::move(primitive));
        first_face = last_face;
    }
    doc.json["meshes"][0]["primitives"] = std::move(replacements);
}

} // namespace

void appearance_cell_colors(BeautyAppearanceOptions& options,const nlohmann::json& partition,
    const std::map<std::string,std::array<float,3>>& colors,const std::string& geometry) {
    require(partition.at("geometry_id")==geometry && partition.at("face_count")==options.face_weights.size(),
            "Contour appearance source changed.");
    std::set<size_t> roots;
    std::set<std::string> ids;
    std::vector<BeautyAppearanceOptions::Cell> cells;
    for(const auto& face:partition.at("faces")) {
        const size_t root=face.at("source_face_id"); roots.insert(root);
        for(const auto& item:face.at("cells")) {
            BeautyAppearanceOptions::Cell cell;cell.source_face_id=root;cell.id=item.at("id");ids.insert(cell.id);
            for(const auto& triangle:item.at("triangles")) {
                std::array<Vec3d,3> corners;
                for(size_t i=0;i<3;++i) for(size_t k=0;k<3;++k) corners[i][k]=triangle[i][k].get<double>();
                cell.triangles.push_back(corners);
            }
            const auto color=colors.find(cell.id);
            if(color!=colors.end()) {cell.weight=1;cell.color=color->second;}
            else if(options.face_weights.at(root)>0) {
                cell.weight=options.face_weights[root];cell.color=options.face_target_colors.at(root);
            }
            cells.push_back(std::move(cell));
        }
    }
    for(const auto& entry:colors) require(ids.count(entry.first)!=0 || entry.first.compare(0,7,"source:")==0,
        "Appearance color leaves its contour mapping.");
    for(auto root:roots) options.face_weights[root]=0;
    options.leaves.erase(std::remove_if(options.leaves.begin(),options.leaves.end(),
        [&](const auto& leaf){return roots.count(leaf.key.source_face_id)!=0;}),options.leaves.end());
    std::sort(cells.begin(),cells.end(),[](const auto& a,const auto& b){return std::tie(a.source_face_id,a.id)<std::tie(b.source_face_id,b.id);});
    options.cells=std::move(cells);options.canonical_geometry_id=geometry;
}

BeautyAppearanceResult edit_glb_appearance(const boost::filesystem::path& source,
    const boost::filesystem::path& destination, const BeautyAppearanceOptions& options, const std::function<bool()>& canceled) {
    BeautyAppearanceResult result;
    try {
        checkpoint(canceled);
        parameter(options.hue_degrees,-360,360); parameter(options.saturation,0,4); parameter(options.brightness,-.5,.5);
        parameter(options.denoise,0,1); parameter(options.soften,0,1); parameter(options.detail_preserve,0,1);
        require(model_artifact_format(destination) == "glb", "Save appearance edits as a new GLB version.");
        require(!boost::filesystem::exists(destination) && !boost::filesystem::is_symlink(destination), "The output already exists; choose a new GLB version.");
        require(!options.face_weights.empty() && options.face_weights.size() <= 2000000, "Appearance editing requires a nonempty face selection.");
        bool selected = false;
        for (float weight : options.face_weights) { parameter(weight,0,1); selected = selected || weight > 0; }
        std::vector<BeautyLeafKey> leaf_keys;
        std::map<size_t,double> coverage;
        for (const auto& leaf : options.leaves) {
            require(leaf.key.valid(options.face_weights.size()) && leaf.key.depth>0,"Invalid appearance leaf.");
            parameter(leaf.weight,0,1); selected=selected || leaf.weight>0;
            require(options.face_weights[leaf.key.source_face_id]==0,"Split roots cannot use whole-face appearance weights.");
            for (auto channel : leaf.color) parameter(channel,0,1);
            leaf_keys.push_back(leaf.key); coverage[leaf.key.source_face_id]+=std::ldexp(1.,-2*leaf.key.depth);
        }
        BeautyLeafDomain::validate_keys(leaf_keys,options.face_weights.size());
        for (const auto& entry : coverage) require(entry.second==1.,"Appearance leaves do not cover every sibling.");
        std::map<size_t,ExPolygons> cell_coverage;
        std::set<std::string> cell_ids;
        std::pair<size_t,std::string> previous_cell;bool first_cell=true;
        size_t extra_triangles=0;
        for(const auto& cell:options.cells) {
            require(cell.source_face_id<options.face_weights.size() && !cell.triangles.empty() &&
                SurfacePartition::hash(cell.id) && cell_ids.insert(cell.id).second,"Invalid appearance cell.");
            const auto key=std::make_pair(cell.source_face_id,cell.id);
            require(first_cell || previous_cell<key,"Unordered appearance cells.");first_cell=false;previous_cell=key;
            require(options.face_weights[cell.source_face_id]==0 && !coverage.count(cell.source_face_id),
                    "Contour roots cannot also use leaf or whole-face edits.");
            parameter(cell.weight,0,1);selected=selected || cell.weight>0;
            for(float channel:cell.color) parameter(channel,0,1);
            auto& covered=cell_coverage[cell.source_face_id];
            for(const auto& corners:cell.triangles) {
                Json triangle=Json::array();
                for(const auto& corner:corners) triangle.push_back({corner[0],corner[1],corner[2]});
                try {
                    const auto region=SurfacePartition::polygons(Json::array({{{"polygon",triangle},{"holes",Json::array()}}}),true);
                    require(SurfacePartition::area(region)>0 &&
                            SurfacePartition::area(intersection_ex(covered,region))<SurfacePartition::area_tolerance,
                            "Appearance cell triangles overlap.");
                    covered=union_ex(covered,region);++extra_triangles;
                } catch(const std::exception& error) {
                    throw std::runtime_error("GLB contour validation face="+std::to_string(cell.source_face_id)+
                        " cell="+cell.id+": "+error.what());
                }
            }
        }
        for(const auto& region:cell_coverage)
            require(SurfacePartition::area(diff_ex(SurfacePartition::root(),region.second))<SurfacePartition::area_tolerance,
                    "Appearance cells do not cover their siblings.");
        if(!options.cells.empty()) {
            extra_triangles-=cell_coverage.size();
            for(const auto& entry:coverage) {
                const auto count=std::count_if(options.leaves.begin(),options.leaves.end(),
                    [&](const auto& leaf){return leaf.key.source_face_id==entry.first;});
                extra_triangles+=size_t(count)-1;
            }
            require(extra_triangles<=std::min(size_t(20000),options.face_weights.size()*2/100),"Appearance partition exceeds its cumulative budget.");
        }
        require(selected, "Select an area before editing appearance.");
        const bool absolute = !options.face_target_colors.empty() || !options.leaves.empty() || !options.cells.empty();
        if (absolute) {
            require(options.face_target_colors.empty() || options.face_target_colors.size() == options.face_weights.size(), "Target colors do not match the surface.");
            require(options.hue_degrees == 0 && options.saturation == 1 && options.brightness == 0 &&
                    options.denoise == 0 && options.soften == 0, "Choose either absolute puzzle colors or relative appearance adjustments.");
            for (const auto& color : options.face_target_colors) for (float channel : color) parameter(channel, 0, 1);
            for (auto weight : options.face_weights) if (weight>0)
                require(options.face_target_colors.size()==options.face_weights.size(),"Absolute root colors are missing.");
        }
        result.source_sha256 = model_artifact_sha256(source);
        require(!result.source_sha256.empty(), "Cannot verify the source GLB.");
        auto doc = read_glb(source,canceled);
        auto material = materials(doc);
        for (const auto& item : doc.json.at("meshes")) for (const auto& primitive : item.at("primitives")) {
            if (!primitive.contains("material")) continue;
            const size_t index = integer(primitive,"material");
            require(index < material.size(), "Invalid GLB primitive material.");
            if (material[index].textured)
                require(primitive.at("attributes").contains("TEXCOORD_0"), "A textured GLB primitive is missing TEXCOORD_0.");
            if (absolute) {
                // Reject malformed native color layouts before passing them to Assimp.
                if (primitive.at("attributes").contains("COLOR_0")) {
                    const size_t color = integer(primitive.at("attributes"), "COLOR_0");
                    const auto& accessor = doc.json.at("accessors").at(color);
                    const bool vec3 = accessor.value("type", "") == "VEC3";
                    const int component = accessor.value("componentType",0);
                    require(component == 5126 || component == 5121 || component == 5123, "Invalid vertex color type.");
                    const size_t width = component == 5121 ? 1 : component == 5123 ? 2 : 4;
                    const auto layout = dense_corner_accessor(doc, color, vec3 ? "VEC3" : "VEC4", (vec3 ? 3 : 4) * width, component, component != 5126);
                    require(layout[2] == integer(doc.json.at("accessors").at(integer(primitive.at("attributes"), "POSITION")), "count"),
                            "Vertex colors do not match the positions.");
                }
            }
        }
        checkpoint(canceled);
        TexturedMesh mesh; std::string error;
        std::vector<std::array<float, 4>> raw_vertex_colors;
        require(load_assimp_textured_model(source.string(),mesh,&error,absolute ? &raw_vertex_colors : nullptr), "Cannot load the GLB texture surface.");
        require(mesh.indices.size() == options.face_weights.size() && mesh.material_ids.size() == mesh.indices.size(),
            "The selection no longer matches the GLB triangle layout; reload the model.");
        require(mesh.vertices.size() <= 6000000, "The GLB exceeds the editable vertex limit.");
        std::vector<std::array<int,3>> corner_map;
        if (!options.leaves.empty() || !options.cells.empty()) {
            TriangleMesh canonical; ObjInfo info;
            require(load_model_artifact(source,canonical,info,error),"Cannot verify canonical leaf geometry.");
            require(SurfaceSelectionPersistence::geometry_fingerprint(canonical.its)==options.canonical_geometry_id,
                "Appearance leaf geometry changed.");
            require(canonical.its.indices.size()==mesh.indices.size(),"Appearance canonical face order changed.");
            corner_map.resize(mesh.indices.size());
            for (size_t f=0;f<mesh.indices.size();++f) {
                std::set<int> used;
                for (size_t c=0;c<3;++c) {
                    int raw=-1;
                    for (int j=0;j<3;++j) if (canonical.its.indices[f][c]==mesh.indices[f][j]) raw=j;
                    require(raw>=0 && used.insert(raw).second,"Appearance canonical corner mapping changed.");
                    corner_map[f][c]=raw;
                }
            }
        }
        require(model_artifact_sha256(source) == result.source_sha256, "The source GLB changed during loading; reload it first.");
        // Verify original material/image correspondence before private baked
        // images or new corner material references are appended to the document.
        std::vector<bool> used_materials(material.size(),false);
        for (int mi : mesh.material_ids) if (mi >= 0 && size_t(mi) < material.size()) used_materials[mi] = true;
        for (size_t mi = 0; mi < material.size(); ++mi) if (used_materials[mi] && material[mi].textured) {
            require(mi < mesh.material_texture_map.size() && mesh.material_texture_map[mi] >= 0 &&
                size_t(mesh.material_texture_map[mi]) < mesh.textures.size() &&
                mesh.textures[mesh.material_texture_map[mi]].data == image_bytes(doc,material[mi].image),
                "The GLB material/image mapping cannot be verified.");
        }
        bool needs_material_bake = false, needs_vertex_bake = false;
        if (absolute) for (size_t face = 0; face < mesh.indices.size(); ++face) if (options.face_weights[face] > 0) {
            const int mi = mesh.material_ids[face];
            if (mi >= 0 && size_t(mi) < material.size() && material[mi].textured) {
                for (size_t ch = 0; ch < 3; ++ch) needs_material_bake = needs_material_bake || std::abs(material[mi].factor[ch] - 1) > 1e-6;
                require(raw_vertex_colors.size() == mesh.vertices.size(), "Cannot verify the GLB vertex color multipliers.");
                for (int32_t v : mesh.indices[face]) for (size_t ch = 0; ch < 3; ++ch) {
                    parameter(raw_vertex_colors[v][ch], 0, 1);
                    needs_vertex_bake = needs_vertex_bake || std::abs(raw_vertex_colors[v][ch] - 1.f) > 1e-6f;
                }
            }
        }
        if (needs_material_bake || needs_vertex_bake) {
            indexed_triangle_set geometry;
            for (const auto& v : mesh.vertices) geometry.vertices.emplace_back(v[0] * 1000.f, -v[2] * 1000.f, v[1] * 1000.f);
            for (const auto& f : mesh.indices) geometry.indices.emplace_back(f[0],f[1],f[2]);
            const auto face_counts = verify_glb_appearance_layout(doc.json,doc.binary,geometry,mesh.material_ids,[&] { checkpoint(canceled); });
            if (needs_vertex_bake) bake_texture_vertex_factors(doc, mesh, material, raw_vertex_colors, face_counts, options, canceled);
        }
        if (absolute && std::any_of(mesh.material_ids.begin(), mesh.material_ids.end(), [&](int mi) {
            return mi >= 0 && size_t(mi) < material.size() && !material[mi].textured;
        })) {
            edit_corner_colors(doc, mesh, material, options, result, canceled);
        }
        std::map<size_t,size_t> edited_images;
        std::set<size_t> selected_roots;
        for (const auto& leaf : options.leaves) if (leaf.weight>0) selected_roots.insert(leaf.key.source_face_id);
        for(const auto& cell:options.cells) if(cell.weight>0) selected_roots.insert(cell.source_face_id);
        for (size_t face = 0; face < mesh.indices.size(); ++face) if (options.face_weights[face] > 0 || selected_roots.count(face)) {
            const int mi = mesh.material_ids[face];
            require(mi >= 0 && size_t(mi) < material.size(), "Invalid appearance face material.");
            if (absolute && !material[mi].textured) continue; // Already written as verified corner colors.
            require(material[mi].textured,
                "This selection has vertex colors or untextured materials; texture appearance editing is not available for it yet.");
            if (absolute) {
                require(raw_vertex_colors.size() == mesh.vertices.size(), "Cannot verify the GLB vertex color multipliers.");
                for (int32_t vertex : mesh.indices[face]) for (size_t ch = 0; ch < 3; ++ch)
                    require(std::isfinite(raw_vertex_colors[vertex][ch]) && std::abs(raw_vertex_colors[vertex][ch] - 1.f) <= 1e-6f,
                            "Absolute puzzle colors require neutral vertex RGB multipliers; bake the vertex colors first.");
            }
            edited_images.emplace(material[mi].image,0);
        }
        if (needs_material_bake) {
            bake_texture_material_factors(doc,material,mesh,options,canceled);
            edited_images.clear();
            for (size_t face = 0; face < mesh.indices.size(); ++face) if (options.face_weights[face] > 0) {
                const int mi = mesh.material_ids[face];
                if (mi >= 0 && size_t(mi) < material.size() && material[mi].textured) edited_images.emplace(material[mi].image,0);
            }
        }
        bool has_editable_pixel = false;
        for (auto& image : edited_images) {
            checkpoint(canceled);
            const auto encoded = image_bytes(doc,image.first);
            cv::Mat pixels = decode_image(encoded);
            const auto mask = raster_mask(mesh,material,image.first,pixels.cols,pixels.rows,options,canceled,corner_map);
            const bool editable = std::any_of(mask.weights.begin(),mask.weights.end(),[](float w){ return w > 0; });
            require(editable, "A selected texture only contains protected shared UV texels or is too small; no partial version was saved.");
            has_editable_pixel = has_editable_pixel || editable;
            const size_t changed = edit_pixels(pixels,mask,options,canceled);
            result.changed_pixels += changed;
            image.second = image.first;
            if (!changed) continue;
            std::vector<unsigned char> png;
            checkpoint(canceled);
            require(cv::imencode(".png",pixels,png,{cv::IMWRITE_PNG_COMPRESSION,3}), "Cannot encode the edited color texture.");
            checkpoint(canceled);
            image.second = append_color_image(doc,image.first,png);
        }
        require(edited_images.empty() ? result.changed_vertices > 0 : has_editable_pixel,
            "This selection only contains shared UV texels or is too small at this texture resolution. Expand the selection or unwrap overlapping UVs first.");
        std::map<size_t,size_t> replacement_textures;
        for (size_t mi = 0; mi < material.size(); ++mi) if (material[mi].textured) {
            const auto image = edited_images.find(material[mi].image);
            if (image == edited_images.end() || image->first == image->second) continue;
            const size_t original = material[mi].texture;
            auto inserted = replacement_textures.emplace(original,doc.json["textures"].size());
            if (inserted.second) {
                Json replacement = doc.json["textures"][original];
                replacement["source"] = image->second;
                doc.json["textures"].push_back(std::move(replacement));
            }
            // Baking may have assigned a private material to these primitives.
            // Update its texture rather than the retained original material.
            doc.json["materials"][material[mi].document_index]["pbrMetallicRoughness"]["baseColorTexture"]["index"] = inserted.first->second;
        }
        write_glb(doc,source,destination,result,canceled);
        result.success = true;
    } catch (const Canceled&) { result.canceled = true; result.error = "Appearance editing was canceled."; }
      catch (const std::exception& e) { result.error = e.what(); }
    if (!result.success) { result.output_sha256.clear(); result.changed_pixels = 0; result.changed_vertices = 0; }
    return result;
}
} // namespace Slic3r::AI
