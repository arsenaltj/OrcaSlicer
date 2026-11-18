# A pure geometry target, sharing the desktop implementation without wx/GUI.
set(ORCA_MODEL_DEPENDENCY_ROOT "" CACHE PATH "Pinned dependency sources: boost/eigen/cereal/tbb")
if(NOT ORCA_MODEL_DEPENDENCY_ROOT OR NOT ORCA_MODEL_CGAL_INCLUDE)
 message(FATAL_ERROR "Repair core requires explicit pinned dependency source roots")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(repair_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/MeshBooleanRepair.cpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../repair_core/extract_desktop_repair.py"
 "${CMAKE_CURRENT_LIST_DIR}/../../libslic3r/MeshBoolean.cpp" "${repair_generated}" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
 "${CMAKE_CURRENT_LIST_DIR}/../../libslic3r/MeshBoolean.cpp"
 "${CMAKE_CURRENT_LIST_DIR}/../../libslic3r/TriangleMesh.cpp"
 "${CMAKE_CURRENT_LIST_DIR}/../../libslic3r/BoundingBox.cpp"
 "${CMAKE_CURRENT_LIST_DIR}/../../libslic3r/MeshRepair.cpp"
 "${CMAKE_CURRENT_LIST_DIR}/../repair_core/extract_desktop_repair.py")
file(GLOB repair_boost_headers LIST_DIRECTORIES true "${ORCA_MODEL_DEPENDENCY_ROOT}/boost-src/libs/*/include" "${ORCA_MODEL_DEPENDENCY_ROOT}/boost-src/libs/numeric/*/include")
if(NOT TARGET Eigen3::Eigen)
 add_library(orca_repair_eigen INTERFACE)
 target_include_directories(orca_repair_eigen INTERFACE "${ORCA_MODEL_DEPENDENCY_ROOT}/eigen-src")
 add_library(Eigen3::Eigen ALIAS orca_repair_eigen)
endif()
set(BUILD_SHARED_LIBS ON)
set(TBB_TEST OFF CACHE BOOL "" FORCE)
set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
# Old oneTBB's non-strict Clang helper removes the -Werror substring and
# corrupts the NDK's required -Werror=format-security into =format-security.
# Preserve the NDK security warning by using the dependency's strict mode.
if(ANDROID)
 set(TBB_STRICT ON CACHE BOOL "" FORCE)
else()
 set(TBB_STRICT OFF CACHE BOOL "" FORCE)
endif()
set(TBBMALLOC_BUILD OFF CACHE BOOL "" FORCE)
set(TBB_DISABLE_HWLOC_AUTOMATIC_SEARCH ON CACHE BOOL "" FORCE)
add_subdirectory("${ORCA_MODEL_DEPENDENCY_ROOT}/tbb-src" "repair-deps/tbb")

set(SLIC3R_APP_NAME OrcaModelSDK)
configure_file("${CMAKE_CURRENT_LIST_DIR}/../../libslic3r/libslic3r_version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/libslic3r_version.h" @ONLY)
option(ORCA_MODEL_FULL_STRICT_CORE "Build full desktop AI chain including IGL exact solid union (requires CGAL CORE/LEDA)" OFF)
if(ORCA_MODEL_FULL_STRICT_CORE)
 add_library(orca_model_full_repair_core STATIC
  "${CMAKE_CURRENT_LIST_DIR}/../../libslic3r/MeshRepair.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/MeshBooleanRepair.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/MeshBooleanSolidUnion.cpp")
 message(FATAL_ERROR "Full AI strict chain remains unlinked with the current header-only numeric profile; supply reviewed CGAL CORE/LEDA dependency before enabling")
endif()
add_library(orca_model_repair_core STATIC
 "${CMAKE_CURRENT_BINARY_DIR}/generated/DesktopEnhancedRepair.cpp"
 "${CMAKE_CURRENT_BINARY_DIR}/generated/RepairGeometryIdentity.cpp"
 "${CMAKE_CURRENT_BINARY_DIR}/generated/TriangleMeshRepairGeometry.cpp"
 "${CMAKE_CURRENT_BINARY_DIR}/generated/RepairBoundingBox.cpp"
 "${CMAKE_CURRENT_LIST_DIR}/../../orca_mobile_slicing_sdk/platform/AndroidSha256Digest.cpp"
 "${CMAKE_CURRENT_LIST_DIR}/../repair_core/translation.cpp")
set_target_properties(orca_model_repair_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(orca_model_repair_core PUBLIC
 "${CMAKE_CURRENT_LIST_DIR}/../.." "${CMAKE_CURRENT_LIST_DIR}/../../../deps_src"
 "${CMAKE_CURRENT_LIST_DIR}/../../../deps_src/admesh"
 "${CMAKE_CURRENT_LIST_DIR}/../../../deps_src/clipper2/CPP/Clipper2Lib/include"
 "${CMAKE_CURRENT_LIST_DIR}/../../../deps_src/qhull/src"
 "${CMAKE_CURRENT_LIST_DIR}/../../../deps_src/libigl"
 "${CMAKE_CURRENT_BINARY_DIR}/generated"
 "${ORCA_MODEL_DEPENDENCY_ROOT}/eigen-src" "${ORCA_MODEL_DEPENDENCY_ROOT}/cereal-src/include"
 "${ORCA_MODEL_CGAL_INCLUDE}" ${repair_boost_headers})
target_compile_definitions(orca_model_repair_core PRIVATE
 CGAL_DISABLE_GMP=1 CGAL_NO_CORE=1 _USE_MATH_DEFINES BOOST_ALL_NO_LIB __TBB_NO_IMPLICIT_LINKAGE=1 __TBBMALLOC_NO_IMPLICIT_LINKAGE=1)
target_link_libraries(orca_model_repair_core PUBLIC Eigen3::Eigen TBB::tbb)
foreach(target IN ITEMS orca_model_repair_core)
 target_compile_definitions(${target} PRIVATE _USE_MATH_DEFINES BOOST_ALL_NO_LIB __TBB_NO_IMPLICIT_LINKAGE=1 __TBBMALLOC_NO_IMPLICIT_LINKAGE=1)
 target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../libslic3r" "${CMAKE_CURRENT_LIST_DIR}/../.." "${CMAKE_CURRENT_BINARY_DIR}/generated" ${repair_boost_headers})
 if(MSVC)
  target_compile_options(${target} PRIVATE /utf-8 /Gy /bigobj /FI"${CMAKE_CURRENT_LIST_DIR}/../repair_core/PortableRepairLog.hpp")
 else()
  target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections -include "${CMAKE_CURRENT_LIST_DIR}/../repair_core/PortableRepairLog.hpp")
 endif()
endforeach()
add_executable(orca_repair_core_consumer "${CMAKE_CURRENT_LIST_DIR}/../tests/repair_core_consumer.cpp")
target_link_libraries(orca_repair_core_consumer PRIVATE orca_model_repair_core)
if(MSVC)
 target_link_options(orca_repair_core_consumer PRIVATE /OPT:REF)
else()
 target_link_options(orca_repair_core_consumer PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME desktop_repair_core_link_consumer COMMAND orca_repair_core_consumer)
set_tests_properties(desktop_repair_core_link_consumer PROPERTIES TIMEOUT 120)

if(WIN32)
 add_custom_command(TARGET orca_repair_core_consumer POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
  $<TARGET_FILE:TBB::tbb> $<TARGET_FILE_DIR:orca_repair_core_consumer>)
endif()
if(ANDROID)
 target_link_options(tbb PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
 target_link_options(orca_repair_core_consumer PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
endif()
