# Additional model coloring sources and optional inference dependencies.
# The three core editing sources already belong to SLIC3R_GUI_SOURCES.
target_sources(libslic3r_gui PRIVATE
    GUI/AI/ModelGeneration/ModelSemanticColoring.cpp
    GUI/AI/ModelGeneration/ModelPreviewSemantics.cpp
    AI/ModelGeneration/SemanticColoring/SemanticColoring.cpp
    AI/ModelGeneration/SemanticColoring/SemanticMaskRefinement.cpp
    AI/ModelGeneration/SemanticColoring/SemanticMaterialRegions.cpp
    AI/ModelGeneration/SemanticColoring/MediaPipeRegionRecognizers.cpp
)
include("${CMAKE_SOURCE_DIR}/cmake/OrcaSemanticRuntime.cmake")
orca_configure_semantic_recognizers(libslic3r_gui)
