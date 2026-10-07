add_executable(printer_workspace_tests test_printer_workspace.cpp)
target_link_libraries(printer_workspace_tests PRIVATE Catch2::Catch2WithMain)
target_include_directories(printer_workspace_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
orcaslicer_discover_tests(printer_workspace_tests)

# Reuse the mainline revision, connection and media guard cases without building
# every unrelated slic3rutils test for a printer-view integration.
add_executable(printer_confirmation_tests ../slic3rutils/test_smart_slicing_apply_guard.cpp)
target_link_libraries(printer_confirmation_tests PRIVATE Catch2::Catch2WithMain libslic3r libslic3r_gui OrcaSlicer::SmartSlicing)
target_include_directories(printer_confirmation_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
set(_TEST_NAME printer_confirmation)
orcaslicer_copy_test_dlls()
if(WIN32)
    add_custom_command(TARGET printer_confirmation_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_PREFIX_PATH}/libpython/python${_bundled_python_abi}.dll"
            "${CMAKE_PREFIX_PATH}/libpython/vcruntime140.dll"
            "${CMAKE_PREFIX_PATH}/libpython/vcruntime140_1.dll"
            "$<TARGET_FILE_DIR:printer_confirmation_tests>"
        COMMENT "Copying Python loader dependencies for printer confirmation tests"
        VERBATIM
    )
elseif(APPLE)
    target_link_options(printer_confirmation_tests PRIVATE
        "LINKER:-rpath,@executable_path/python/lib")
    add_custom_command(TARGET printer_confirmation_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_PREFIX_PATH}/libpython"
            "$<TARGET_FILE_DIR:printer_confirmation_tests>/python"
        COMMENT "Copying Python runtime for macOS printer confirmation tests"
        VERBATIM
    )
endif()
# The same guard cases also live in slic3rutils_tests; keep CTest names unique.
catch_discover_tests(printer_confirmation_tests TEST_PREFIX "Printer confirmation: " ADD_TAGS_AS_LABELS)
