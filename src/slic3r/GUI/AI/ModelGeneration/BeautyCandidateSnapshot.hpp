#pragma once
#include "ModelPreviewColorState.hpp"
#include "ModelOriginalAppearance.hpp"
#include "PortraitColorBlockPostprocess.hpp"
#include "PrinterFilamentRestoreSnapshot.hpp"
#include <optional>
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
}
