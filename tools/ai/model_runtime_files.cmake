# File ownership and packaging for model application capabilities.
# Explicit membership prevents a helper from working locally but missing from installation.
set(ORCA_MODEL_CAPABILITY_RUNTIME_FILES
    "${CMAKE_SOURCE_DIR}/tools/ai/capability_catalog.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/config_proposal.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/design_workflow.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/model_artifact_workflow.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/model_color_regions.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/model_color_space.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/model_contracts.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/model_creation_policy.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/model_generation_workflow.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/model_job_application.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/model_job_lifecycle.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/model_job_repository.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/model_mesh_repair.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/model_obj_io.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/model_quality_workflow.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/model_request.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/model_texture_baking.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/portrait_geometry_reference.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/portrait_model_materials.py"
    "${CMAKE_SOURCE_DIR}/tools/ai/portrait_multiview_workflow.py"
)
