#pragma once

#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "libslic3r/Format/OBJ.hpp"

#include <functional>

namespace Slic3r::GUI {

class Plater;
struct TextureImportOptions;

// Target colors and physical feed slots are separate decisions. Shared by the
// native handoff and its regression checks; does not mutate a project.
TextureImportOptions model_import_color_options(const AI::ModelImportRequest& request);
ObjImportColorFn workbench_obj_color_mapper(Plater* plater, AI::ImportColorMode mode,
    const AI::PrintablePaletteSnapshot& palette, AI::ModelImportResult& result, bool& cancelled);

// Anti-corruption layer between AI application contracts and Orca workspace
// implementation details. No Orca type crosses either public port.
class OrcaWorkspaceAdapter final : public AI::IPrintablePaletteProvider, public AI::IModelArtifactConsumer
{
public:
    using ImportSucceededFn = std::function<void()>;

    OrcaWorkspaceAdapter(Plater* plater, ImportSucceededFn on_import_succeeded);

    AI::PrintablePaletteSnapshot printable_palette() const override;
    std::vector<AI::PhysicalFilamentChannel> project_filament_channels() const;
    AI::ModelImportResult import_artifact(const AI::ModelImportRequest& request) override;
    AI::ModelImportResult import_workbench_artifact(const AI::ModelImportRequest& request);
    // Opaque freshness check for a modal confirmation without exposing slicing types.
    std::function<bool()> capture_import_guard() const;
    bool set_project_filament_color(size_t slot, const std::string& color,
        const std::function<bool()>& current, std::string& error);

private:
    Plater*           m_plater { nullptr };
    ImportSucceededFn m_on_import_succeeded;
    AI::VerifiedGlbImportCopy m_verified_glb_import;
};

} // namespace Slic3r::GUI
