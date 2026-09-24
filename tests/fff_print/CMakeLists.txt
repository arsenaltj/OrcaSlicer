get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests 
	../local_print_recipe_fixture.hpp
	${_TEST_NAME}_tests.cpp
	test_helpers.cpp
	test_helpers.hpp
	test_cooling.cpp
	test_extrusion_entity.cpp
	test_extrusion_processor.cpp
	test_fill.cpp
	test_flow.cpp
	test_gcode_timing.cpp
	test_gcodewriter.cpp
	test_model.cpp
	test_multifilament.cpp
	test_perimeters.cpp
	test_print.cpp
	test_printobject.cpp
	test_mixed_filament.cpp
	test_color_projection.cpp
	test_color_surface_binding.cpp
	test_skirt_brim.cpp
	test_slicing_pipeline_hook.cpp
	test_support_material.cpp
	test_trianglemesh.cpp
	test_wipe_tower.cpp
	)
target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain)
if(WIN32)
    # Snapshot identity tests use the existing OpenSSL SHA-256 implementation.
    target_link_libraries(${_TEST_NAME}_tests Crypt32)
endif()
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()

orcaslicer_discover_tests(${_TEST_NAME}_tests)
