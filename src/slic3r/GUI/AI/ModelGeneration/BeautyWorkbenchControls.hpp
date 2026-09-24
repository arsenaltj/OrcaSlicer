#pragma once

#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include <boost/filesystem/path.hpp>
#include <wx/panel.h>
#include <functional>
#include <memory>
#include <string>

class wxButton;
class wxChoice;
class wxStaticText;

namespace Slic3r::GUI {
class ModelPreview3D;

// Adapter for the existing result-window finishing controls. It deliberately
// does not own or replace ModelPreview3D: selection, semantic preview and
// manual face overrides stay in the current window implementation.
class BeautyWorkbenchControls final : public wxPanel {
public:
    BeautyWorkbenchControls(wxWindow* parent, ModelPreview3D* preview,
                            AI::IPrintablePaletteProvider& palette,
                            std::function<void()> layout_changed);
    ~BeautyWorkbenchControls() override = default;

    void synchronize(const boost::filesystem::path& source, bool editable, bool visible);
    void mark_saved();
    void set_dirty(bool dirty) { m_dirty = dirty; update_text(); }
    bool has_changes() const { return m_dirty; }
    bool ready() const { return m_ready; }
    void prepare_options(AI::ModelFinishingOptions& options,
                         const AI::SurfaceSelectionPersistence::SelectionState& selection) const;
    static nlohmann::json accepted_document(const AI::ModelFinishingOptions& options,
                                            const std::string& output_geometry);
    static void prepare_import(const boost::filesystem::path&, AI::ModelImportRequest&) {}

    std::function<void()> on_boundary_adjust;
    std::function<void()> on_undo;
    std::function<void()> on_redo;
    std::function<void()> on_save;
    std::function<size_t(const std::string&)> on_auto_match;
    std::function<void()> on_reoptimize;

private:
    void update_text();
    ModelPreview3D* m_preview {nullptr};
    AI::IPrintablePaletteProvider& m_palette;
    std::function<void()> m_layout_changed;
    wxStaticText* m_status {nullptr};
    wxChoice* m_auto_region {nullptr};
    wxButton* m_auto_match {nullptr};
    wxButton* m_reoptimize {nullptr};
    wxButton* m_boundary {nullptr};
    wxButton* m_undo {nullptr};
    wxButton* m_redo {nullptr};
    wxButton* m_save {nullptr};
    boost::filesystem::path m_source;
    std::string m_geometry_id;
    std::shared_ptr<const AI::BeautySurface> m_surface;
    AI::BeautyDocument m_document;
    bool m_editable {false};
    bool m_visible {false};
    bool m_ready {false};
    bool m_dirty {false};
};
}
