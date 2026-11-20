# Android Model and Slicing use the same exact previously approved runtime.
# This is a trusted build-time input, not an operation/request option.
if(TARGET TBB::tbb OR TARGET orca_model_approved_tbb)
 message(FATAL_ERROR "Conflicting TBB target: approved runtime binding must be unique")
endif()
set(ORCA_MODEL_ANDROID_TBB_RUNTIME "" CACHE FILEPATH "Explicit previously approved Android Slicing libtbb.so")
if(NOT ORCA_MODEL_ANDROID_TBB_RUNTIME OR NOT IS_ABSOLUTE "${ORCA_MODEL_ANDROID_TBB_RUNTIME}")
 message(FATAL_ERROR "Android repair Core requires explicit absolute ORCA_MODEL_ANDROID_TBB_RUNTIME")
endif()
set(approved_tbb_script "${CMAKE_CURRENT_LIST_DIR}/../repair_core/verify_approved_tbb.py")
set(approved_tbb_manifest "${CMAKE_CURRENT_LIST_DIR}/../repair_core/approved_tbb.json")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${approved_tbb_script}"
 --library "${ORCA_MODEL_ANDROID_TBB_RUNTIME}" --include "${ORCA_MODEL_DEPENDENCY_ROOT}/tbb-src/include"
 --manifest "${approved_tbb_manifest}" --output "${CMAKE_CURRENT_BINARY_DIR}/approved-tbb-binding.json"
 COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ORCA_MODEL_ANDROID_TBB_RUNTIME}" "${approved_tbb_script}" "${approved_tbb_manifest}")
file(GLOB_RECURSE approved_tbb_headers CONFIGURE_DEPENDS "${ORCA_MODEL_DEPENDENCY_ROOT}/tbb-src/include/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${approved_tbb_headers})
add_library(orca_model_approved_tbb SHARED IMPORTED GLOBAL)
set_target_properties(orca_model_approved_tbb PROPERTIES
 IMPORTED_LOCATION "${ORCA_MODEL_ANDROID_TBB_RUNTIME}" IMPORTED_SONAME "libtbb.so"
 INTERFACE_INCLUDE_DIRECTORIES "${ORCA_MODEL_DEPENDENCY_ROOT}/tbb-src/include")
add_library(TBB::tbb ALIAS orca_model_approved_tbb)
set_property(TARGET orca_model_sdk APPEND PROPERTY LINK_DEPENDS "${ORCA_MODEL_ANDROID_TBB_RUNTIME}")
target_link_options(orca_model_sdk PRIVATE "-Wl,-z,defs")
set_target_properties(orca_model_sdk PROPERTIES SKIP_BUILD_RPATH TRUE)
