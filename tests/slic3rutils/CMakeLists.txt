get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    test_beauty_cell_domain.cpp
    test_parent_surface_visibility.cpp
    test_portrait_parent_cleanup.cpp
    test_portrait_residual_proposal.cpp
    test_portrait_surface_ownership_v2.cpp
    test_surface_partition.cpp
	../local_print_recipe_fixture.hpp
    ${_TEST_NAME}_tests_main.cpp
    test_dev_mapping.cpp
    test_filament_bitmap_utils.cpp
    test_network_versions.cpp
    test_action_source.cpp
    test_ai_contracts.cpp
    test_orca_palette_snapshot_builder.cpp
    test_ai_sidecar_client.cpp
    test_model_generation_presentation.cpp
    test_model_generation_client.cpp
    test_model_preview_palette.cpp
    test_model_preview_normals.cpp
    test_model_library_metadata.cpp
    test_model_generation_submission_state.cpp
    test_local_print_color_matching.cpp
    test_local_print_color_recipes.cpp
    test_local_print_color_boundary.cpp
    test_local_print_color_restore.cpp
    test_local_semantic_evidence.cpp
    test_local_semantic_draft.cpp
    test_local_semantic_geometry.cpp
    test_local_print_color_experiment.cpp
    test_local_print_color_application.cpp
    test_project_config_undo.cpp
    test_model_preview_state.cpp
    test_post_generation_workbench.cpp
    test_portrait_optimization.cpp
    test_beauty_document.cpp
    test_beauty_partition_bridge.cpp
    test_beauty_transaction_controller.cpp
    test_semantic_region_evidence.cpp
    test_beauty_leaf_domain.cpp
    test_beauty_leaf_editing.cpp
    test_beauty_shape_lock.cpp
    test_portrait_color_plan.cpp
    test_portrait_shape_details.cpp
    test_portrait_surface_ownership.cpp
    test_local_semantic_worker_client.cpp
    test_model_color_import.cpp
    test_plugin_host_api.cpp
    test_plugin_capability_config.cpp
    test_plugin_config.cpp
    test_plugin_capabilities_in_use.cpp
    test_plugin_install.cpp
    test_plugin_lifecycle.cpp
    test_slicing_pipeline_bindings.cpp
    test_slicing_pipeline_config.cpp
    test_plugin_sort.cpp
    test_plugin_cloud_metadata.cpp
    test_plugin_audit.cpp
    test_ui_redesign.cpp
    test_startup_setup.cpp
    test_vertex_color_region_editor.cpp
    test_surface_selection.cpp
    test_surface_selection_refinement.cpp
    test_surface_selection_state.cpp
    test_color_trial_state.cpp
    test_semantic_coloring.cpp
    test_semantic_mask_refinement.cpp
    test_semantic_material_regions.cpp
    test_mediapipe_face_masks.cpp
    test_model_semantic_coloring.cpp
    test_model_finishing.cpp
    test_beauty_appearance.cpp
    test_beauty_surface.cpp
    test_beauty_puzzle.cpp
    test_beauty_edit_regions.cpp
    test_beauty_boundary_drag.cpp
    test_beauty_boundary_contours.cpp
    test_model_artifact.cpp
    test_orca_model_preparation.cpp
    test_smart_slicing_candidates.cpp
    test_smart_slicing_candidate_evaluation.cpp
    test_smart_slicing_candidate_selection.cpp
    test_smart_slicing_candidate_search.cpp
    test_smart_slicing_trial_scheduler.cpp
    test_smart_slicing_candidate_capture.cpp
    test_smart_slicing_capabilities.cpp
    test_smart_slicing_coordinator.cpp
    test_smart_slicing_dispatch.cpp
    test_smart_slicing_model_features.cpp
    test_smart_slicing_orca_model_features.cpp
    test_smart_slicing_protected_regions.cpp
    test_beauty_protected_regions.cpp
    test_smart_slicing_apply_guard.cpp
    test_smart_slicing_apply_service.cpp
    test_smart_slicing_risk_confirmation.cpp
    test_smart_slicing_parameters.cpp
    test_smart_slicing_parameter_policy.cpp
    test_smart_slicing_runtime.cpp
    test_smart_slicing_session.cpp
    test_smart_slicing_workflow.cpp
    test_smart_slicing_performance_acceptance.cpp
    test_smart_slicing_six_nozzle_validation.cpp
    ../fff_print/test_helpers.cpp
    )

if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests Setupapi.lib)
endif ()

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r_gui libslic3r OrcaSlicer::AIContracts OrcaSlicer::SmartSlicing pybind11::embed Catch2::Catch2WithMain)
target_include_directories(${_TEST_NAME}_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/libslic3r)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

if (WIN32)
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_PREFIX_PATH}/libpython" "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_PREFIX_PATH}/libpython/python${_bundled_python_abi}.dll"
            "${CMAKE_PREFIX_PATH}/libpython/vcruntime140.dll"
            "${CMAKE_PREFIX_PATH}/libpython/vcruntime140_1.dll"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>"
        COMMENT "Copying Python runtime for slic3rutils plugin host API tests"
        VERBATIM
    )
elseif (APPLE)
    target_link_options(${_TEST_NAME}_tests PRIVATE
        "LINKER:-rpath,@executable_path/python/lib")

    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_PREFIX_PATH}/libpython"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMENT "Copying Python runtime for macOS plugin host API tests"
        VERBATIM
    )
endif()

orcaslicer_discover_tests(${_TEST_NAME}_tests)

# Offline contour replay only; this target is not a software production entry.
add_executable(surface_partition_tool ${CMAKE_SOURCE_DIR}/tools/ai/surface_partition_tool.cpp)
target_link_libraries(surface_partition_tool PRIVATE libslic3r)
target_include_directories(surface_partition_tool PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/libslic3r)
if (WIN32)
    target_link_libraries(surface_partition_tool PRIVATE Crypt32)
endif ()
set_property(TARGET surface_partition_tool PROPERTY FOLDER "tests")
