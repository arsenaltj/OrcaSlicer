# Explicit header-only dependency source root; never reuse another build's objects.
set(ORCA_MODEL_DEPENDENCY_ROOT "" CACHE PATH "Contains pinned boost-src/eigen-src/cereal-src/tbb-src")
if(NOT ORCA_MODEL_DEPENDENCY_ROOT)
 message(FATAL_ERROR "Region core needs explicit ORCA_MODEL_DEPENDENCY_ROOT with pinned dependency sources")
endif()
set(SLIC3R_APP_NAME OrcaModelSDK)
set(BBL_INTERNAL_TESTING 0)
set(ORCA_CHECK_GCODE_PLACEHOLDERS 0)
configure_file(${CMAKE_CURRENT_LIST_DIR}/../../libslic3r/libslic3r_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/libslic3r_version.h @ONLY)
add_library(orca_model_region_core OBJECT ../slic3r/GUI/AI/Model/VertexColorRegionEditor.cpp)
file(GLOB boost_component_headers LIST_DIRECTORIES true "${ORCA_MODEL_DEPENDENCY_ROOT}/boost-src/libs/*/include")
target_compile_definitions(orca_model_region_core PRIVATE ORCA_MODEL_REGION_COMPUTE_ONLY)
target_compile_definitions(orca_model_region_core PRIVATE BOOST_ALL_NO_LIB __TBB_NO_IMPLICIT_LINKAGE=1 __TBBMALLOC_NO_IMPLICIT_LINKAGE=1)
target_include_directories(orca_model_region_core PRIVATE
 ${boost_component_headers}
 ${CMAKE_CURRENT_BINARY_DIR}/generated ../ ../libslic3r ../../deps_src ../../deps_src/admesh
 ${ORCA_MODEL_DEPENDENCY_ROOT}/boost-src
 ${ORCA_MODEL_DEPENDENCY_ROOT}/eigen-src
 ${ORCA_MODEL_DEPENDENCY_ROOT}/cereal-src/include
 ${ORCA_MODEL_DEPENDENCY_ROOT}/tbb-src/include)
set_target_properties(orca_model_region_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
get_target_property(region_includes orca_model_region_core INCLUDE_DIRECTORIES)
target_sources(orca_model_sdk PRIVATE $<TARGET_OBJECTS:orca_model_region_core> region_adapter.cpp)
target_include_directories(orca_model_sdk PRIVATE ${region_includes})
target_compile_definitions(orca_model_sdk PRIVATE ORCA_MODEL_HAS_REGION_CORE)
target_compile_definitions(orca_model_sdk PRIVATE BOOST_ALL_NO_LIB __TBB_NO_IMPLICIT_LINKAGE=1 __TBBMALLOC_NO_IMPLICIT_LINKAGE=1)
if(MSVC)
 target_compile_options(orca_model_region_core PRIVATE /utf-8)
 target_compile_options(orca_model_sdk PRIVATE /utf-8)
endif()
