#include "FixModelByCgal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <cstddef>
#include <exception>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/log/trivial.hpp>

#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/MeshSeamRepair.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/format.hpp"
#include "libslic3r/Thread.hpp"
#include "../GUI/I18N.hpp"
#include "../GUI/Widgets/ProgressDialog.hpp"

// Orca: This file provides utilities for repairing 3D model meshes using the CGAL library, handling mesh splitting, merging, and boolean operations.

namespace Slic3r {

namespace {

constexpr size_t invalid_repair_index = std::numeric_limits<size_t>::max();

class RepairStageTimer
{
public:
    RepairStageTimer(const char *stage, size_t volume_idx = invalid_repair_index,
                     size_t part_idx = invalid_repair_index,
                     const std::atomic<bool> *canceled = nullptr,
                     size_t vertices = 0, size_t faces = 0)
        : m_stage(stage), m_volume_idx(volume_idx), m_part_idx(part_idx),
          m_canceled(canceled), m_vertices(vertices), m_faces(faces),
          m_started(std::chrono::steady_clock::now())
    {
        BOOST_LOG_TRIVIAL(info) << "[CgalRepair] begin stage=" << m_stage
            << " volume=" << log_index(m_volume_idx)
            << " part=" << log_index(m_part_idx)
            << " vertices=" << m_vertices
            << " faces=" << m_faces;
    }

    ~RepairStageTimer() { finish(); }

    void finish(size_t vertices = invalid_repair_index, size_t faces = invalid_repair_index)
    {
        if (m_finished)
            return;
        if (vertices != invalid_repair_index)
            m_vertices = vertices;
        if (faces != invalid_repair_index)
            m_faces = faces;

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_started).count();
        BOOST_LOG_TRIVIAL(info) << "[CgalRepair] end stage=" << m_stage
            << " elapsed_ms=" << elapsed_ms
            << " volume=" << log_index(m_volume_idx)
            << " part=" << log_index(m_part_idx)
            << " vertices=" << m_vertices
            << " faces=" << m_faces
            << " cancel_requested=" << (m_canceled && m_canceled->load());
        m_finished = true;
    }

private:
    static long long log_index(size_t index)
    {
        return index == invalid_repair_index ? -1LL : static_cast<long long>(index);
    }

    const char                    *m_stage;
    size_t                         m_volume_idx;
    size_t                         m_part_idx;
    const std::atomic<bool>       *m_canceled;
    size_t                         m_vertices;
    size_t                         m_faces;
    std::chrono::steady_clock::time_point m_started;
    bool                           m_finished = false;
};

// Orca: Helper functions for analyzing mesh properties and transformations.

bool is_not_3dimensional_part(const TriangleMesh &mesh)
{
    // Orca: Determines if a mesh is degenerate or represents a non-3dimensional part by checking volume and bounding box dimensions.
    if (mesh.its.indices.empty())
        return true;

    indexed_triangle_set tmp = mesh.its;
    its_remove_degenerate_faces(tmp, true);
    if (tmp.indices.empty())
        return true;

    const BoundingBoxf3 bbox = mesh.bounding_box();
    const Vec3d size = bbox.size();
    const double min_dim = std::min(size.x(), std::min(size.y(), size.z()));
    const double max_dim = std::max(size.x(), std::max(size.y(), size.z()));
    if (min_dim <= EPSILON)
        return true;

    const double volume = std::abs(its_volume(mesh.its));
    const double bbox_volume = size.x() * size.y() * size.z();
    if (volume <= EPSILON)
        return true;

    const double min_relative_thickness = 1e-6;
    const double min_volume_ratio = 1e-6;
    if (min_dim / max_dim <= min_relative_thickness)
        return true;
    if (bbox_volume > 0.0 && volume / bbox_volume <= min_volume_ratio)
        return true;

    return false;
}

} // namespace

// Orca: Exception class for handling user-initiated cancellation of model repair operations.
class RepairCanceledException : public std::exception {
public:
    const char* what() const noexcept override { return "Model repair has been canceled"; }
};

