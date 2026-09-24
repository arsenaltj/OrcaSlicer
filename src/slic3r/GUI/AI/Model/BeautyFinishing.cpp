#include "ModelFinishing.hpp"
#include "BeautySurface.hpp"
#include "BeautyDocument.hpp"
#include "ModelArtifact.hpp"
#include "GlbGeometryEditing.hpp"
#include "SurfaceSelectionState.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace Slic3r::AI {
namespace {
// PR #15's puzzle persistence is intentionally deferred to the later
// LocalPrintColor/region transaction migration. This stage only needs the
// texture-preserving appearance path and the current result-window selection.
#if 0
void require_puzzle(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Publish a byte-identical base when only the partition changed or all paint
// was cleared. Never hard-link the base itself: future external modifications
// of a source model must not alter an accepted version.
void copy_puzzle_base(const boost::filesystem::path& source, const boost::filesystem::path& destination,
                      const std::string& expected_hash, const std::function<void()>& checkpoint) {
    const auto staging = destination.parent_path() / boost::filesystem::unique_path(".puzzle-%%%%-%%%%-%%%%");
    bool published = false, owns_staging = false;
    try {
        owns_staging = boost::filesystem::create_directory(staging);
        require_puzzle(owns_staging, "Cannot create a puzzle staging directory.");
        const auto temporary = staging / "base.glb";
        boost::filesystem::ifstream input(source, std::ios::binary);
        boost::filesystem::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        require_puzzle(bool(input) && bool(output), "Cannot copy the original puzzle texture.");
        std::array<char, 65536> buffer;
        while (input) {
            checkpoint();
            input.read(buffer.data(), std::streamsize(buffer.size()));
            if (input.gcount() > 0) output.write(buffer.data(), input.gcount());
        }
        require_puzzle(input.eof(), "Cannot completely read the original puzzle model.");
        input.close();
        output.close();
        require_puzzle(bool(output) && model_artifact_sha256(temporary) == expected_hash,
                       "The original puzzle model changed while copying.");
        checkpoint();
        boost::filesystem::create_hard_link(temporary, destination);
        published = true;
        checkpoint();
        boost::filesystem::remove(temporary);
        boost::filesystem::remove(staging);
    } catch (...) {
        boost::system::error_code ignored;
        if (published) boost::filesystem::remove(destination, ignored);
        if (owns_staging) {
            boost::filesystem::remove(staging / "base.glb", ignored);
            boost::filesystem::remove(staging, ignored);
        }
        throw;
    }
}

ModelFinishingResult finish_puzzle_artifact(const boost::filesystem::path& source,
    const boost::filesystem::path& destination, const ModelFinishingOptions& options,
    const std::function<bool()>& canceled) {
    ModelFinishingResult result;
    bool owns_output = false;
    auto checkpoint = [&] { if (canceled && canceled()) throw std::runtime_error("Puzzle edit cancelled."); };
    try {
        require_puzzle(!options.beauty_appearance && !options.beauty_deform && !options.smooth_surface &&
                       !options.repair_mesh && !options.recolor_selected && !options.clean_color_spots,
                       "Save puzzle editing separately from other beauty tools.");
        require_puzzle(model_artifact_format(source) == "glb" && model_artifact_format(destination) == "glb",
                       "Puzzle editing currently requires GLB models with embedded textures.");
        require_puzzle(!boost::filesystem::exists(destination) && !boost::filesystem::is_symlink(destination),
                       "Choose a new output model version.");
        checkpoint();
        const auto folder = boost::filesystem::canonical(source.parent_path());
        require_puzzle(boost::filesystem::canonical(destination.parent_path()) == folder,
                       "Save puzzle versions beside their original model.");
        const auto& record = options.beauty_document;
        require_puzzle(record.is_object(), "The puzzle edit record is missing.");
        const auto filename = record.at("puzzle_base_file").get<std::string>();
        const auto base_hash = record.at("puzzle_base_sha256").get<std::string>();
        const boost::filesystem::path relative(filename);
        require_puzzle(!filename.empty() && filename.size() <= 255 && filename.find_first_of("/\\:") == std::string::npos &&
                       relative.filename() == relative && model_artifact_format(relative) == "glb",
                       "The puzzle base must name a GLB in the same asset folder.");
        require_puzzle(base_hash.size() == 64 && std::all_of(base_hash.begin(), base_hash.end(), [](char c) {
                           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                       }), "The puzzle base fingerprint is invalid.");
        const auto base = folder / relative;
        require_puzzle(boost::filesystem::is_regular_file(base) && !boost::filesystem::is_symlink(base) &&
                       boost::filesystem::canonical(base).parent_path() == folder,
                       "The original puzzle model is unavailable.");
        require_puzzle(model_artifact_sha256(base) == base_hash,
                       "The original puzzle texture changed; reopen the original model before editing.");
        result.source_sha256 = model_artifact_sha256(source);
        require_puzzle(!result.source_sha256.empty(), "Cannot verify the source model.");
        TriangleMesh mesh, original;
        ObjInfo info, original_colors;
        std::string error;
        require_puzzle(load_model_artifact(source, mesh, info, error), "Cannot load the current puzzle model.");
        checkpoint();
        require_puzzle(load_model_artifact(base, original, original_colors, error), "Cannot load the original puzzle model.");
        const auto geometry = SurfaceSelectionPersistence::geometry_fingerprint(mesh.its);
        require_puzzle(SurfaceSelectionPersistence::geometry_fingerprint(original.its) == geometry &&
                       original.its.indices == mesh.its.indices && original.its.vertices == mesh.its.vertices,
                       "The original puzzle texture belongs to different geometry.");
        const auto puzzle = BeautyPuzzle::decode(record.at("puzzle"), geometry, mesh.its.indices.size());
        auto surface = options.beauty_surface;
        if (!surface) surface = BeautySurface::build(original.its, original_colors.vertex_colors, {}, canceled);
        puzzle.validate(*surface);
        checkpoint();
        result.faces_before = mesh.its.indices.size();
        result.vertices = mesh.its.vertices.size();
        if (puzzle.colors.empty()) {
            copy_puzzle_base(base, destination, base_hash, checkpoint);
            owns_output = true;
        } else {
            // Rebuild from validated region ownership, not stale UI face arrays.
            // Starting from the verified original also restores pixels released
            // by a moved boundary or by clearing a piece's color.
            BeautyAppearanceOptions appearance;
            appearance.face_weights.assign(puzzle.face_piece.size(), 0.f);
            appearance.face_target_colors.resize(puzzle.face_piece.size());
            for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
                if ((f & 4095) == 0) checkpoint();
                const auto color = puzzle.colors.find(puzzle.face_piece[f]);
                if (color == puzzle.colors.end()) continue;
                require_puzzle(std::abs(color->second[3] - 1.f) <= 1e-6f,
                               "Puzzle painting changes RGB only; use an opaque target color.");
                appearance.face_weights[f] = 1.f;
                for (size_t ch = 0; ch < 3; ++ch) appearance.face_target_colors[f][ch] = color->second[ch];
                ++result.recolored_faces;
            }
            const auto edited = edit_glb_appearance(base, destination, appearance, canceled);
            result.canceled = edited.canceled;
            require_puzzle(edited.success, edited.error.c_str());
            owns_output = true;
            result.changed_texture_pixels = edited.changed_pixels;
        }
        checkpoint();
        require_puzzle(model_artifact_sha256(source) == result.source_sha256 && model_artifact_sha256(base) == base_hash,
                       "A puzzle source changed while saving; reload the model.");
        TriangleMesh saved;
        ObjInfo saved_colors;
        require_puzzle(load_model_artifact(destination, saved, saved_colors, error), "Cannot reload the saved puzzle model.");
        require_puzzle(saved.its.indices == mesh.its.indices && saved.its.vertices == mesh.its.vertices,
                       "Puzzle editing changed the model geometry.");
        result.faces_after = saved.its.indices.size();
        const auto size = saved.bounding_box().size();
        for (size_t ch = 0; ch < 3; ++ch) result.dimensions[ch] = size[ch];
        result.output_sha256 = model_artifact_sha256(destination);
        require_puzzle(!result.output_sha256.empty(), "Cannot verify the saved puzzle model.");
        checkpoint();
        result.changed_edit_record = true;
        result.success = true;
        owns_output = false;
    } catch (const std::exception& e) {
        result.canceled = result.canceled || (canceled && canceled());
        result.error = e.what();
    }
    if (owns_output) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
    if (!result.success) {
        result.output_sha256.clear();
        result.changed_edit_record = false;
        result.changed_texture_pixels = 0;
        result.recolored_faces = 0;
    }
    return result;
}
#endif
} // namespace

