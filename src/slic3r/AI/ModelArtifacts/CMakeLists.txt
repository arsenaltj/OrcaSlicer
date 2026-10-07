add_library(orcaslicer_ai_model_artifacts STATIC
    ModelArtifact.cpp ModelArtifact.hpp
    GlbGeometryEditing.cpp GlbGeometryEditing.hpp
    ModelObjText.hpp
    SurfaceSelectionState.hpp
)
add_library(OrcaSlicer::ModelArtifacts ALIAS orcaslicer_ai_model_artifacts)
target_link_libraries(orcaslicer_ai_model_artifacts PUBLIC OrcaSlicer::AIContracts libslic3r OpenSSL::Crypto)
encoding_check(orcaslicer_ai_model_artifacts)