// Orca: Main function to repair model objects using CGAL, with progress dialog and cancellation support.
// Returns false if fixing was canceled. fix_result contains error message if failed.
bool fix_model_with_cgal_gui(ModelObject &original_object, int volume_idx, GUI::ProgressDialog &progress_dialog, const wxString &msg_header, std::string &fix_result, bool keep_painting, bool* painting_removed, bool* modified)
{
    if (painting_removed)
        *painting_removed = false;
    if (modified)
        *modified = false;
    // Hold SaveObjectGaurd to prevent backup manager from racing concurrent mesh mutations (use-after-free).
    SaveObjectGaurd backup_gaurd(original_object);

    std::atomic<bool> canceled = false;
    std::atomic<bool> finished = false;
    RepairStageTimer total_timer("repair_model_total", invalid_repair_index, invalid_repair_index, &canceled);

    // Repair may split/delete volumes before it succeeds. Keep both geometry
    // and paint private until the entire operation completes without canceling.
    Model repair_model;
    ModelObject *model_object_ptr = nullptr;
    {
        RepairStageTimer stage("copy_model_to_staging", invalid_repair_index, invalid_repair_index, &canceled);
        model_object_ptr = repair_model.add_object(original_object);
        stage.finish();
        BOOST_LOG_TRIVIAL(info) << "[CgalRepair] staging_result volumes=" << model_object_ptr->volumes.size();
    }
    ModelObject& model_object = *model_object_ptr;

    // Orca: Synchronization primitives for progress updates between worker thread and GUI.
    std::mutex mtx;
    std::condition_variable condition;
    struct Progress {
        std::string message;
        int         percent  = 0;
        bool        updated  = false;
    } progress;

    bool   success = false;
    bool   staged_paint_removed = false;
    bool   staged_modified = false;
    size_t ivolume = 0;

    // Orca: Lambda for updating progress from worker thread.
    auto on_progress = [&mtx, &condition, &ivolume, &model_object, &progress](const std::string &msg, unsigned prcnt) {
        std::unique_lock<std::mutex> lock(mtx);
        progress.message = msg;
        const size_t total = std::max<size_t>(1, model_object.volumes.size());
        progress.percent = int(std::floor((float(prcnt) + float(ivolume) * 100.f) / float(total)));
        progress.updated = true;
        condition.notify_all();
    };

    // Orca: Worker thread that performs the actual model repair operations.
    auto worker_thread = std::thread([&model_object, volume_idx, &ivolume, on_progress, &success, &canceled, &finished, &fix_result, &staged_paint_removed, &staged_modified, keep_painting]() {
        try {
	        set_current_thread_name("cgal_fix_model");

            size_t start_volume = volume_idx == -1 ? 0 : size_t(volume_idx);
            size_t end_volume   = volume_idx == -1 ? std::numeric_limits<size_t>::max() : size_t(volume_idx);

            for (ivolume = start_volume; ivolume < model_object.volumes.size(); ++ivolume) {
                if (volume_idx != -1 && ivolume > end_volume)
                    break;
                if (canceled)
                    throw RepairCanceledException();

                on_progress(_u8L("Repairing model object"), 10);

                ModelVolume *volume = model_object.volumes[ivolume];
                if (volume->is_painted_modifier())
                    continue;
                // A painted modifier follows the complete host mesh. Let the
                // official repair handle that host without fragmenting it.
                const bool has_painted_modifiers = std::any_of(model_object.volumes.begin(), model_object.volumes.end(),
                    [volume](const ModelVolume* candidate) {
                        return candidate->is_painted_modifier() && candidate->painted_modifier_host == volume->id();
                    });
                TriangleMesh stitched_mesh;
                {
                    RepairStageTimer stage("copy_volume_mesh", ivolume, invalid_repair_index, &canceled);
                    stitched_mesh = volume->mesh();
                    stage.finish(stitched_mesh.its.vertices.size(), stitched_mesh.its.indices.size());
                }

                bool exact_seams_stitched = false;
                {
                    RepairStageTimer stage("stitch_exact_mesh_seams", ivolume, invalid_repair_index, &canceled,
                                           stitched_mesh.its.vertices.size(), stitched_mesh.its.indices.size());
                    exact_seams_stitched = stitch_exact_mesh_seams(stitched_mesh);
                }
                if (exact_seams_stitched) {
                    staged_modified = true;
                    volume->set_mesh(std::move(stitched_mesh));
                    volume->set_new_unique_id();
                    // No faces moved or changed order; keep the seam painting.
                    // The official repair below still checks orientation and topology.
                }
                // A valid volume keeps its identity and painting even when the
                // repair preference would discard paint on a changed mesh.

                // Orca: Split splittable volumes into parts for individual processing.
                size_t parts_count = 1;
                bool is_splittable = false;
                {
                    RepairStageTimer stage("check_volume_splittable", ivolume, invalid_repair_index, &canceled,
                                           stitched_mesh.its.vertices.size(), stitched_mesh.its.indices.size());
                    is_splittable = !exact_seams_stitched && !has_painted_modifiers && volume->is_splittable();
                }
                if (canceled)
                    throw RepairCanceledException();
                if (is_splittable) {
                    RepairStageTimer stage("split_volume", ivolume, invalid_repair_index, &canceled,
                                           stitched_mesh.its.vertices.size(), stitched_mesh.its.indices.size());
                    const ObjectID before_id = volume->id();
                    const bool had_painting = volume->is_any_painted();
                    parts_count = volume->split(1, keep_painting);
                    if (volume->id() != before_id) {
                        staged_modified = true;
                        staged_paint_removed |= !keep_painting && had_painting;
                    }
                    BOOST_LOG_TRIVIAL(info) << "[CgalRepair] split_result volume=" << ivolume
                        << " parts=" << parts_count;
                    if (parts_count > 1) {
                        const std::string msg = Slic3r::format(L("Split into %1% parts"), parts_count);
                        on_progress(msg, 10);
                    }
                }
                if (canceled)
                    throw RepairCanceledException();

                size_t part_end = std::min(ivolume + parts_count - 1, model_object.volumes.size() - 1);
                if (volume_idx != -1)
                    end_volume = part_end;

                size_t removed_parts = 0;
                {
                    RepairStageTimer stage("remove_degenerate_split_parts", ivolume, invalid_repair_index, &canceled);
                    for (size_t idx = part_end + 1; idx > ivolume; --idx) {
                        const size_t part_idx = idx - 1;
                        const ModelVolume *part_volume = model_object.volumes[part_idx];
                        if (has_painted_modifiers || !is_not_3dimensional_part(part_volume->mesh()))
                            continue;

                        staged_modified = true;
                        model_object.delete_volume(part_idx);
                        ++removed_parts;
                        if (part_end > 0)
                            --part_end;
                        else
                            part_end = 0;
                        if (volume_idx != -1)
                            end_volume = part_end;
                    }
                }
                BOOST_LOG_TRIVIAL(info) << "[CgalRepair] split_cleanup volume=" << ivolume
                    << " removed_parts=" << removed_parts << " remaining_parts=" << (parts_count - removed_parts);

                if (removed_parts >= parts_count) {
                    ivolume = part_end;
                    on_progress(_u8L("Repair finished"), 100);
                    continue;
                }

                for (size_t part_idx = ivolume; part_idx <= part_end && part_idx < model_object.volumes.size(); ++part_idx) {
                    if (canceled)
                        throw RepairCanceledException();
                    on_progress(_u8L("Repairing model object"), unsigned(100 * (part_idx - ivolume)));
                    ModelVolume *part_volume = model_object.volumes[part_idx];
                    const bool had_painting = part_volume->is_any_painted();
                    MeshRepairStats stats;
                    std::string error;
                    RepairStageTimer stage("repair_part_mesh", ivolume, part_idx, &canceled,
                                           part_volume->mesh().its.vertices.size(), part_volume->mesh().its.indices.size());
                    // Run the official volume repair on private staging. In particular,
                    // closed meshes may still need orientation or nonmanifold repairs.
                    if (!part_volume->repair(keep_painting, &stats, &error, [&](int percent) {
                        if (canceled)
                            throw RepairCanceledException();
                        const size_t count = std::max<size_t>(1, part_end - ivolume + 1);
                        on_progress(_u8L("Repairing model object"),
                                    unsigned((100 * (part_idx - ivolume) + size_t(percent)) / count));
                    })) {
                        if (canceled)
                            throw RepairCanceledException();
                        throw Slic3r::RuntimeError(error.empty() ? _u8L("Repair failed") : error);
                    }
                    if (canceled)
                        throw RepairCanceledException();
                    staged_modified |= stats.modified;
                    staged_paint_removed |= stats.modified && !keep_painting && had_painting;
                    on_progress(_u8L("Repairing model object"), unsigned(100 * (part_idx - ivolume + 1)));
                }

                ivolume = part_end;

                on_progress(_u8L("Repair finished"), 100);
            }

            if (staged_modified) {
                model_object.sync_painted_modifiers();
                model_object.invalidate_bounding_box();
            }

            if (ivolume > 0)
                --ivolume;
            on_progress(_u8L("Repair finished"), 100);
            success = true;
            finished = true;
        } catch (RepairCanceledException &) {
            canceled = true;
            finished = true;
            on_progress(_u8L("Repair canceled"), 100);
        } catch (std::exception &ex) {
            success = false;
            finished = true;
            fix_result = ex.what();
            on_progress(ex.what(), 100);
        }
    });

    // Orca: Main GUI loop to update progress dialog and handle cancellation.
    while (!finished) {
        std::unique_lock<std::mutex> lock(mtx);
        condition.wait_for(lock, std::chrono::milliseconds(250), [&progress]{ return progress.updated; });

        // Decrease progress percent slightly to avoid auto-closing.
        if (!progress_dialog.Update(progress.percent - 1, msg_header + _(progress.message))) {
            BOOST_LOG_TRIVIAL(info) << "[CgalRepair] cancel_requested source=progress_dialog";
            canceled = true;
        } else
            progress_dialog.Fit();

        progress.updated = false;
    }

    if (canceled) {
        // Nothing to show.
    } else if (success) {
        fix_result.clear();
    }

    if (worker_thread.joinable())
        worker_thread.join();

    BOOST_LOG_TRIVIAL(info) << "[CgalRepair] worker_joined canceled=" << canceled.load()
        << " success=" << success << " error=" << fix_result;

    if (!canceled && success && staged_modified) {
        RepairStageTimer stage("commit_staged_model", invalid_repair_index, invalid_repair_index, &canceled);
        const size_t original_volume_count = original_object.volumes.size();
        try {
            for (const ModelVolume* volume : model_object.volumes)
                original_object.add_volume(*volume);
        } catch (...) {
            while (original_object.volumes.size() > original_volume_count)
                original_object.delete_volume(original_object.volumes.size() - 1);
            throw;
        }
        for (size_t i = 0; i < original_volume_count; ++i)
            original_object.delete_volume(0);
        // Preserve transforms changed during staging, including the sole-volume
        // transform that delete_volume folds into instances during this commit.
        for (size_t i = 0; i < model_object.instances.size(); ++i) {
            Geometry::Transformation transformation = model_object.instances[i]->get_transformation();
            if (model_object.volumes.size() == 1)
                transformation = Geometry::Transformation(transformation.get_matrix() *
                    model_object.volumes.front()->get_transformation().get_matrix());
            original_object.instances[i]->set_transformation(transformation);
        }
        original_object.invalidate_bounding_box();
        if (painting_removed)
            *painting_removed = staged_paint_removed;
        if (modified)
            *modified = true;
        stage.finish();
        BOOST_LOG_TRIVIAL(info) << "[CgalRepair] commit_result volumes=" << original_object.volumes.size();
    }

    return !canceled;
}

} // namespace Slic3r