ModelFinishingResult finish_beauty_artifact(const boost::filesystem::path& source,
    const boost::filesystem::path& destination,const ModelFinishingOptions& options,
    const std::function<bool()>& canceled)
{
    ModelFinishingResult result;
    bool owns_output=false;
    auto checkpoint=[&]{if(canceled && canceled())throw std::runtime_error("Beauty edit cancelled.");};
    try {
        if(options.beauty_appearance==options.beauty_deform || options.smooth_surface || options.repair_mesh ||
            options.recolor_selected || options.clean_color_spots)
            throw std::runtime_error("Choose either local appearance or the geometry trial.");
        if(model_artifact_format(source)!="glb" || model_artifact_format(destination)!="glb")
            throw std::runtime_error("The new beauty tools currently require a GLB model with embedded materials.");
        if(boost::filesystem::exists(destination))throw std::runtime_error("Choose a new output model version.");
        checkpoint();
        result.source_sha256=model_artifact_sha256(source);
        TriangleMesh mesh;ObjInfo info;std::string error;
        if(!load_model_artifact(source,mesh,info,error))throw std::runtime_error(error);
        const auto geometry=SurfaceSelectionPersistence::geometry_fingerprint(mesh.its);
        const auto document=BeautyDocument::decode(options.beauty_document,geometry,mesh.its.indices.size());
        std::vector<uint8_t> selected(mesh.its.indices.size(),0);
        for(size_t f:options.selected_faces) {
            if(f>=selected.size())throw std::runtime_error("Beauty selection belongs to another model.");selected[f]=1;
        }
        if(options.selected_faces.empty())throw std::runtime_error("Select a local region before editing.");
        result.faces_before=mesh.its.indices.size();result.vertices=mesh.its.vertices.size();
        checkpoint();
        auto protection=document.protection();
        if(!options.beauty_protected_faces.empty()) {
            if(options.beauty_protected_faces.size()!=selected.size())throw std::runtime_error("Protection belongs to another model.");
            for(size_t f=0;f<selected.size();++f) {
                if(options.beauty_protected_faces[f]>1)throw std::runtime_error("Invalid protection mask.");
                protection[f]|=options.beauty_protected_faces[f];
            }
        }
        auto surface=options.beauty_surface;
        if(surface && (surface->geometry_id!=geometry || surface->face_patch!=document.face_patch))
            throw std::runtime_error("The cached beauty surface belongs to another model.");
        if(!surface)surface=BeautySurface::build(mesh.its,info.vertex_colors,document.face_patch,canceled);
        if(options.beauty_appearance) {
            auto appearance=options.appearance;
            if(appearance.face_weights.empty())appearance.face_weights=surface->face_weights(mesh.its,selected,protection,options.beauty_feather_mm);
            if(appearance.face_weights.size()!=selected.size())throw std::runtime_error("Appearance mask is not ready.");
            for(size_t f=0;f<selected.size();++f)if((!selected[f] || protection[f]) && appearance.face_weights[f]!=0)
                throw std::runtime_error("Appearance weights extend outside the selected surface.");
            const auto edited=edit_glb_appearance(source,destination,appearance,canceled);
            if(!edited.success) {result.canceled=edited.canceled;throw std::runtime_error(edited.error);}
            owns_output=true;result.changed_texture_pixels=edited.changed_pixels;
        } else {
            const auto original=read_glb_geometry_source(source,mesh.its,checkpoint);
            mesh.its=surface->deform(mesh.its,selected,protection,options.beauty_displacement_mm,
                options.beauty_falloff_mm,result.moved_vertices,canceled);
            checkpoint();
            write_glb_geometry_edit(*original,destination,mesh.its,options.selected_faces,checkpoint);
            owns_output=true;
        }
        checkpoint();
        if(model_artifact_sha256(source)!=result.source_sha256)throw std::runtime_error("Source changed while editing; reload it.");
        TriangleMesh saved;ObjInfo saved_colors;
        if(!load_model_artifact(destination,saved,saved_colors,error))throw std::runtime_error(error);
        if(saved.its.indices!=mesh.its.indices || saved.its.vertices.size()!=mesh.its.vertices.size())
            throw std::runtime_error("The edited artifact changed its triangle correspondence.");
        if(options.beauty_appearance && saved.its.vertices!=mesh.its.vertices)
            throw std::runtime_error("Appearance editing changed the geometry.");
        result.faces_after=saved.its.indices.size();
        const auto size=saved.bounding_box().size();
        for(int i=0;i<3;++i)result.dimensions[i]=size[i];
        result.output_sha256=model_artifact_sha256(destination);
        checkpoint();result.success=true;owns_output=false;
    } catch(const std::exception& e) {
        result.canceled=result.canceled || (canceled && canceled());result.error=e.what();
    }
    if(owns_output) {boost::system::error_code ignored;boost::filesystem::remove(destination,ignored);}
    return result;
}
} // namespace Slic3r::AI
