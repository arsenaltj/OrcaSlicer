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

# SolveSpace constraint solver (2D sketch solver backbone), built in deps/SLVS.
if (SLIC3R_CAD)
    find_package(SLVS REQUIRED)
endif ()

set(ORCA_SLIC3R_CORE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
include("${CMAKE_CURRENT_SOURCE_DIR}/core_sources.cmake")
set(lisbslic3r_sources ${ORCA_SLIC3R_CORE_SOURCES})
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND NOT TARGET orca_slicer_core_manifest_check)
    add_custom_target(orca_slicer_core_manifest_check ALL
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/verify_core_source_manifest.py
                --source-root ${CMAKE_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking shared OrcaSlicer::Core source manifest"
        VERBATIM)
endif()

# Parametric Design/CAD kernel. Needs OCCT's ModelingAlgorithms module and the
# vendored SolveSpace solver; both are pulled in only when SLIC3R_CAD is ON.
# Optional CAD source selection lives in the shared manifest.

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
# Preserve upstream target consumers while exposing the shared core contract.
add_library(OrcaSlicer::Core ALIAS orca_slicer_core)
add_library(libslic3r ALIAS orca_slicer_core)
if(TARGET orca_slicer_core_manifest_check)
    add_dependencies(orca_slicer_core orca_slicer_core_manifest_check)
endif()
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${lisbslic3r_sources})

if (SLIC3R_STATIC)
    set(CGAL_Boost_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
endif ()
set(CGAL_DO_NOT_WARN_ABOUT_CMAKE_BUILD_TYPE ON CACHE BOOL "" FORCE)

cmake_policy(PUSH)
cmake_policy(SET CMP0011 NEW)
# CGAL's config resets policies (cmake_minimum_required ...3.31), so a plain SET
# can't reach it; the default opts its Boost lookup into BoostConfig (CMP0167).
set(CMAKE_POLICY_DEFAULT_CMP0167 NEW)
find_package(CGAL 6.2 REQUIRED)
find_package(OpenCV REQUIRED core)
find_package(assimp REQUIRED)
unset(CMAKE_POLICY_DEFAULT_CMP0167)
cmake_policy(POP)

add_library(libslic3r_cgal STATIC
    CutSurface.hpp CutSurface.cpp
    IntersectionPoints.hpp IntersectionPoints.cpp
    MeshBoolean.hpp MeshBoolean.cpp
    MeshRepair.hpp MeshRepair.cpp
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

if (IS_CLANG_CL)
    # CGAL passes /fp:strict /fp:except-. clang-cl reports the second as overriding part of
    # the first; the settings cc1 receives are the same ones MSVC produces from that pair.
    target_compile_options(libslic3r_cgal PRIVATE -Wno-overriding-option)
endif ()

# Lets CGAL algorithms given CGAL::Parallel_if_available_tag run on TBB.
target_compile_definitions(libslic3r_cgal PRIVATE CGAL_LINKED_WITH_TBB)
target_link_libraries(libslic3r_cgal PRIVATE ${_cgal_tgt} admesh libigl mcut boost_libs TBB::tbb)

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
    TKDESTEP
    TKDE
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
# The CAD kernel is the only consumer of OCCT's ModelingAlgorithms module: TKFillet
# (BRepFilletAPI), TKOffset (BRepOffsetAPI) and TKBool, which the other two need.
#
# PREPEND, never append: this list is single-pass static link order, dependents before
# dependencies — note TKernel, which everything needs, is deliberately last. TKOffset
# references BRepAlgo_Loop, which TKBool defines, so TKOffset must come BEFORE TKBool.
# Appending put it after, and a strictly single-pass linker (the Flatpak build) failed with
#     libTKOffset.a(BRepOffset_MakeLoops.cxx.o): undefined reference to
#     `BRepAlgo_Loop::BRepAlgo_Loop()'
# while the ordinary Linux, macOS and Windows links resolved it anyway. Use set() rather
# than list(PREPEND), which needs CMake 3.15 and this project supports 3.13.
if (SLIC3R_CAD)
    set(OCCT_LIBS TKFillet TKOffset TKBool ${OCCT_LIBS})
    # deps is configured separately, so its SLIC3R_CAD can differ from ours. The module is
    # all-or-nothing, so one absent toolkit proves it; fail here rather than at link time.
    if (NOT TARGET TKFillet)
        message(FATAL_ERROR
            "SLIC3R_CAD is ON, but the OpenCASCADE in ${CMAKE_PREFIX_PATH} was built without "
            "BUILD_MODULE_ModelingAlgorithms. Rebuild the dependencies with -DSLIC3R_CAD=ON, "
            "or configure this project with -DSLIC3R_CAD=OFF.")
    endif ()
endif ()
# Published for the Windows packaging step in the top-level CMakeLists.txt.
set(OCCT_LIBS "${OCCT_LIBS}" CACHE INTERNAL "OCCT toolkits linked by libslic3r")
if (WIN32)
    # OCCT installs RelWithDebInfo and Debug builds to bin/occti and bin/occtd, so each build
    # type's DLL directory is read from the imported target, falling back to the first installed
    # build, as CMake does when linking.
    get_target_property(_occt_configs TKernel IMPORTED_CONFIGURATIONS)
    list(GET _occt_configs 0 _occt_fallback)
    foreach (_build_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set(_occt_config ${_occt_fallback})
        if (_build_config IN_LIST _occt_configs)
            set(_occt_config ${_build_config})
        endif ()
        get_target_property(_occt_dll TKernel IMPORTED_LOCATION_${_occt_config})
        get_filename_component(_occt_bin "${_occt_dll}" DIRECTORY)
        set(OCCT_BIN_DIR_${_build_config} "${_occt_bin}" CACHE INTERNAL "OCCT DLL directory for ${_build_config} builds")
    endforeach ()
endif ()

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
    # OCCT's static targets name FreeType as a bare "freetype", which the macOS linker cannot
    # find; this target maps that name to FREETYPE_LIBRARIES.
    if (NOT TARGET freetype)
        add_library(freetype INTERFACE IMPORTED GLOBAL)
        target_link_libraries(freetype INTERFACE ${FREETYPE_LIBRARIES})
    endif ()
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

if (SLIC3R_CAD)
    target_link_libraries(orca_slicer_core PUBLIC SLVS::slvs)
endif ()

if(WIN32)
    # The streaming digest's OpenSSL fallback also needs its static CAPI engine.
    target_link_libraries(orca_slicer_core PRIVATE Psapi.lib bcrypt.lib Crypt32.lib)
endif()

if(SLIC3R_PROFILE)
    target_link_libraries(orca_slicer_core PRIVATE Shiny)
endif()

if (WIN32)
    # Public, since BlacklistedLibraryCheck.hpp includes windows.h. Empty
    # WIN32_LEAN_AND_MEAN matches the sources that define it themselves; bare
    # NOMINMAX matches the one libigl already passes.
    target_compile_definitions(orca_slicer_core PUBLIC "WIN32_LEAN_AND_MEAN=" "NOMINMAX")
endif ()

if (SLIC3R_PCH AND NOT SLIC3R_SYNTAXONLY)
    add_precompiled_header(orca_slicer_core pchheader.hpp FORCEINCLUDE)
endif ()
