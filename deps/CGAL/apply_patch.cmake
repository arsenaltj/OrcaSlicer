# ExternalProject may live inside or outside the application's Git checkout.
# Stop parent-repository discovery so git apply always uses this source root.
if(NOT IS_DIRECTORY "${SOURCE_DIR}" OR NOT EXISTS "${PATCH_FILE}")
    message(FATAL_ERROR "CGAL patch requires an existing source root and patch")
endif()
find_program(CGAL_PATCH_GIT git REQUIRED)
get_filename_component(_source_parent "${SOURCE_DIR}" DIRECTORY)
set(_patch_command "${CMAKE_COMMAND}" -E env
    --unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_INDEX_FILE --unset=GIT_COMMON_DIR
    "GIT_CEILING_DIRECTORIES=${_source_parent}"
    "${CGAL_PATCH_GIT}" apply --ignore-space-change --whitespace=fix)
execute_process(COMMAND ${_patch_command} --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE _can_apply
    OUTPUT_VARIABLE _check_out ERROR_VARIABLE _check_error)
if(_can_apply EQUAL 0)
    execute_process(COMMAND ${_patch_command} "${PATCH_FILE}"
        WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE _applied
        OUTPUT_VARIABLE _apply_out ERROR_VARIABLE _apply_error)
    if(NOT _applied EQUAL 0)
        message(FATAL_ERROR "CGAL patch failed: ${_apply_error}")
    endif()
else()
    execute_process(COMMAND ${_patch_command} --reverse --check "${PATCH_FILE}"
        WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE _already_applied
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT _already_applied EQUAL 0)
        message(FATAL_ERROR "CGAL patch does not match source: ${_check_error}")
    endif()
endif()
