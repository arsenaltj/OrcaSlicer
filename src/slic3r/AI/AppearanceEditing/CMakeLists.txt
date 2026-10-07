add_library(orcaslicer_ai_appearance STATIC
    AppearanceEngine.cpp AppearanceEngine.hpp BaselineFinishing.hpp
    ModelFinishing.cpp ModelFinishing.hpp
    BeautyFinishing.cpp
    BeautyAppearance.cpp BeautyAppearance.hpp
    BeautySurface.cpp BeautySurface.hpp
    BeautyBoundaryContours.hpp BeautyRegionBoundary.cpp BeautyRegionBoundary.hpp
    BeautyRegionPrecision.cpp BeautyRegionPrecision.hpp
    VertexColorRegionEditor.cpp VertexColorRegionEditor.hpp
    ModelColorCleanup.hpp BeautyDocument.hpp BeautyPuzzle.hpp BeautyPuzzleColor.cpp BeautyPuzzleRefinement.cpp BeautyEditRegions.hpp SurfaceSelectionState.hpp
)
add_library(OrcaSlicer::AppearanceEditing ALIAS orcaslicer_ai_appearance)
target_link_libraries(orcaslicer_ai_appearance PUBLIC OrcaSlicer::ModelArtifacts OrcaSlicer::ColorMatching)
encoding_check(orcaslicer_ai_appearance)
