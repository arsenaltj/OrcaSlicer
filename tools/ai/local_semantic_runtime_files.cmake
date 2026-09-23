# Explicit local-semantic component installation; no dependency or weight fetch.
# glb_artifact.py is already installed by the shared AI artifact component.
set(ORCA_LOCAL_SEMANTIC_RUNTIME_FILES
    "${CMAKE_SOURCE_DIR}/tools/ai/local_semantic_worker.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/local_semantic_geometry.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/local_semantic_render.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/local_semantic_transform.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/local_semantic_views.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/local_semantic_projection.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/local_semantic_pipeline.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/local_semantic_request.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/local_eye_landmarks.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/local_face_landmarks.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/local_body_regions.py"
)
