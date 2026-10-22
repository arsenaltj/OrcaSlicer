#pragma once
#include "ModelPreviewColorState.hpp"
#include "PortraitColorBlockPostprocess.hpp"
namespace Slic3r::GUI {
struct PortraitShapeDetails;
struct PrinterFilamentRestoreSnapshot {
        std::shared_ptr<const PortraitColorBlocks::Result> color_blocks;
        std::string geometry_id;
        ModelPreviewColorState color_trial;
        AI::SemanticColoring::FaceColors face_color_overrides;
        AI::SemanticColoring::FaceColors automatic_face_colors;
        AI::SemanticColoring::SubfaceColors automatic_subface_colors;
        AI::SemanticColoring::FaceColors saved_semantic_faces;
        AI::SemanticColoring::SubfaceColors saved_semantic_subfaces;
        std::shared_ptr<const PortraitShapeDetails> portrait_shapes;
        bool shapes_unlocked {false};
        nlohmann::json leaf_edits;
    };
}
