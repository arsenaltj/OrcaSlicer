#pragma once

#include <cassert>
#include <array>
#include <vector>
#include <functional>
#include <map>
#include <string>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include <boost/filesystem/path.hpp>

namespace Slic3r::AI {

// Display-only texture data keeps original face order; it never enters the printable mesh.
struct ModelArtifactTextureSurface {
    struct Image { int width{0}, height{0}; std::vector<unsigned char> rgba; };
    struct Corner { std::array<float,2> uv{}; std::array<float,4> multiplier{1,1,1,1}; };
    enum class AlphaMode { Opaque, Mask, Blend };
    struct Face {
        int image{-1}, wrap_s{10497}, wrap_t{10497}, min_filter{9729}, mag_filter{9729};
        AlphaMode alpha_mode{AlphaMode::Opaque};
        float alpha_cutoff{0.5f};
        std::array<Corner,3> corners;
    };
    std::vector<Image> images;
    std::vector<Face> faces;
};

// CPU-generated levels avoid the native driver mipmap path. Level zero stays original.
std::vector<ModelArtifactTextureSurface::Image> model_texture_mipmaps(const ModelArtifactTextureSurface::Image& image);

// AI editing uses Z-up millimetres and sRGB colors. GLB files use glTF's
// Y-up metres and linear COLOR_0; conversion occurs only at this boundary.
bool load_model_artifact(const boost::filesystem::path& path, TriangleMesh& mesh,
                         ObjInfo& colors, std::string& error, const std::function<bool()>& canceled = {},
                         ModelArtifactTextureSurface* texture_surface = nullptr);
bool write_model_artifact(const boost::filesystem::path& path, const indexed_triangle_set& mesh,
                          const std::vector<RGBA>& colors, std::string& error);
// Rebind original colors after coincident vertices were split for face colors.
// Requires exactly the same ordered face/corner positions; conflicting colors
// cannot be merged into one target vertex. No resampling or geometry change.
std::vector<RGBA> remap_model_vertex_colors(const indexed_triangle_set& original,
    const std::vector<RGBA>& colors, const indexed_triangle_set& target);
bool is_model_artifact(const boost::filesystem::path& path);
std::string model_artifact_format(const boost::filesystem::path& path);
std::string model_artifact_sha256(const boost::filesystem::path& path);
// A single process-local, verified GLB import copy. Persistent OBJ files are
// never trusted as a decoding shortcut until this process checks the GLB.
struct VerifiedGlbImportCopy {
    std::string source_sha256;
    boost::filesystem::path obj_path;
    std::string obj_sha256;
};
bool prepare_glb_obj_import(const boost::filesystem::path& source,
                            const boost::filesystem::path& cache_root,
                            VerifiedGlbImportCopy& verified,
                            boost::filesystem::path& output,
                            std::string& error,
                            bool& reused);
// Archive a local model as a self-contained GLB. Existing GLBs are copied
// exactly; OBJ positions use Orca's Z-up millimetres, with embedded PNG/JPEGs.
bool archive_local_model(const boost::filesystem::path& source,const boost::filesystem::path& destination,std::string& error);

} // namespace Slic3r::AI
