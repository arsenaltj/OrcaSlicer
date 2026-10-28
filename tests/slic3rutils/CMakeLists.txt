get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    test_orca_slicing_support.cpp
    ../../src/orca_mobile_slicing_sdk/engine.cpp
    ../../src/orca_mobile_slicing_sdk/project_support.cpp
    ../../src/orca_mobile_slicing_sdk/recommendation_support.cpp
    ../../src/orca_mobile_slicing_sdk/smart_slice_bridge.cpp
    test_portrait_color_blocks.cpp
    test_beauty_cell_domain.cpp
    test_parent_surface_visibility.cpp
    test_portrait_parent_cleanup.cpp
    test_portrait_residual_proposal.cpp
    test_portrait_surface_ownership_v2.cpp
    test_surface_partition.cpp
	../local_print_recipe_fixture.hpp
    ${_TEST_NAME}_tests_main.cpp
    test_bambu_filament_ids.cpp
    test_bed_exclusion_fields.cpp
    test_cloud_delete_queue.cpp
    test_config_value_formatter.cpp
    test_creality_cfs_match.cpp
    test_creality_print.cpp
    test_dev_mapping.cpp
    test_device_access.cpp
    test_moonraker_printer_agent.cpp
    test_network_discovery.cpp
    test_filament_bitmap_utils.cpp
    test_colour_picker_field.cpp
    test_filament_color_picker.cpp
    test_ams_materials_setting.cpp
    test_gcode_preview_colors.cpp
    test_device_progress.cpp
    test_device_manager_integration.cpp
    test_web_media_controller.cpp
    test_scene_raycaster.cpp
    test_scene_benchmark.cpp
    test_lazy.cpp
    test_prebuild_queue.cpp
    test_staged_build.cpp
    test_network_versions.cpp
    test_orca_cloud_agent.cpp
    test_action_source.cpp
    test_option_state_registry.cpp
    test_ai_contracts.cpp
    test_orca_palette_snapshot_builder.cpp
    test_ai_sidecar_client.cpp
    test_ai_text.cpp
    test_model_generation_presentation.cpp
    test_model_generation_client.cpp
    test_model_preview_palette.cpp
    test_model_preview_normals.cpp
    test_gl_model_geometry.cpp
    test_mesh_raycaster.cpp
    test_font_texture_compression.cpp
    test_model_library_metadata.cpp
    test_atomic_model_capabilities.cpp
    test_model_library_filter.cpp
    test_model_library_export.cpp
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
    # Exercise seam enums and predicates through the embedded Python host API.
    test_precise_seam_plugin.cpp
    test_plugin_capability_config.cpp
    test_plugin_config.cpp
    test_plugin_capabilities_in_use.cpp
    test_plugin_status.cpp
    test_printer_agent.cpp
    test_plugin_install.cpp
    test_plugin_lifecycle.cpp
    test_plugin_resource_texture.cpp
    test_plugin_printer_agent.cpp
    test_printhost.cpp
    test_slicing_pipeline_bindings.cpp
    test_slicing_pipeline_config.cpp
    test_plugin_sort.cpp
    test_plugin_cloud_metadata.cpp
    test_plugin_audit.cpp
    test_plugin_json_depth.cpp
    test_shortcuts.cpp
    test_texture_color_palette.cpp
    test_texture_island_view.cpp
    test_timelapse_codecs.cpp
    test_file_url.cpp
    test_imgui_wrapper.cpp
    test_user_manager.cpp
    test_sequential_clearance.cpp
    test_object_list.cpp
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
    test_beauty_persistence.cpp
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
    test_software_viewport_export.cpp
    test_smart_slicing_apply_service.cpp
    test_smart_slicing_risk_confirmation.cpp
    test_smart_slicing_parameters.cpp
    test_smart_slicing_parameter_policy.cpp
    test_smart_slicing_runtime.cpp
    test_smart_slicing_session.cpp
    test_smart_slicing_workflow.cpp
    test_smart_slicing_performance_acceptance.cpp
    test_smart_slicing_six_nozzle_validation.cpp
    test_smart_slice_engine.cpp
    test_smart_slice_applier.cpp
    test_slicing_recommendation_client.cpp
    ../fff_print/test_helpers.cpp
    )

if (SLIC3R_CAD)
    target_sources(${_TEST_NAME}_tests PRIVATE test_design_sketch_tool.cpp)
endif ()

if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests Setupapi.lib)
endif ()

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r_gui libslic3r OrcaSlicer::AIContracts OrcaSlicer::SmartSlicing pybind11::embed Catch2::Catch2WithMain)
target_include_directories(${_TEST_NAME}_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/libslic3r)
if (ORCA_SEMANTIC_RUNTIME_DIR)
    target_compile_definitions(${_TEST_NAME}_tests PRIVATE
        ORCA_TEST_SEMANTIC_RUNTIME_DIR=R"\(${ORCA_SEMANTIC_RUNTIME_DIR}\)")
endif()
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls(${_TEST_NAME}_tests)

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
elseif (NOT FLATPAK)
    # The CI unit-test runner only receives the build/tests tree, so both the
    # interpreter and the libpython the test binary links have to travel next to
    # the executable; find_bundled_python_home() picks the copy up from there.
    if (APPLE)
        target_link_options(${_TEST_NAME}_tests PRIVATE
            "LINKER:-rpath,@executable_path/python/lib")
    else ()
        set_property(TARGET ${_TEST_NAME}_tests APPEND PROPERTY BUILD_RPATH "$ORIGIN/python/lib")
    endif ()

    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_PREFIX_PATH}/libpython"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMENT "Copying Python runtime for the plugin host API tests"
        VERBATIM
    )
elseif (FLATPAK)
    # Same <testdir>/python home as WIN32/APPLE; symlink since /app/libpython
    # already ships in the flatpak (the test exe links libpython3.12.so from it).
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "${CMAKE_PREFIX_PATH}/libpython"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/python"
        COMMENT "Linking Python runtime for flatpak plugin host API tests"
        VERBATIM
    )
endif()

# scripts/run_unit_tests.sh installs the tests' numpy into that runtime with this uv,
# staged where the app build tree keeps it (<exe dir>/tools/uv, see src/CMakeLists.txt).
if (ORCA_BUNDLED_UV_EXECUTABLE)
    add_custom_command(TARGET ${_TEST_NAME}_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/tools/uv"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ORCA_BUNDLED_UV_EXECUTABLE}"
            "$<TARGET_FILE_DIR:${_TEST_NAME}_tests>/tools/uv/${ORCA_BUNDLED_UV_FILENAME}"
        COMMENT "Copying uv for the plugin host API tests"
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
