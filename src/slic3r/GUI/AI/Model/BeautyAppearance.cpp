#include "BeautyAppearance.hpp"
#include "ModelArtifact.hpp"
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
    size_t image = 0, texture = 0;
    int wrap_s = 10497, wrap_t = 10497;
    std::array<double, 2> offset{0,0}, scale{1,1};
    double rotation = 0;
    bool textured = false;
    std::array<double, 4> factor {1, 1, 1, 1};
};
std::vector<Material> materials(const Document& doc) {
    std::vector<Material> result;
    for (const auto& material : doc.json.value("materials", Json::array())) {
        Material value;
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
bool touches(const std::array<Point, 3>& triangle, double x, double y) {
    // A texel can contribute through bilinear sampling one pixel away from
    // its center. Separating-axis triangle/square coverage also handles tiny
    // triangles, so a subpixel unselected face is never accidentally missed.
    for (size_t edge = 0; edge < 3; ++edge) {
        const auto& a = triangle[edge]; const auto& b = triangle[(edge + 1) % 3];
        const double nx = b[1] - a[1], ny = a[0] - b[0];
        const double p0 = nx * (triangle[0][0] - x) + ny * (triangle[0][1] - y);
        const double p1 = nx * (triangle[1][0] - x) + ny * (triangle[1][1] - y);
        const double p2 = nx * (triangle[2][0] - x) + ny * (triangle[2][1] - y);
        const double extent = std::abs(nx) + std::abs(ny);
        if (std::min({p0,p1,p2}) > extent || std::max({p0,p1,p2}) < -extent) return false;
    }
    const double min_x = std::min({triangle[0][0],triangle[1][0],triangle[2][0]});
    const double max_x = std::max({triangle[0][0],triangle[1][0],triangle[2][0]});
    const double min_y = std::min({triangle[0][1],triangle[1][1],triangle[2][1]});
    const double max_y = std::max({triangle[0][1],triangle[1][1],triangle[2][1]});
    return x + 1 >= min_x && x - 1 <= max_x && y + 1 >= min_y && y - 1 <= max_y;
}
int wrap_pixel(int position, int size, int wrap) {
    if (wrap == 10497) return (position % size + size) % size;
    return std::clamp(position, 0, size - 1); // Only the immediately adjacent tile edge can occur here.
}
struct RasterMask {
    std::vector<float> weights;
    // Only allocated for absolute colors: reference one face instead of storing
    // three floats per texel. -2 denotes conflicting requested colors.
    std::vector<int32_t> target_faces;
};
RasterMask raster_mask(const TexturedMesh& mesh, const std::vector<Material>& material,
    size_t image, int width, int height, const BeautyAppearanceOptions& options, const std::function<bool()>& canceled) {
    RasterMask mask;
    mask.weights.assign(size_t(width) * height, -1.f);
    const bool absolute = !options.face_target_colors.empty();
    if (absolute) mask.target_faces.assign(mask.weights.size(), -1);
    uint64_t visits = 0;
    for (size_t face = 0; face < mesh.indices.size(); ++face) {
        if ((face & 1023) == 0) checkpoint(canceled);
        const int mi = mesh.material_ids[face];
        if (mi < 0 || size_t(mi) >= material.size() || !material[mi].textured || material[mi].image != image) continue;
        const auto triangle = face_uvs(mesh, face, material[mi], width, height);
        const int x0 = std::max(-1, int(std::floor(std::min({triangle[0][0],triangle[1][0],triangle[2][0]}) - 1.5)));
        const int x1 = std::min(width, int(std::ceil(std::max({triangle[0][0],triangle[1][0],triangle[2][0]}) + .5)));
        const int y0 = std::max(-1, int(std::floor(std::min({triangle[0][1],triangle[1][1],triangle[2][1]}) - 1.5)));
        const int y1 = std::min(height, int(std::ceil(std::max({triangle[0][1],triangle[1][1],triangle[2][1]}) + .5)));
        visits += uint64_t(x1 - x0 + 1) * (y1 - y0 + 1);
        require(visits <= 300000000, "The UV layout is too expensive for local editing; simplify or unwrap the texture first.");
        for (int y = y0; y <= y1; ++y) {
            if ((y & 127) == 0) checkpoint(canceled);
            for (int x = x0; x <= x1; ++x) if (touches(triangle, x + .5, y + .5)) {
                const size_t pixel = size_t(wrap_pixel(y, height, material[mi].wrap_t)) * width + wrap_pixel(x, width, material[mi].wrap_s);
                mask.weights[pixel] = mask.weights[pixel] < 0 ? options.face_weights[face] : std::min(mask.weights[pixel], options.face_weights[face]);
                if (absolute && options.face_weights[face] > 0) {
                    auto& owner = mask.target_faces[pixel];
                    if (owner == -1) owner = int32_t(face);
                    else if (owner >= 0) {
                        const auto& a = options.face_target_colors[size_t(owner)];
                        const auto& b = options.face_target_colors[face];
                        for (size_t ch = 0; ch < 3; ++ch) if (std::abs(a[ch] - b[ch]) > 1e-6f) { owner = -2; break; }
                    }
                    if (owner == -2) mask.weights[pixel] = 0;
                }
            }
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
    const bool absolute = !options.face_target_colors.empty();
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
                const auto& target = options.face_target_colors[size_t(face)];
                for (size_t ch = 0; ch < 3; ++ch) filtered[ch] = target[ch];
            } else filtered = adjust(filtered, options);
            bool different = false;
            for (size_t channel = 0; channel < 3; ++channel) {
                const auto value = static_cast<unsigned char>(std::lround(255 * std::clamp(color[channel] + weight*(filtered[channel]-color[channel]), 0.0, 1.0)));
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
} // namespace

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
        require(selected, "Select an area before editing appearance.");
        const bool absolute = !options.face_target_colors.empty();
        if (absolute) {
            require(options.face_target_colors.size() == options.face_weights.size(), "Target colors do not match the surface.");
            require(options.hue_degrees == 0 && options.saturation == 1 && options.brightness == 0 &&
                    options.denoise == 0 && options.soften == 0, "Choose either absolute puzzle colors or relative appearance adjustments.");
            for (const auto& color : options.face_target_colors) for (float channel : color) parameter(channel, 0, 1);
        }
        result.source_sha256 = model_artifact_sha256(source);
        require(!result.source_sha256.empty(), "Cannot verify the source GLB.");
        auto doc = read_glb(source,canceled);
        const auto material = materials(doc);
        for (const auto& item : doc.json.at("meshes")) for (const auto& primitive : item.at("primitives")) {
            if (!primitive.contains("material")) continue;
            const size_t index = integer(primitive,"material");
            require(index < material.size(), "Invalid GLB primitive material.");
            if (material[index].textured)
                require(primitive.at("attributes").contains("TEXCOORD_0"), "A textured GLB primitive is missing TEXCOORD_0.");
        }
        checkpoint(canceled);
        TexturedMesh mesh; std::string error;
        std::vector<std::array<float, 4>> raw_vertex_colors;
        require(load_assimp_textured_model(source.string(),mesh,&error,absolute ? &raw_vertex_colors : nullptr), "Cannot load the GLB texture surface.");
        require(mesh.indices.size() == options.face_weights.size() && mesh.material_ids.size() == mesh.indices.size(),
            "The selection no longer matches the GLB triangle layout; reload the model.");
        require(mesh.vertices.size() <= 6000000, "The GLB exceeds the editable vertex limit.");
        require(model_artifact_sha256(source) == result.source_sha256, "The source GLB changed during loading; reload it first.");
        std::map<size_t,size_t> edited_images;
        for (size_t face = 0; face < mesh.indices.size(); ++face) if (options.face_weights[face] > 0) {
            const int mi = mesh.material_ids[face];
            require(mi >= 0 && size_t(mi) < material.size() && material[mi].textured,
                "This selection has vertex colors or untextured materials; texture appearance editing is not available for it yet.");
            if (absolute) {
                for (size_t ch = 0; ch < 3; ++ch)
                    require(std::abs(material[mi].factor[ch] - 1.) <= 1e-6,
                            "Absolute puzzle colors require neutral material RGB multipliers; bake the material color first.");
                require(raw_vertex_colors.size() == mesh.vertices.size(), "Cannot verify the GLB vertex color multipliers.");
                for (int32_t vertex : mesh.indices[face]) for (size_t ch = 0; ch < 3; ++ch)
                    require(std::isfinite(raw_vertex_colors[vertex][ch]) && std::abs(raw_vertex_colors[vertex][ch] - 1.f) <= 1e-6f,
                            "Absolute puzzle colors require neutral vertex RGB multipliers; bake the vertex colors first.");
            }
            edited_images.emplace(material[mi].image,0);
        }
        bool has_editable_pixel = false;
        for (auto& image : edited_images) {
            checkpoint(canceled);
            const auto encoded = image_bytes(doc,image.first);
            // Assert Assimp's material-to-image correspondence rather than
            // silently using a mismatched imported material index.
            for (size_t mi = 0; mi < material.size(); ++mi) if (material[mi].textured && material[mi].image == image.first) {
                require(mi < mesh.material_texture_map.size() && mesh.material_texture_map[mi] >= 0 &&
                    size_t(mesh.material_texture_map[mi]) < mesh.textures.size() &&
                    mesh.textures[mesh.material_texture_map[mi]].data == encoded, "The GLB material/image mapping cannot be verified.");
            }
            cv::Mat pixels = decode_image(encoded);
            const auto mask = raster_mask(mesh,material,image.first,pixels.cols,pixels.rows,options,canceled);
            has_editable_pixel = has_editable_pixel || std::any_of(mask.weights.begin(),mask.weights.end(),[](float w){ return w > 0; });
            const size_t changed = edit_pixels(pixels,mask,options,canceled);
            result.changed_pixels += changed;
            image.second = image.first;
            if (!changed) continue;
            std::vector<unsigned char> png;
            checkpoint(canceled);
            require(cv::imencode(".png",pixels,png,{cv::IMWRITE_PNG_COMPRESSION,3}), "Cannot encode the edited color texture.");
            checkpoint(canceled);
            while (doc.binary.size() % 4) doc.binary.push_back(0);
            Json replacement = doc.json["images"][image.first];
            replacement["bufferView"] = doc.json["bufferViews"].size();
            replacement["mimeType"] = "image/png";
            doc.json["bufferViews"].push_back({{"buffer",0},{"byteOffset",doc.binary.size()},{"byteLength",png.size()}});
            require(png.size() <= max_bytes - doc.binary.size(), "The edited GLB exceeds 512 MB.");
            doc.binary.insert(doc.binary.end(),png.begin(),png.end());
            image.second = doc.json["images"].size();
            doc.json["images"].push_back(std::move(replacement));
        }
        require(has_editable_pixel, "This selection only contains shared UV texels or is too small at this texture resolution. Expand the selection or unwrap overlapping UVs first.");
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
            doc.json["materials"][mi]["pbrMetallicRoughness"]["baseColorTexture"]["index"] = inserted.first->second;
        }
        write_glb(doc,source,destination,result,canceled);
        result.success = true;
    } catch (const Canceled&) { result.canceled = true; result.error = "Appearance editing was canceled."; }
      catch (const std::exception& e) { result.error = e.what(); }
    if (!result.success) { result.output_sha256.clear(); result.changed_pixels = 0; }
    return result;
}
} // namespace Slic3r::AI
