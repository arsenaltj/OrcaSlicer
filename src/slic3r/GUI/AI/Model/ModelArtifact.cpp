#include "ModelArtifact.hpp"

#include "libslic3r/Format/AssimpImport.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "libslic3r/Sha256Digest.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include "libslic3r/TriangleMesh.hpp"
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <stdexcept>
#include <openssl/evp.h>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <type_traits>
#ifdef _WIN32
#include <charconv>
#endif

namespace Slic3r::AI {
namespace {
constexpr uint64_t max_bytes = 512ull * 1024 * 1024;
float linear(float v) { return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f); }
float srgb(float v) {
    v = std::clamp(v, 0.f, 1.f);
    return v <= .0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - .055f;
}
uint32_t read_u32(const unsigned char* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
void append_u32(std::vector<unsigned char>& data, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) data.push_back(static_cast<unsigned char>(value >> shift));
}
void append_float(std::vector<unsigned char>& data, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(data, bits);
}
void write_u32(std::ostream& output, uint32_t value) {
    std::array<unsigned char, 4> bytes;
    for (size_t i = 0; i < 4; ++i) bytes[i] = static_cast<unsigned char>(value >> (i * 8));
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
nlohmann::json glb_description(const boost::filesystem::path& path) {
    boost::filesystem::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 20> header {};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    const uint32_t json_size = read_u32(header.data() + 12);
    if (!input || read_u32(header.data()) != 0x46546c67 || read_u32(header.data() + 4) != 2 ||
        read_u32(header.data() + 8) != boost::filesystem::file_size(path) ||
        read_u32(header.data() + 16) != 0x4e4f534a || !json_size || json_size % 4 ||
        json_size > 32u * 1024 * 1024 || json_size > boost::filesystem::file_size(path) - 20)
        throw std::runtime_error("Invalid or incomplete GLB 2.0 file.");
    std::string encoded(json_size, '\0');
    input.read(encoded.data(), encoded.size());
    if (!input) throw std::runtime_error("GLB description is incomplete.");
    auto doc = nlohmann::json::parse(encoded);
    if (doc.at("asset").value("version", "") != "2.0") throw std::runtime_error("Only GLB 2.0 is supported.");
    for (const auto& extension : doc.value("extensionsRequired", nlohmann::json::array()))
        if (extension != "KHR_materials_unlit" && extension != "KHR_texture_transform" && extension != "KHR_mesh_quantization")
            throw std::runtime_error("Unsupported GLB extension: " + extension.get<std::string>());
    for (const auto& node : doc.value("nodes", nlohmann::json::array()))
        if (node.contains("skin") || node.contains("weights")) throw std::runtime_error("Bake animated GLB geometry before editing.");
    for (const auto& mesh : doc.at("meshes")) for (const auto& primitive : mesh.at("primitives"))
        if (primitive.value("mode", 4) != 4 || primitive.contains("targets"))
            throw std::runtime_error("GLB must contain static triangle meshes.");
    for (const auto& material : doc.value("materials", nlohmann::json::array())) {
        const auto pbr = material.value("pbrMetallicRoughness", nlohmann::json::object());
        if (pbr.contains("baseColorTexture")) {
            const auto& ti = pbr["baseColorTexture"];
            const auto transform = ti.value("extensions", nlohmann::json::object()).value("KHR_texture_transform", nlohmann::json::object());
            if (transform.value("texCoord", ti.value("texCoord", 0)) != 0)
                throw std::runtime_error("GLB color textures must use TEXCOORD_0 for local editing.");
        }
    }
    for (const auto& buffer : doc.at("buffers"))
        if (buffer.contains("uri")) throw std::runtime_error("GLB geometry must be embedded in the file.");
    for (const auto& image : doc.value("images", nlohmann::json::array()))
        if (image.contains("uri") && image.at("uri").get<std::string>().rfind("data:", 0) != 0)
            throw std::runtime_error("GLB images must be embedded in the file.");
    return doc;
}
float wrap(float value, int mode) {
    if (mode == 33071) return std::clamp(value, 0.f, 1.f);
    if (mode == 33648) { value -= 2.f * std::floor(value / 2.f); return 1.f - std::abs(value - 1.f); }
    if (mode == 10497) return value - std::floor(value);
    throw std::runtime_error("Unsupported GLB texture wrapping mode.");
}
} // namespace

std::string model_artifact_format(const boost::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return extension == ".glb" ? "glb" : extension == ".obj" ? "obj" : "";
}

std::string model_artifact_sha256(const boost::filesystem::path& path) {
    boost::filesystem::ifstream input(path, std::ios::binary);
    if (!input) return {};
    Sha256Digest digest;
    std::array<char, 65536> buffer;
    while (input) {
        input.read(buffer.data(), buffer.size());
        if (input.gcount() && !digest.update(buffer.data(), size_t(input.gcount()))) return {};
    }
    if (!input.eof()) return {};
    return digest.final_hex();
}

bool is_model_artifact(const boost::filesystem::path& path) {
    boost::system::error_code ec;
    if (model_artifact_format(path).empty() || !boost::filesystem::is_regular_file(path, ec) || ec) return false;
    const auto size = boost::filesystem::file_size(path, ec);
    return !ec && size > 0 && size <= max_bytes;
}

bool load_model_artifact(const boost::filesystem::path& path, TriangleMesh& mesh, ObjInfo& info, std::string& error,
                         const std::function<bool()>& canceled) {
    error.clear();
    info = ObjInfo {};
    auto stop_if_canceled = [&] {
        if (!canceled || !canceled()) return false;
        mesh = TriangleMesh {};info = ObjInfo {};
        error = "Model loading canceled.";
        return true;
    };
    try {
        if (stop_if_canceled()) return false;
        if (!is_model_artifact(path)) throw std::runtime_error("The OBJ/GLB model is missing or exceeds 512 MB.");
        if (model_artifact_format(path) == "obj") return load_obj(path.string().c_str(), &mesh, info, error, nullptr, canceled);
        const auto doc = glb_description(path);
        if (stop_if_canceled()) return false;
        TexturedMesh textured;
        std::vector<std::array<float, 4>> vertex_colors;
        const auto source_materials = doc.find("materials");
        // A declared but missing texture must still follow the original raw
        // sampling/error path. Inspect conservatively without moving validation.
        const bool raw_fallback_only = source_materials == doc.end() ||
            (source_materials->is_array() && std::all_of(source_materials->begin(), source_materials->end(), [](const auto& material) {
                if (!material.is_object()) return false;
                const auto pbr = material.find("pbrMetallicRoughness");
                return pbr == material.end() || (pbr->is_object() && !pbr->contains("baseColorTexture"));
            }));
        if (!load_assimp_textured_model(path.string(), textured, &error, &vertex_colors,
                raw_fallback_only ? AssimpRawColorPolicy::FallbackOnly : AssimpRawColorPolicy::Always)) return false;
        if (stop_if_canceled()) return false;
        if (textured.vertices.empty() || textured.vertices.size() > 6000000 || textured.indices.size() > 2000000)
            throw std::runtime_error("GLB model exceeds the editable mesh limit.");
        indexed_triangle_set its;
        its.vertices.reserve(textured.vertices.size());
        its.indices.reserve(textured.indices.size());
        for (size_t vertex = 0; vertex < textured.vertices.size(); ++vertex) {
            if ((vertex & 4095) == 0 && stop_if_canceled()) return false;
            const auto& v = textured.vertices[vertex];
            if (!std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); }))
                throw std::runtime_error("GLB contains non-finite geometry.");
            its.vertices.emplace_back(v[0] * 1000.f, -v[2] * 1000.f, v[1] * 1000.f);
        }
        for (size_t face = 0; face < textured.indices.size(); ++face) {
            if ((face & 4095) == 0 && stop_if_canceled()) return false;
            const auto& f = textured.indices[face];
            for (int v : f) if (v < 0 || size_t(v) >= its.vertices.size()) throw std::runtime_error("GLB triangle index is invalid.");
            its.indices.emplace_back(f[0], f[1], f[2]);
        }
        struct Pixels { std::vector<unsigned char> data; int width {0}, height {0}; };
        std::vector<Pixels> images(textured.textures.size());
        for (size_t i = 0; i < images.size(); ++i) {
            if (stop_if_canceled()) return false;
            if (!decode_texture_to_pixels(textured.textures[i], images[i].data, images[i].width, images[i].height) ||
                uint64_t(images[i].width) * images[i].height > 64ull * 1024 * 1024)
                throw std::runtime_error("GLB color texture cannot be decoded.");
        }
        info = ObjInfo {};
        const auto materials = doc.find("materials");
        const bool declares_color_texture = materials != doc.end() &&
            std::any_of(materials->begin(), materials->end(), [](const auto& material) {
                return material.value("pbrMetallicRoughness", nlohmann::json::object()).contains("baseColorTexture");
            });
        if (!declares_color_texture && textured.precomputed_vertex_colors.size() == its.vertices.size()) {
            // Native import already converted each primitive's vertex colors.
            // Keep editing's opaque alpha and white unreferenced vertices;
            // declared textures retain the original missing-texture checks.
            std::vector<unsigned char> referenced(its.vertices.size(), 0);
            for (const auto& face : its.indices)
                for (int vertex : face) referenced[vertex] = 1;
            info.vertex_colors = std::move(textured.precomputed_vertex_colors);
            for (size_t vertex = 0; vertex < info.vertex_colors.size(); ++vertex) {
                if ((vertex & 4095) == 0 && stop_if_canceled()) return false;
                if (!referenced[vertex]) info.vertex_colors[vertex] = RGBA {1.f, 1.f, 1.f, 1.f};
                else info.vertex_colors[vertex][3] = 1.f;
            }
            mesh = TriangleMesh(std::move(its));
            if (mesh.volume() < 0) mesh.flip_triangles();
            if (stop_if_canceled()) return false;
            return !mesh.empty();
        }
        info.vertex_colors.assign(its.vertices.size(), RGBA {1.f, 1.f, 1.f, 1.f});
        std::vector<unsigned char> assigned(its.vertices.size(), 0);
        struct MaterialSampling {
            int texture {-1}, wrap_s {10497}, wrap_t {10497};
            std::array<float, 4> factor {1.f, 1.f, 1.f, 1.f};
            std::array<float, 3> previous_vertex_color {};
            RGBA previous_output_color {1.f, 1.f, 1.f, 1.f};
            bool previous_color_ready {false};
            bool uniform_color_candidate {true};
            nlohmann::json transform = nlohmann::json::object();
            std::array<float, 2> scale {1.f, 1.f}, offset {0.f, 0.f};
            float cosine {1.f}, sine {0.f};
            bool transform_ready {false};
        };
        // Only cache materials actually encountered in this load. Keep texture
        // transform parsing lazy: unused vertices/materials were not sampled.
        std::unordered_map<int, MaterialSampling> material_sampling;
        const bool has_vertex_colors = vertex_colors.size() == its.vertices.size();
        for (size_t fi = 0; fi < textured.indices.size(); ++fi) {
            if ((fi & 4095) == 0 && stop_if_canceled()) return false;
            const int material = fi < textured.material_ids.size() ? textured.material_ids[fi] : -1;
            auto inserted = material_sampling.try_emplace(material);
            auto& sampling = inserted.first->second;
            if (inserted.second) {
                sampling.texture = material >= 0 && size_t(material) < textured.material_texture_map.size()
                    ? textured.material_texture_map[material] : -1;
                sampling.factor = material >= 0 && size_t(material) < textured.material_colors.size()
                    ? textured.material_colors[material] : std::array<float, 4>{1.f, 1.f, 1.f, 1.f};
                if (material >= 0 && doc.contains("materials") && size_t(material) < doc["materials"].size()) {
                    const auto& pbr = doc["materials"][material].value("pbrMetallicRoughness", nlohmann::json::object());
                    if (pbr.contains("baseColorTexture")) {
                        if (sampling.texture < 0) throw std::runtime_error("The GLB color texture is missing or cannot be read.");
                        const auto& ti = pbr["baseColorTexture"];
                        sampling.transform = ti.value("extensions", nlohmann::json::object()).value("KHR_texture_transform", nlohmann::json::object());
                        const auto& td = doc.at("textures").at(ti.at("index").get<size_t>());
                        if (td.contains("sampler")) {
                            const auto& sampler = doc.at("samplers").at(td.at("sampler").get<size_t>());
                            sampling.wrap_s = sampler.value("wrapS", 10497); sampling.wrap_t = sampler.value("wrapT", 10497);
                        }
                    }
                }
            }
            const int texture = sampling.texture;
            const auto& factor = sampling.factor;
            for (int vi : textured.indices[fi]) {
                if (assigned[vi]) continue;
                assigned[vi] = 1;
                // Untextured primitives often use one vertex color for an
                // entire material. Reuse only identical float bit patterns so
                // invalid or signed-zero inputs keep the original behavior.
                if (texture < 0 && sampling.uniform_color_candidate && sampling.previous_color_ready) {
                    if (!has_vertex_colors ||
                        std::memcmp(vertex_colors[vi].data(), sampling.previous_vertex_color.data(), sizeof(float) * 3) == 0) {
                        info.vertex_colors[vi] = sampling.previous_output_color;
                        continue;
                    }
                    // A varying material has no consecutive-color reuse;
                    // leave it on the original conversion path from here on.
                    sampling.uniform_color_candidate = false;
                }
                RGBA color {1.f, 1.f, 1.f, 1.f};
                if (texture >= 0) {
                    if (size_t(texture) >= images.size() || size_t(vi) >= textured.uvs.size())
                        throw std::runtime_error("GLB texture coordinates are missing.");
                    const auto& image = images[texture];
                    auto uv = textured.uvs[vi];
                    if (!sampling.transform_ready) {
                        sampling.scale = sampling.transform.value("scale", std::array<float, 2>{1.f, 1.f});
                        sampling.offset = sampling.transform.value("offset", std::array<float, 2>{0.f, 0.f});
                        const float angle = sampling.transform.value("rotation", 0.f);
                        sampling.cosine = std::cos(angle); sampling.sine = std::sin(angle);
                        sampling.transform_ready = true;
                    }
                    const float u = uv[0] * sampling.scale[0], v = uv[1] * sampling.scale[1];
                    uv = {sampling.offset[0] + sampling.cosine * u - sampling.sine * v,
                          sampling.offset[1] + sampling.sine * u + sampling.cosine * v};
                    if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) throw std::runtime_error("GLB UV is invalid.");
                    const int x = std::min(image.width - 1, int(wrap(uv[0], sampling.wrap_s) * image.width));
                    const int y = std::min(image.height - 1, int(wrap(uv[1], sampling.wrap_t) * image.height));
                    const auto* pixel = image.data.data() + (size_t(y) * image.width + x) * 3;
                    for (size_t c = 0; c < 3; ++c) color[c] = linear(pixel[2 - c] / 255.f);
                }
                for (size_t c = 0; c < 3; ++c) {
                    const float vertex = has_vertex_colors ? vertex_colors[vi][c] : 1.f;
                    if (!std::isfinite(vertex) || !std::isfinite(factor[c])) throw std::runtime_error("GLB color is invalid.");
                    color[c] = srgb(color[c] * factor[c] * vertex);
                }
                info.vertex_colors[vi] = color;
                if (texture < 0 && sampling.uniform_color_candidate) {
                    if (has_vertex_colors)
                        std::memcpy(sampling.previous_vertex_color.data(), vertex_colors[vi].data(), sizeof(float) * 3);
                    sampling.previous_output_color = color;
                    sampling.previous_color_ready = true;
                }
            }
        }
        mesh = TriangleMesh(std::move(its));
        if (mesh.volume() < 0) mesh.flip_triangles();
        if (stop_if_canceled()) return false;
        return !mesh.empty();
    } catch (const std::exception& e) { error = e.what(); return false; }
}

