#pragma once

#include <boost/filesystem/path.hpp>
#include <atomic>
#include <string>
#include <utility>

namespace Slic3r::GUI {

// A preparation result belongs to the asset visible when its worker started.
// Cancellation is sticky, including when the user switches away and back.
struct BeautyPreparationTicket {
    BeautyPreparationTicket(boost::filesystem::path model, std::string geometry_id)
        : model(std::move(model)), geometry_id(std::move(geometry_id)) {}

    void cancel_if_changed(const boost::filesystem::path& current_model,
                           const std::string& current_geometry) {
        if (model != current_model || geometry_id != current_geometry)
            canceled = true;
    }

    bool accepts(const boost::filesystem::path& current_model,
                 const std::string& workbench_geometry,
                 const std::string& preview_geometry, bool selection_ready = true) const {
        return selection_ready && !canceled.load() && model == current_model &&
               geometry_id == workbench_geometry && geometry_id == preview_geometry;
    }

    // Completed beauty data stays private until selection initialization succeeds.
    // Obsolete/canceled tasks can be retired without waiting for the new editor.
    bool waits_for_selection(bool selection_ready, bool selection_failed,
        const boost::filesystem::path& current_model, const std::string& workbench_geometry,
        const std::string& preview_geometry) const {
        return !selection_ready && !selection_failed &&
               accepts(current_model, workbench_geometry, preview_geometry);
    }

    const boost::filesystem::path model;
    const std::string geometry_id;
    std::atomic<bool> canceled {false};
};

} // namespace Slic3r::GUI
