#pragma once

#include <cassert>
#include <functional>
#include <map>
#include <string>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include <boost/filesystem/path.hpp>

namespace Slic3r::AI {

// AI editing uses Z-up millimetres and sRGB colors. GLB files use glTF's
// Y-up metres and linear COLOR_0; conversion occurs only at this boundary.
bool load_model_artifact(const boost::filesystem::path& path, TriangleMesh& mesh,
                         ObjInfo& colors, std::string& error, const std::function<bool()>& canceled = {});
bool write_model_artifact(const boost::filesystem::path& path, const indexed_triangle_set& mesh,
                          const std::vector<RGBA>& colors, std::string& error);
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