static bool write_model_artifact_impl(const boost::filesystem::path& path, const indexed_triangle_set& mesh,
                                      const std::vector<RGBA>& colors, std::string& error,
                                      std::string* written_sha256) {
    boost::filesystem::path temporary = path; temporary += ".partial";
    bool owned = false;
    std::string prepared_hash;
    if (written_sha256) written_sha256->clear();
    try {
        if (mesh.vertices.empty() || mesh.indices.empty() || colors.size() != mesh.vertices.size())
            throw std::runtime_error("The edited model has incomplete geometry or colors.");
        if (boost::filesystem::exists(path) || boost::filesystem::exists(temporary))
            throw std::runtime_error("The output already exists; choose a new model version.");
        if (model_artifact_format(path).empty()) throw std::runtime_error("Choose OBJ or GLB for the model.");
        if (!path.parent_path().empty()) boost::filesystem::create_directories(path.parent_path());
        for (const auto& v : mesh.vertices) for (int c = 0; c < 3; ++c)
            if (!std::isfinite(v[c])) throw std::runtime_error("The edited model contains invalid vertices.");
        for (const auto& color : colors) for (float c : color)
            if (!std::isfinite(c) || c < 0.f || c > 1.f) throw std::runtime_error("The edited model contains invalid colors.");
        for (const auto& f : mesh.indices) for (int c = 0; c < 3; ++c)
            if (f[c] < 0 || size_t(f[c]) >= mesh.vertices.size()) throw std::runtime_error("The edited mesh has invalid indices.");
        boost::filesystem::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Unable to create the model file.");
        owned = true;
        output.imbue(std::locale::classic());
        if (model_artifact_format(path) == "obj") {
            constexpr char header[] = "# Orca AI model: Z-up millimetres, sRGB vertex colors\n";
            output << header << std::setprecision(9);
#ifdef _WIN32
            // Like LocalesUtils, use floating-point to_chars on the supported
            // Windows toolchain. Keep the classic-locale stream fallback for
            // toolchains whose C++17 library lacks floating-point charconv.
            // A bounded batch avoids stream locale/sentry work per number.
            std::string batch;
            batch.reserve(64 * 1024 + 256);
            std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(nullptr, EVP_MD_CTX_free);
            if (written_sha256) {
                digest.reset(EVP_MD_CTX_new());
                if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1 ||
                    EVP_DigestUpdate(digest.get(), header, sizeof(header) - 1) != 1)
                    throw std::runtime_error("Unable to hash the prepared OBJ import copy.");
            }
            auto append_number = [](std::string& target, auto value) {
                char text[64];
                std::to_chars_result converted;
                if constexpr (std::is_floating_point_v<decltype(value)>)
                    converted = std::to_chars(text, text + sizeof(text), value, std::chars_format::general, 9);
                else
                    converted = std::to_chars(text, text + sizeof(text), value);
                if (converted.ec != std::errc()) throw std::runtime_error("Unable to format the model number.");
                target.append(text, converted.ptr);
            };
            struct PositionText {
                uint32_t bits {0};
                uint8_t length {0};
                bool valid {false};
                char text[64] {};
            };
            constexpr unsigned position_cache_bits = 12;
            constexpr size_t position_cache_size = size_t(1) << position_cache_bits;
            auto position_cache = std::make_unique<PositionText[]>(position_cache_size);
            size_t position_samples = 0, position_hits = 0;
            auto append_position = [&](std::string& target, float value) {
                if (!position_cache) {
                    append_number(target, value);
                    return;
                }
                uint32_t bits;
                std::memcpy(&bits, &value, sizeof(bits));
                auto& entry = position_cache[(bits * 2654435761u) >> (32 - position_cache_bits)];
                if (entry.valid && entry.bits == bits) {
                    ++position_hits;
                } else {
                    const auto converted = std::to_chars(entry.text, entry.text + sizeof(entry.text),
                                                         value, std::chars_format::general, 9);
                    if (converted.ec != std::errc()) throw std::runtime_error("Unable to format the model number.");
                    entry.bits = bits;
                    entry.length = uint8_t(converted.ptr - entry.text);
                    entry.valid = true;
                }
                target.append(entry.text, entry.length);
                // Sparse coordinate reuse pays less than the lookup cost. Decide
                // once from roughly the first 1.4k vertices, without changing any bytes.
                if (++position_samples == 4096 && position_hits < 820)
                    position_cache.reset();
            };
            auto flush_batch = [&]() {
                output.write(batch.data(), std::streamsize(batch.size()));
                if (!output) throw std::runtime_error("Unable to write the complete model.");
                if (digest && EVP_DigestUpdate(digest.get(), batch.data(), batch.size()) != 1)
                    throw std::runtime_error("Unable to hash the prepared OBJ import copy.");
                batch.clear();
            };
            std::array<float, 3> previous_color {};
            std::string formatted_color;
            bool has_previous_color = false;
            for (size_t i = 0; i < mesh.vertices.size(); ++i) {
                batch += "v ";
                for (int c = 0; c < 3; ++c) {
                    append_position(batch, mesh.vertices[i][c]);
                    batch += ' ';
                }
                const auto& color = colors[i];
                // Material-colored meshes often repeat a color for millions of
                // vertices. Compare bits so signed zero retains its OBJ text.
                if (!has_previous_color || std::memcmp(previous_color.data(), color.data(), sizeof(float) * 3) != 0) {
                    std::copy_n(color.begin(), 3, previous_color.begin());
                    formatted_color.clear();
                    for (int c = 0; c < 3; ++c) {
                        append_number(formatted_color, color[c]);
                        if (c != 2) formatted_color += ' ';
                    }
                    has_previous_color = true;
                }
                batch += formatted_color;
                batch += '\n';
                if (batch.size() >= 64 * 1024) flush_batch();
            }
            for (const auto& face : mesh.indices) {
                batch += "f ";
                for (int c = 0; c < 3; ++c) {
                    append_number(batch, face[c] + 1);
                    batch += c == 2 ? '\n' : ' ';
                }
                if (batch.size() >= 64 * 1024) flush_batch();
            }
            if (!batch.empty()) flush_batch();
            if (digest) {
                unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned length = 0;
                if (EVP_DigestFinal_ex(digest.get(), bytes, &length) != 1)
                    throw std::runtime_error("Unable to hash the prepared OBJ import copy.");
                std::ostringstream hex;
                for (unsigned i = 0; i < length; ++i)
                    hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
                prepared_hash = hex.str();
            }
#else
            for (size_t i = 0; i < mesh.vertices.size(); ++i) {
                const auto& v = mesh.vertices[i]; const auto& c = colors[i];
                output << "v " << v.x() << ' ' << v.y() << ' ' << v.z() << ' ' << c[0] << ' ' << c[1] << ' ' << c[2] << '\n';
            }
            for (const auto& f : mesh.indices) output << "f " << f[0] + 1 << ' ' << f[1] + 1 << ' ' << f[2] + 1 << '\n';
#endif
        } else {
            std::vector<unsigned char> binary;
            std::array<float, 3> minimum {INFINITY, INFINITY, INFINITY}, maximum {-INFINITY, -INFINITY, -INFINITY};
            binary.reserve(mesh.vertices.size() * 24 + mesh.indices.size() * 12);
            for (const auto& v : mesh.vertices) {
                const std::array<float, 3> p {v.x() * .001f, v.z() * .001f, -v.y() * .001f};
                for (size_t c = 0; c < 3; ++c) { append_float(binary, p[c]); minimum[c] = std::min(minimum[c], p[c]); maximum[c] = std::max(maximum[c], p[c]); }
            }
            const size_t colors_offset = binary.size();
            for (const auto& color : colors) for (size_t c = 0; c < 3; ++c) append_float(binary, linear(color[c]));
            const size_t indices_offset = binary.size();
            for (const auto& face : mesh.indices) for (int c = 0; c < 3; ++c) append_u32(binary, uint32_t(face[c]));
            using nlohmann::json;
            json doc = {
                {"asset", {{"version", "2.0"}, {"generator", "Orca AI local model editor"}}},
                {"scene", 0}, {"scenes", {{{"nodes", {0}}}}}, {"nodes", {{{"mesh", 0}}}},
                {"buffers", {{{"byteLength", binary.size()}}}},
                {"bufferViews", {{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", colors_offset}},
                                 {{"buffer", 0}, {"byteOffset", colors_offset}, {"byteLength", indices_offset - colors_offset}},
                                 {{"buffer", 0}, {"byteOffset", indices_offset}, {"byteLength", binary.size() - indices_offset}}}},
                {"accessors", {{{"bufferView", 0}, {"componentType", 5126}, {"count", mesh.vertices.size()}, {"type", "VEC3"}, {"min", minimum}, {"max", maximum}},
                               {{"bufferView", 1}, {"componentType", 5126}, {"count", colors.size()}, {"type", "VEC3"}},
                               {{"bufferView", 2}, {"componentType", 5125}, {"count", mesh.indices.size() * 3}, {"type", "SCALAR"}}}},
                {"meshes", {{{"primitives", {{{"attributes", {{"POSITION", 0}, {"COLOR_0", 1}}}, {"indices", 2}, {"mode", 4}, {"material", 0}}}}}}},
                {"materials", {{{"doubleSided", true}, {"pbrMetallicRoughness", {{"baseColorFactor", {1., 1., 1., 1.}}, {"metallicFactor", 0.}, {"roughnessFactor", 1.}}}}}}
            };
            std::string encoded = doc.dump();
            while (encoded.size() % 4) encoded += ' ';
            const size_t size = 28 + encoded.size() + binary.size();
            if (size > max_bytes) throw std::runtime_error("The edited GLB exceeds 512 MB.");
            write_u32(output, 0x46546c67); write_u32(output, 2); write_u32(output, uint32_t(size));
            write_u32(output, uint32_t(encoded.size())); write_u32(output, 0x4e4f534a); output.write(encoded.data(), encoded.size());
            write_u32(output, uint32_t(binary.size())); write_u32(output, 0x004e4942); output.write(reinterpret_cast<const char*>(binary.data()), binary.size());
        }
        output.close();
        if (!output || boost::filesystem::file_size(temporary) > max_bytes) throw std::runtime_error("Unable to write the complete model.");
        // Windows OBJ imports already hashed the exact bytes sent to the
        // writer. Other formats/toolchains retain the read-back path.
        if (written_sha256 && prepared_hash.empty()) {
            prepared_hash = model_artifact_sha256(temporary);
            if (prepared_hash.empty()) throw std::runtime_error("Unable to hash the prepared model file.");
        }
        boost::filesystem::rename(temporary, path);
        if (written_sha256) written_sha256->swap(prepared_hash);
        return true;
    } catch (const std::exception& e) {
        if (owned) { boost::system::error_code ignored; boost::filesystem::remove(temporary, ignored); }
        error = e.what(); return false;
    }
}

