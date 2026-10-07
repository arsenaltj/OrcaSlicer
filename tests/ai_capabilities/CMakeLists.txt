set(_color_sources
    test_automatic_color_regions.cpp
    test_color_island_cleanup.cpp
    test_source_color_details.cpp
    test_portrait_color_constraints.cpp
    test_color_matching_engine.cpp
    test_texture_color_engine.cpp
    ../slic3rutils/test_local_print_color_boundary.cpp
)
set(_appearance_sources
    test_beauty_color_regions.cpp
    test_beauty_region_refinement.cpp
    test_beauty_region_precision.cpp
    ../slic3rutils/test_beauty_edit_regions.cpp
    ../slic3rutils/test_beauty_boundary_contours.cpp
    test_appearance_engine.cpp
    ../slic3rutils/test_model_finishing.cpp
    ../slic3rutils/test_beauty_appearance.cpp
)

# Each optimization task can build only its own executable and dependency closure.
# Keep the aggregate suite for compatibility and cross-module native fixtures.
function(add_ai_module_tests module library)
    set(_TEST_NAME ai_${module})
    add_executable(${_TEST_NAME}_tests ${ARGN})
    target_link_libraries(${_TEST_NAME}_tests PRIVATE test_common ${library} Catch2::Catch2WithMain)
    set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests/ai")
    orcaslicer_copy_test_dlls()
    catch_discover_tests(${_TEST_NAME}_tests ADD_TAGS_AS_LABELS TEST_PREFIX "${_TEST_NAME}::")
endfunction()

add_ai_module_tests(color_matching OrcaSlicer::ColorMatching
    ai_capabilities_test_support.cpp ${_color_sources})
add_ai_module_tests(appearance OrcaSlicer::AppearanceEditing
    ai_capabilities_test_support.cpp ${_appearance_sources})
add_ai_module_tests(generation OrcaSlicer::ModelGeneration test_generation_service.cpp)
add_ai_module_tests(placement OrcaSlicer::Placement
    ai_capabilities_test_support.cpp test_placement_engine.cpp)
add_ai_module_tests(slicing_strategy OrcaSlicer::SmartSlicing test_smart_slicing_strategy.cpp)

set(_TEST_NAME ai_capabilities)
add_executable(ai_capabilities_tests
    ai_capabilities_test_support.cpp ${_color_sources} ${_appearance_sources}
    test_generation_service.cpp test_placement_engine.cpp test_smart_slicing_strategy.cpp
    ../slic3rutils/test_smart_slicing_candidates.cpp)
target_link_libraries(ai_capabilities_tests PRIVATE test_common OrcaSlicer::ColorMatching OrcaSlicer::AppearanceEditing OrcaSlicer::ModelGeneration OrcaSlicer::Placement OrcaSlicer::SmartSlicing Catch2::Catch2WithMain)
set_property(TARGET ai_capabilities_tests PROPERTY FOLDER "tests")
orcaslicer_copy_test_dlls()
catch_discover_tests(ai_capabilities_tests ADD_TAGS_AS_LABELS TEST_PREFIX "ai_capabilities::")
