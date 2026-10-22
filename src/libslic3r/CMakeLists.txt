cmake_minimum_required(VERSION 3.13)
project(libslic3r)

include(PrecompiledHeader)

if(NOT DEFINED ORCA_CHECK_GCODE_PLACEHOLDERS)
    set(ORCA_CHECK_GCODE_PLACEHOLDERS "0")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/libslic3r_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/libslic3r_version.h @ONLY)

if (MINGW)
    add_compile_options(-Wa,-mbig-obj)
endif ()

set(OpenVDBUtils_SOURCES "")
if (TARGET OpenVDB::openvdb)
    set(OpenVDBUtils_SOURCES OpenVDBUtils.cpp OpenVDBUtils.hpp)
endif()

option(BUILD_SHARED_LIBS "Build shared libs" OFF)
option(USE_SLIC3R_CONSOLE_LOG "Enable console logging in RelWithDebInfo builds" OFF)

set(ORCA_SLIC3R_CORE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
include("${CMAKE_CURRENT_SOURCE_DIR}/core_sources.cmake")
set(lisbslic3r_sources ${ORCA_SLIC3R_CORE_SOURCES})
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(orca_slicer_core_manifest_check ALL
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/verify_core_source_manifest.py
                --source-root ${CMAKE_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking shared OrcaSlicer::Core source manifest"
        VERBATIM
    )
endif()


if (APPLE)
    list(APPEND lisbslic3r_sources
            MacUtils.mm
            Format/ModelIO.hpp
            Format/ModelIO.mm
        )
endif ()

add_library(orca_slicer_core STATIC ${lisbslic3r_sources}
    "${CMAKE_CURRENT_BINARY_DIR}/libslic3r_version.h"
    ${OpenVDBUtils_SOURCES})
# Public cross-platform core contract. Keep libslic3r as the implementation
# name so upstream and existing desktop tooling continue to resolve it.
add_library(OrcaSlicer::Core ALIAS orca_slicer_core)
# Backward-compatible target name used by existing tests and upstream tooling.
add_library(libslic3r ALIAS orca_slicer_core)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${lisbslic3r_sources})

if (SLIC3R_STATIC)
    set(CGAL_Boost_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
endif ()
set(CGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE ON CACHE BOOL "" FORCE)

cmake_policy(PUSH)
cmake_policy(SET CMP0011 NEW)
# CGAL's config resets policies (cmake_minimum_required ...3.23), so a plain SET
# can't reach it; the default opts its Boost lookup into BoostConfig (CMP0167).
set(CMAKE_POLICY_DEFAULT_CMP0167 NEW)
find_package(CGAL REQUIRED)
find_package(OpenCV REQUIRED core)
find_package(assimp REQUIRED)
unset(CMAKE_POLICY_DEFAULT_CMP0167)
cmake_policy(POP)

add_library(libslic3r_cgal STATIC
    CutSurface.hpp CutSurface.cpp
    IntersectionPoints.hpp IntersectionPoints.cpp
    MeshBoolean.hpp MeshBoolean.cpp
    TryCatchSignal.hpp TryCatchSignal.cpp
    Triangulation.hpp Triangulation.cpp
)
target_include_directories(libslic3r_cgal PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Reset compile options of libslic3r_cgal. Despite it being linked privately, CGAL options
# (-frounding-math) still propagate to dependent libs which is not desired.
get_target_property(_cgal_tgt CGAL::CGAL ALIASED_TARGET)
if (NOT TARGET ${_cgal_tgt})
    set (_cgal_tgt CGAL::CGAL)
endif ()
get_target_property(_opts ${_cgal_tgt} INTERFACE_COMPILE_OPTIONS)
if (_opts)
    set(_opts_bad "${_opts}")
    set(_opts_good "${_opts}")
    list(FILTER _opts_bad INCLUDE REGEX frounding-math)
    list(FILTER _opts_good EXCLUDE REGEX frounding-math)
    set_target_properties(${_cgal_tgt} PROPERTIES INTERFACE_COMPILE_OPTIONS "${_opts_good}")
    target_compile_options(libslic3r_cgal PRIVATE "${_opts_bad}")
endif()

target_link_libraries(libslic3r_cgal PRIVATE ${_cgal_tgt} admesh libigl mcut boost_libs)

if (MSVC AND "${CMAKE_SIZEOF_VOID_P}" STREQUAL "4") # 32 bit MSVC workaround
    target_compile_definitions(libslic3r_cgal PRIVATE CGAL_DO_NOT_USE_MPZF)
endif ()

encoding_check(orca_slicer_core)

target_compile_definitions(orca_slicer_core PUBLIC -DUSE_TBB -DTBB_USE_CAPTURED_EXCEPTION=0)
if (USE_SLIC3R_CONSOLE_LOG)
    target_compile_definitions(orca_slicer_core PRIVATE $<$<CONFIG:RelWithDebInfo>:SLIC3R_CONSOLE_LOG>)
endif()
target_include_directories(orca_slicer_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/TextureToColor PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(orca_slicer_core SYSTEM PUBLIC ${EXPAT_INCLUDE_DIRS})

# Find the OCCT and related libraries
set(OpenCASCADE_DIR "${CMAKE_PREFIX_PATH}/lib/cmake/occt")
find_package(OpenCASCADE REQUIRED)
target_include_directories(orca_slicer_core SYSTEM PUBLIC ${OpenCASCADE_INCLUDE_DIR})

find_package(JPEG REQUIRED)
find_package(Draco REQUIRED)

set(OCCT_LIBS
    TKXDESTEP
    TKSTEP
    TKSTEP209
    TKSTEPAttr
    TKSTEPBase
    TKXCAF
    TKXSBase
    TKVCAF
    TKCAF
    TKLCAF
    TKCDF
    TKV3d
    TKService
    TKMesh
    TKBO
    TKPrim
    TKHLR
    TKShHealing
    TKTopAlgo
    TKGeomAlgo
    TKBRep
    TKGeomBase
    TKG3d
    TKG2d
    TKMath
    TKernel
)

target_link_libraries(orca_slicer_core
    PUBLIC
        Eigen3::Eigen
        admesh
        libigl
        libnest2d
        miniz
        opencv_world
        assimp::assimp
    PRIVATE
        ${CMAKE_DL_LIBS}
        ${EXPAT_LIBRARIES}
        ${OCCT_LIBS}
        boost_libs
        cereal::cereal
        clipper
        Clipper2
        draco::draco
        glu-libtess
        JPEG::JPEG
        libslic3r_cgal
        mcut
        noise::noise
        PNG::PNG
        qhull
        qoi
        semver
        TBB::tbb
        TBB::tbbmalloc
        ZLIB::ZLIB
        OpenSSL::Crypto
    )

if(NOT WIN32)
    # Link freetype for OCCT dependency (CAD operations need font rendering)
    target_link_libraries(orca_slicer_core PRIVATE ${FREETYPE_LIBRARIES})
    if (NOT APPLE)
        target_link_libraries(orca_slicer_core PRIVATE fontconfig)
    endif()
endif()

if (APPLE)
    find_library(FOUNDATION Foundation REQUIRED)
    find_library(MODELIO ModelIO REQUIRED)
    target_link_libraries(orca_slicer_core PRIVATE ${FOUNDATION} ${MODELIO})
endif ()

if (TARGET OpenVDB::openvdb)
    target_link_libraries(orca_slicer_core PRIVATE OpenVDB::openvdb)
endif()

if(WIN32)
    # The streaming digest's OpenSSL fallback also needs its static CAPI engine.
    target_link_libraries(orca_slicer_core PRIVATE Psapi.lib bcrypt.lib Crypt32.lib)
endif()

if(SLIC3R_PROFILE)
    target_link_libraries(orca_slicer_core PRIVATE Shiny)
endif()

if (SLIC3R_PCH AND NOT SLIC3R_SYNTAXONLY)
    add_precompiled_header(orca_slicer_core pchheader.hpp FORCEINCLUDE)
endif ()