bool write_model_artifact(const boost::filesystem::path& path, const indexed_triangle_set& mesh,
                          const std::vector<RGBA>& colors, std::string& error) {
    return write_model_artifact_impl(path, mesh, colors, error, nullptr);
}

bool prepare_glb_obj_import(const boost::filesystem::path& source,
                            const boost::filesystem::path& cache_root,
                            VerifiedGlbImportCopy& verified,
                            boost::filesystem::path& output,
                            std::string& error,
                            bool& reused) {
    output.clear();
    error.clear();
    reused = false;
    if (cache_root.empty()) {
        error = "The Orca temporary directory is unavailable for model import.";
        return false;
    }
    if (model_artifact_format(source) != "glb" || !is_model_artifact(source)) {
        error = "The generated GLB is missing or exceeds 512 MB.";
        return false;
    }

    try {
        const std::string source_hash = model_artifact_sha256(source);
        if (source_hash.empty()) {
            error = "Unable to verify the GLB import source.";
            return false;
        }
        // The full content hash keeps history nesting and saved-version names
        // out of the import path; a 3MF save retains only this OBJ basename.
        const auto canonical = cache_root / "ai-import" / ("orcaslicer-ai-glb-" + source_hash + ".obj");
        const bool previously_verified = verified.source_sha256 == source_hash &&
                                         !verified.obj_sha256.empty();
        if (previously_verified && is_model_artifact(verified.obj_path) &&
            model_artifact_sha256(verified.obj_path) == verified.obj_sha256) {
            output = verified.obj_path;
            reused = true;
            return true;
        }

        // A content-addressed name does not prove that an OBJ was produced
        // from these GLB bytes. Never trust a previous process's copy (or a
        // modified copy from this process) after decoding the source again.
        verified = {};
        TriangleMesh mesh;
        ObjInfo colors;
        if (!load_model_artifact(source, mesh, colors, error)) return false;
        boost::filesystem::path candidate = canonical;
        if (boost::filesystem::exists(candidate))
            candidate = cache_root / "ai-import" /
                ("orcaslicer-ai-glb-" + source_hash + "-" +
                 boost::filesystem::unique_path("%%%%-%%%%").string() + ".obj");
        std::string obj_hash;
        if (!write_model_artifact_impl(candidate, mesh.its, colors.vertex_colors, error, &obj_hash)) return false;
        if (obj_hash.empty()) {
            error = "Unable to verify the prepared OBJ import copy.";
            return false;
        }
        verified = {source_hash, candidate, obj_hash};
        output = candidate;
        return true;
    } catch (const std::exception& e) {
        verified = {};
        error = e.what();
        return false;
    }
}

