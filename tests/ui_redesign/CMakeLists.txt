add_executable(printer_workspace_tests test_printer_workspace.cpp)
target_link_libraries(printer_workspace_tests PRIVATE Catch2::Catch2WithMain)
target_include_directories(printer_workspace_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
orcaslicer_discover_tests(printer_workspace_tests)

# Reuse the mainline revision, connection and media guard cases without building
# every unrelated slic3rutils test for a printer-view integration.
add_executable(printer_confirmation_tests ../slic3rutils/test_smart_slicing_apply_guard.cpp)
target_link_libraries(printer_confirmation_tests PRIVATE Catch2::Catch2WithMain libslic3r libslic3r_gui OrcaSlicer::SmartSlicing)
target_include_directories(printer_confirmation_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(WIN32)
    orcaslicer_copy_test_dlls()
endif()
# The same guard cases also live in slic3rutils_tests; keep CTest names unique.
catch_discover_tests(printer_confirmation_tests TEST_PREFIX "Printer confirmation: " ADD_TAGS_AS_LABELS)
