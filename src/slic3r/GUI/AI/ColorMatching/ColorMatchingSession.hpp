#pragma once

#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorMatching.hpp"
#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"
#include "libslic3r/Model.hpp"
#include <atomic>
#include <thread>

namespace Slic3r::GUI {

// UI-neutral lifetime and draft. Orca target identity is a value, not a Plater.
// The view owns the main-thread native transaction and supplies fresh evidence.
class ColorMatchingSession final {
public:
    struct Draft {
        AI::GeneratedModelArtifact artifact;
        ObjectID target_volume;
        std::string model_name, target_source;
        boost::filesystem::path workbench_source_path;
        ObjectBase::Timestamp target_paint_stamp {0}, target_config_stamp {0};
        AI::PrintablePaletteSnapshot snapshot;
        LocalPrintColorMatching::Input input;
        AI::LocalPrintColorResult initial, computed;
        indexed_triangle_set source_mesh, native_mesh;
        std::vector<RGBA> source_colors;
        std::vector<AI::LocalPrintColorResult> undo, redo;
        bool has_result {false}, restored_confirmation {false};
    };
    struct Operation {
        std::thread worker;
        std::atomic<bool> cancel {false};
        bool busy {false}, stopped {false}, layer_busy {false};
        uint64_t revision {0};
    };
    ~ColorMatchingSession() { shutdown(); }
    ColorMatchingSession() = default;
    ColorMatchingSession(const ColorMatchingSession&) = delete;
    ColorMatchingSession& operator=(const ColorMatchingSession&) = delete;
    Draft& draft() { return m_draft; } // The single UI-thread owner edits this draft.
    const Draft& draft() const { return m_draft; }
    Operation& operation() { return m_operation; }
    bool editable() const { return !m_operation.busy && !m_operation.stopped; }
    void shutdown() {
        m_operation.stopped = true; ++m_operation.revision; m_operation.cancel = true;
        if (m_operation.worker.joinable()) m_operation.worker.join();
    }
    void checkpoint() {
        if (!editable()) return;
        m_draft.undo.push_back(m_draft.input.identity); m_draft.redo.clear();
    }
    bool restore(bool redo) {
        auto& from = redo ? m_draft.redo : m_draft.undo;
        auto& to = redo ? m_draft.undo : m_draft.redo;
        if (!editable() || from.empty()) return false;
        to.push_back(m_draft.input.identity); m_draft.input.identity = std::move(from.back()); from.pop_back();
        m_draft.has_result = false; m_draft.restored_confirmation = false;
        return true;
    }
    bool publish(uint64_t revision, const std::string& source, const std::string& geometry,
                 LocalPrintColorMatching::Computation result) {
        if (m_operation.stopped || m_operation.cancel || revision != m_operation.revision || !result.ok() ||
            m_draft.input.identity.source_sha256 != source || m_draft.input.identity.geometry_id != geometry ||
            result.result.source_sha256 != source || result.result.geometry_id != geometry) return false;
        m_draft.computed = std::move(result.result); m_draft.has_result = true;
        return true;
    }
    bool can_apply(const AI::PrintablePaletteSnapshot& current, size_t requested, bool original,
                   bool accept_error) const {
        const auto& value = m_draft.computed;
        return editable() && m_draft.has_result && !original && requested == value.requested_color_count &&
            current.material_fingerprint == value.material_fingerprint && current.process_fingerprint == value.process_fingerprint &&
            !value.targets.empty() && std::all_of(value.targets.begin(), value.targets.end(),
                [](const auto& target) { return target.executable && !target.recipe; }) &&
            (value.unresolved_count() == 0 || accept_error);
    }
private:
    Draft m_draft;
    Operation m_operation;
};

} // namespace Slic3r::GUI
