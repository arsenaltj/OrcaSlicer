add_library(orcaslicer_ai_color_matching STATIC
    AutomaticColorRegions.cpp AutomaticColorRegions.hpp
    ColorIslandCleanup.cpp ColorIslandCleanup.hpp
    SourceColorDetails.cpp SourceColorDetails.hpp
    PortraitColorConstraints.cpp PortraitColorConstraints.hpp
    ColorMatchingEngine.cpp
    ColorMatchingEngine.hpp
    TextureColorMatchingEngine.cpp
    TextureColorMatchingEngine.hpp
    ColorMatchingTypes.hpp
    RegionColorMatching.hpp
    ModelPreviewPalette.hpp
    LocalPrintColorRecipes.hpp
    LocalPrintColorLayeredSelection.hpp
    LocalPrintRecipeProofState.hpp
    LocalPrintColorQuality.hpp
    LocalPrintColorPatchRefinement.hpp
    LocalPrintColorBoundaryRefinement.hpp
)
add_library(OrcaSlicer::ColorMatching ALIAS orcaslicer_ai_color_matching)
target_link_libraries(orcaslicer_ai_color_matching PUBLIC OrcaSlicer::AIContracts libslic3r OpenSSL::Crypto)
encoding_check(orcaslicer_ai_color_matching)