bool archive_local_model(const boost::filesystem::path& source,const boost::filesystem::path& destination,std::string& error) {
    const auto staged=destination.parent_path()/boost::filesystem::unique_path("local-model-%%%%-%%%%.glb");
    bool owned=false;
    try {
        if(!is_model_artifact(source) || model_artifact_format(destination)!="glb" || boost::filesystem::exists(destination))
            throw std::runtime_error("请选择有效的 GLB 或 OBJ 模型；原文件不会被覆盖。");
        boost::filesystem::create_directories(destination.parent_path());
        if(model_artifact_format(source)=="glb") {boost::filesystem::copy_file(source,staged);owned=true;}
        else {
            TexturedMesh mesh;std::vector<RGBA> colors;
            if(!load_assimp_textured_model(source.string(),mesh,&error,&colors))throw std::runtime_error(error);
            if(mesh.indices.empty() || mesh.indices.size()>2000000 || mesh.vertices.size()>6000000 || colors.size()!=mesh.vertices.size())
                throw std::runtime_error("模型超过当前工作台支持的 200 万三角面。");
            using Json=nlohmann::json;
            Json doc={{"asset",{{"version","2.0"},{"generator","Orca local model import"}}},
                {"scene",0},{"scenes",{{{"nodes",{0}}}}},{"nodes",{{{"mesh",0}}}},
                {"bufferViews",Json::array()},{"accessors",Json::array()},{"materials",Json::array()},
                {"images",Json::array()},{"textures",Json::array()}};
            std::vector<unsigned char> bytes;
            auto view=[&](size_t offset) {
                const auto id=doc["bufferViews"].size();doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",bytes.size()-offset}});
                while(bytes.size()%4)bytes.push_back(0);
                if(bytes.size()>max_bytes)throw std::runtime_error("本地模型和贴图合计超过 512 MB。");return id;
            };
            auto accessor=[&](size_t offset,size_t count,const char* type,int component=5126) {
                const auto id=doc["accessors"].size();doc["accessors"].push_back({{"bufferView",view(offset)},{"componentType",component},{"count",count},{"type",type}});return id;
            };
            std::array<float,3> minimum{INFINITY,INFINITY,INFINITY},maximum{-INFINITY,-INFINITY,-INFINITY};
            for(const auto& v:mesh.vertices) {
                const std::array<float,3> p{v[0]*.001f,v[2]*.001f,-v[1]*.001f};
                for(size_t c=0;c<3;++c) {
                    if(!std::isfinite(p[c]))throw std::runtime_error("模型含无效坐标。");
                    append_float(bytes,p[c]);minimum[c]=std::min(minimum[c],p[c]);maximum[c]=std::max(maximum[c],p[c]);
                }
            }
            const size_t position=accessor(0,mesh.vertices.size(),"VEC3");doc["accessors"][position]["min"]=minimum;doc["accessors"][position]["max"]=maximum;
            size_t offset=bytes.size();
            for(const auto& c:colors)for(size_t k=0;k<4;++k) {
                if(!std::isfinite(c[k]) || c[k]<0 || c[k]>1)throw std::runtime_error("模型含无效颜色。");
                append_float(bytes,k==3?c[k]:linear(c[k]));
            }
            const size_t color=accessor(offset,colors.size(),"VEC4");offset=bytes.size();
            if(mesh.uvs.size()!=mesh.vertices.size())throw std::runtime_error("OBJ 贴图坐标不完整。");
            for(const auto& uv:mesh.uvs) {
                if(!std::isfinite(uv[0]) || !std::isfinite(uv[1]))throw std::runtime_error("OBJ 贴图坐标无效。");
                append_float(bytes,uv[0]);append_float(bytes,1.f-uv[1]);
            }
            const size_t uv=accessor(offset,mesh.uvs.size(),"VEC2");
            for(const auto& image:mesh.textures) {
                const auto& data=image.data;
                const bool png=data.size()>8 && data[0]==137 && data[1]==80 && data[2]==78 && data[3]==71;
                const bool jpeg=data.size()>2 && data[0]==255 && data[1]==216;
                if((!png && !jpeg) || data.size()>max_bytes-bytes.size())throw std::runtime_error("OBJ 请使用 PNG 或 JPEG 贴图，且总大小不超过 512 MB。");
                offset=bytes.size();bytes.insert(bytes.end(),data.begin(),data.end());
                const size_t image_id=doc["images"].size();
                doc["images"].push_back({{"bufferView",view(offset)},{"mimeType",png?"image/png":"image/jpeg"}});
                doc["textures"].push_back({{"source",image_id}});
            }
            for(size_t m=0;m<mesh.material_colors.size();++m) {
                auto factor=mesh.material_colors[m];for(size_t c=0;c<3;++c)factor[c]=linear(std::clamp(factor[c],0.f,1.f));
                Json pbr={{"baseColorFactor",factor},{"metallicFactor",0},{"roughnessFactor",1}};
                if(m<mesh.material_texture_map.size() && mesh.material_texture_map[m]>=0)
                    pbr["baseColorTexture"]={{"index",mesh.material_texture_map[m]}};
                doc["materials"].push_back({{"doubleSided",true},{"pbrMetallicRoughness",std::move(pbr)}});
            }
            std::map<int,std::vector<std::array<int,3>>> groups;
            for(size_t f=0;f<mesh.indices.size();++f)groups[f<mesh.material_ids.size()?mesh.material_ids[f]:-1].push_back(mesh.indices[f]);
            Json primitives=Json::array();
            for(const auto& group:groups) {
                offset=bytes.size();
                for(const auto& face:group.second)for(int v:face) {
                    if(v<0 || size_t(v)>=mesh.vertices.size())throw std::runtime_error("模型含无效三角面。");append_u32(bytes,uint32_t(v));
                }
                Json primitive={{"attributes",{{"POSITION",position},{"COLOR_0",color},{"TEXCOORD_0",uv}}},
                    {"indices",accessor(offset,group.second.size()*3,"SCALAR",5125)},{"mode",4}};
                if(group.first>=0 && size_t(group.first)<mesh.material_colors.size())primitive["material"]=group.first;
                primitives.push_back(std::move(primitive));
            }
            doc["meshes"]={{{"primitives",std::move(primitives)}}};doc["buffers"]={{{"byteLength",bytes.size()}}};
            std::string json=doc.dump();while(json.size()%4)json+=' ';
            const size_t size=28+json.size()+bytes.size();if(size>max_bytes)throw std::runtime_error("模型超过 512 MB。");
            boost::filesystem::ofstream out(staged,std::ios::binary);owned=true;
            write_u32(out,0x46546c67);write_u32(out,2);write_u32(out,uint32_t(size));
            write_u32(out,uint32_t(json.size()));write_u32(out,0x4e4f534a);out.write(json.data(),json.size());
            write_u32(out,uint32_t(bytes.size()));write_u32(out,0x004e4942);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());out.close();
            if(!out)throw std::runtime_error("无法写入本地模型，请检查磁盘空间。");
        }
        TriangleMesh verified;ObjInfo sampled;
        if(!load_model_artifact(staged,verified,sampled,error))throw std::runtime_error(error);
        // Publish without replacement; a failed validation leaves no history asset.
        boost::filesystem::create_hard_link(staged,destination);boost::filesystem::remove(staged);return true;
    }catch(const std::exception& e) {
        if(owned){boost::system::error_code ignored;boost::filesystem::remove(staged,ignored);}error=e.what();return false;
    }
}
} // namespace Slic3r::AI
