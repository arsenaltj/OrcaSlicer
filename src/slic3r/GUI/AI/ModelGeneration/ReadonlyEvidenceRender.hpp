#pragma once

#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <set>
#include <string>

namespace Slic3r::GUI {

struct ReadonlyEvidenceRenderResult {
    boost::filesystem::path output;
    size_t view_count {0};
    size_t face_count {0};
    int image_size {0};
};

// Render the exact imported mesh into a new cache directory. This writes only
// disposable evidence and never changes model colors, semantic state, or a
// material tree. The caller must provide a new, non-existing output directory.
inline bool write_readonly_evidence_render_package(
    const AI::SemanticColoring::MeshSnapshot& source,
    const boost::filesystem::path& output,
    const std::string& source_sha256,
    int image_size,
    ReadonlyEvidenceRenderResult& result,
    std::string& error,
    const AI::SemanticColoring::Cancel& cancel = {})
{
    using namespace AI::SemanticColoring;
    error.clear();
    result = {};
    if (source.mesh.indices.empty() || source.geometry_id.empty() || source.content_id.empty()) {
        error = "当前模型没有可用的原始网格身份";
        return false;
    }
    if (source_sha256.size() != 64 || image_size < 64 || image_size > 2048) {
        error = "证据渲染输入身份或尺寸无效";
        return false;
    }
    boost::system::error_code fs_error;
    if (!boost::filesystem::create_directory(output, fs_error)) {
        error = fs_error ? "无法创建证据输出目录: " + fs_error.message() :
            "证据输出目录已存在，未覆盖已有运行";
        return false;
    }
    constexpr std::array<float, 6> yaws {{0.f, 45.f, 90.f, 135.f, 180.f, -135.f}};
    nlohmann::json views = nlohmann::json::array();
    const auto write_bytes = [&](const boost::filesystem::path& path, const void* data, size_t bytes) {
        std::ofstream stream(path.string(), std::ios::binary);
        if (!stream) return false;
        stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        return bool(stream);
    };
    const auto write_ppm = [&](const boost::filesystem::path& path, const RGBImage& image) {
        std::ofstream stream(path.string(), std::ios::binary);
        if (!stream || !image.valid()) return false;
        stream << "P6\n" << image.width << ' ' << image.height << "\n255\n";
        stream.write(reinterpret_cast<const char*>(image.pixels.data()),
                     static_cast<std::streamsize>(image.pixels.size()));
        return bool(stream);
    };
    const auto yaw_name = [](float yaw) {
        const int rounded = static_cast<int>(std::lround(yaw));
        return rounded < 0 ? std::string("oblique-m") + std::to_string(-rounded)
                           : std::string("oblique-p") + std::to_string(rounded);
    };
    try {
        for (float yaw : yaws) {
            if (cancel && cancel()) { error = "证据渲染已取消"; return false; }
            const auto view = render_view(source, yaw, image_size, cancel);
            if (view.canceled || !view.error.empty() || !view.image.valid() ||
                view.face_ids.size() != view.depth.size() ||
                view.face_ids.size() != view.barycentric.size()) {
                error = view.error.empty() ? "证据渲染未完成" : view.error;
                return false;
            }
            const std::string name = yaw_name(yaw);
            const std::string render_id = "orca-readonly-render-v1-" + name;
            const auto ppm = output / (name + "-color.ppm");
            const auto faces = output / (name + "-face-id.u32");
            const auto bary = output / (name + "-barycentric.f32");
            const auto depth = output / (name + "-depth.f32");
            const auto visibility = output / (name + "-visibility.u8");
            if (!write_ppm(ppm, view.image) ||
                !write_bytes(faces, view.face_ids.data(), view.face_ids.size() * sizeof(uint32_t)) ||
                !write_bytes(bary, view.barycentric.data(), view.barycentric.size() * sizeof(Barycentric)) ||
                !write_bytes(depth, view.depth.data(), view.depth.size() * sizeof(float))) {
                error = "证据渲染缓冲写入失败";
                return false;
            }
            std::vector<uint8_t> visible(view.face_ids.size(), 0);
            size_t visible_pixels = 0;
            std::set<uint32_t> visible_faces;
            for (size_t index = 0; index < view.face_ids.size(); ++index) {
                if (view.face_ids[index] == UINT32_MAX || !std::isfinite(view.depth[index])) continue;
                visible[index] = 1;
                ++visible_pixels;
                visible_faces.insert(view.face_ids[index]);
            }
            if (!write_bytes(visibility, visible.data(), visible.size())) {
                error = "证据可见性缓冲写入失败";
                return false;
            }
            views.push_back({
                {"view_id", name}, {"render_id", render_id}, {"yaw_degrees", yaw},
                {"projection", "orthographic"}, {"model_transform", "identity-source-mesh"},
                {"image_width", image_size}, {"image_height", image_size},
                {"color", ppm.filename().string()}, {"face_id", faces.filename().string()},
                {"barycentric", bary.filename().string()}, {"depth", depth.filename().string()},
                {"visibility", visibility.filename().string()},
                {"visible_pixels", visible_pixels}, {"visible_faces", visible_faces.size()}
            });
        }
        const nlohmann::json manifest {
            {"schema", "orca.readonly-render-package/v1"},
            {"status", "RENDERED_PENDING_PROVIDER"},
            {"renderer", "semantic-coloring-cpu-z-buffer"},
            {"source_sha256", source_sha256}, {"geometry_id", source.geometry_id},
            {"source_appearance_sha256", source.content_id},
            {"face_count", source.mesh.indices.size()}, {"image_size", image_size},
            {"views", views},
            {"candidate_slot", nullptr}, {"material_write_authorized", false},
            {"material_tree_changed", false}
        };
        std::ofstream manifest_stream((output / "render-manifest.json").string(), std::ios::binary);
        if (!manifest_stream) { error = "证据 manifest 写入失败"; return false; }
        manifest_stream << manifest.dump(2) << '\n';
        if (!manifest_stream) { error = "证据 manifest 写入失败"; return false; }
        result.output = output;
        result.view_count = views.size();
        result.face_count = source.mesh.indices.size();
        result.image_size = image_size;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace Slic3r::GUI
