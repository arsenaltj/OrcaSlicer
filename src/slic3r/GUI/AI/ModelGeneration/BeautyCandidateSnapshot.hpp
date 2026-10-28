#pragma once
#include "ModelPreviewColorState.hpp"
#include "ModelOriginalAppearance.hpp"
#include "PortraitColorBlockPostprocess.hpp"
#include "PrinterFilamentRestoreSnapshot.hpp"
#include <optional>
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include <nlohmann/json.hpp>

namespace Slic3r { class DynamicPrintConfig; }
namespace Slic3r::GUI {
struct SemanticRegionEvidence;
struct SecondaryRegionEvidence;
struct BeautyPartitionSnapshot;
struct PortraitShapeDetails;
struct BeautyCandidateSnapshot {
    std::shared_ptr<const PortraitColorBlocks::Result> color_blocks;
    std::optional<PrinterFilamentRestoreSnapshot> printer_restore;
    std::optional<AI::ModelMatchedColors> matched_colors;
    std::string matched_configuration_signature;
    std::shared_ptr<DynamicPrintConfig> project_config;
    std::vector<std::string> project_presets;
    boost::filesystem::path source;
    boost::filesystem::path candidate;
    std::string model_sha256;
    std::string geometry_id;
    OriginalAppearanceReference original_appearance;
    std::string original_appearance_error;
    std::string id;
    AI::ModelFinishingResult result;
    AI::ModelFinishingOptions options;
    AI::SurfaceSelectionPersistence::SelectionState selection;
    ModelPreviewColorState color_trial;
    std::vector<std::pair<size_t, std::array<float, 3>>> face_overrides;
    AI::SemanticColoring::FaceColors semantic_faces;
    AI::SemanticColoring::SubfaceColors semantic_subfaces;
    nlohmann::json semantic_provenance;
    std::shared_ptr<const SemanticRegionEvidence> region_evidence;
    std::string region_evidence_error;
    std::shared_ptr<const SecondaryRegionEvidence> secondary_evidence;
    std::string secondary_evidence_error;
    std::shared_ptr<const PortraitShapeDetails> shape_details;
    bool shapes_unlocked {false};
    bool manual_color_dirty {false};
    bool dirty {false};
    nlohmann::json leaf_edits;
    std::shared_ptr<const BeautyPartitionSnapshot> partition;
};

inline BeautyCandidateSnapshot beauty_comparison_baseline(
    const BeautyCandidateSnapshot& baseline,
    const BeautyCandidateSnapshot& current)
{
    auto comparison = baseline;
    if (baseline.geometry_id.empty() || baseline.geometry_id != current.geometry_id)
        return comparison;

    // Partitioning, semantic evidence and draft state are editor context, not
    // appearance. They may be created after the entry baseline was captured
    // and must stay live while the user temporarily views the entry colors.
    comparison.selection = current.selection;
    comparison.region_evidence = current.region_evidence;
    comparison.region_evidence_error = current.region_evidence_error;
    comparison.secondary_evidence = current.secondary_evidence;
    comparison.secondary_evidence_error = current.secondary_evidence_error;
    comparison.shape_details = current.shape_details;
    comparison.shapes_unlocked = current.shapes_unlocked;
    comparison.manual_color_dirty = current.manual_color_dirty;
    comparison.dirty = current.dirty;
    comparison.partition = current.partition;
    return comparison;
}
}
