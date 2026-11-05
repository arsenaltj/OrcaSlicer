cmake_minimum_required(VERSION 3.24)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
project(orca_mobile_slicing LANGUAGES C CXX)
option(ORCA_SDK_HOST "Build standalone host SDK Core without the desktop GUI" OFF)
option(ORCA_SDK_ACCEPTANCE_BUILD "SDK-only moderate optimization for local compilation acceptance" OFF)
option(ORCA_SDK_PRECOMPILED_HEADERS "Reuse stable SDK Core C++ headers during standalone builds" OFF)
if(NOT ORCA_SDK_HOST AND (NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a"))
    message(FATAL_ERROR "This entry is for Android arm64-v8a; use the pinned NDK toolchain and -DANDROID_ABI=arm64-v8a")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(ANDROID)
 set(CMAKE_ANDROID_API 24)
 set(ANDROID_STL c++_static CACHE STRING "" FORCE)
 set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 -Wl,--exclude-libs,ALL")
 set(ORCA_SDK_PLATFORM_LIBS log)
else()
 set(ORCA_SDK_PLATFORM_LIBS "")
endif()
set(ORCA_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "OrcaSlicer source root")
option(ORCA_MOBILE_REAL_TRIANGULATION "Compile existing shared Core CGAL constrained triangulation" OFF)
set(ORCA_MOBILE_CGAL_INCLUDE "" CACHE PATH "Verified CGAL 6.2.1 include root")
set(ORCA_DEPS_SOURCE "${ORCA_SOURCE_ROOT}/deps_src")
include(FetchContent)
set(BOOST_ENABLE_CMAKE ON CACHE BOOL "" FORCE)
set(BOOST_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(BOOST_SKIP_INSTALL_RULES ON CACHE BOOL "" FORCE)
set(BOOST_INCLUDE_LIBRARIES "system;filesystem;thread;log;locale;regex;chrono;date_time;iostreams;atomic" CACHE STRING "" FORCE)
set(BOOST_FILESYSTEM_DISABLE_STATX ON CACHE BOOL "Disable statx for Android API 24" FORCE)
set(BOOST_FILESYSTEM_HAS_STATX OFF CACHE BOOL "Disable statx detection for Android API 24" FORCE)
set(BOOST_LOCALE_ENABLE_ICONV OFF CACHE BOOL "Disable iconv on Android API 24" FORCE)
FetchContent_Declare(boost
    URL https://github.com/boostorg/boost/releases/download/boost-1.84.0/boost-1.84.0.tar.gz
    URL_HASH SHA256=4d27e9efed0f6f152dc28db6430b9d3dfb40c0345da7342eaa5a987dde57bd95)
FetchContent_MakeAvailable(boost)
FetchContent_GetProperties(boost)
file(GLOB _orca_boost_component_includes LIST_DIRECTORIES true "${boost_SOURCE_DIR}/libs/*/include")
add_library(orca_boost_headers INTERFACE)
target_include_directories(orca_boost_headers INTERFACE "${boost_SOURCE_DIR}" "${boost_SOURCE_DIR}/libs/headers/include" ${_orca_boost_component_includes})
add_library(boost_headeronly ALIAS orca_boost_headers)
set(TBB_TEST OFF CACHE BOOL "" FORCE)
set(TBB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TBB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(TBB_ENABLE_IPO OFF CACHE BOOL "" FORCE)
set(TBBMALLOC_BUILD ON CACHE BOOL "" FORCE)
set(JUST_INSTALL_CEREAL ON CACHE BOOL "" FORCE)
set(SKIP_PERFORMANCE_COMPARISON ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(cereal
    URL https://mirror.nju.edu.cn/macports/distfiles/cereal/cereal-1.3.0.tar.gz
    URL_HASH SHA256=9E6AAC6FA2835BBC0361718945D0FE468C6B1283A212A271257A1F1A8C418991)
FetchContent_MakeAvailable(cereal)
FetchContent_Declare(tbb
    URL https://github.com/oneapi-src/oneTBB/archive/refs/tags/v2021.5.0.zip
    URL_HASH SHA256=83ea786c964a384dd72534f9854b419716f412f9d43c0be88d41874763e7bb47)
FetchContent_MakeAvailable(tbb)
if(TARGET tbbmalloc AND ANDROID)
    set_target_properties(tbbmalloc PROPERTIES LINK_FLAGS "" LINK_DEPENDS "")
endif()
FetchContent_Declare(eigen
    URL https://gitlab.com/libeigen/eigen/-/archive/5.0.1/eigen-5.0.1.zip
    URL_HASH SHA256=0dbb1f9e3aaad66f352c03227d8c983f6f0b49e0b07e71a7300f4abcc01aee12)
FetchContent_MakeAvailable(eigen)
set(NLOPT_PYTHON OFF CACHE BOOL "" FORCE)
set(NLOPT_OCTAVE OFF CACHE BOOL "" FORCE)
set(NLOPT_MATLAB OFF CACHE BOOL "" FORCE)
set(NLOPT_GUILE OFF CACHE BOOL "" FORCE)
set(NLOPT_SWIG OFF CACHE BOOL "" FORCE)
set(NLOPT_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlopt
    URL https://github.com/stevengj/nlopt/archive/v2.5.0.tar.gz
    URL_HASH SHA256=c6dd7a5701fff8ad5ebb45a3dc8e757e61d52658de3918e38bab233e7fd3b4ae)
FetchContent_MakeAvailable(nlopt)
if(TARGET nlopt AND NOT TARGET NLopt::nlopt)
    add_library(NLopt::nlopt ALIAS nlopt)
endif()
set(LIBNOISE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libnoise
    URL https://github.com/SoftFever/Orca-deps-libnoise/archive/refs/tags/1.0.zip
    URL_HASH SHA256=96ffd6cc47898dd8147aab53d7d1b1911b507d9dbaecd5613ca2649468afd8b6)
FetchContent_MakeAvailable(libnoise)
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib URL https://github.com/madler/zlib/archive/refs/tags/v1.2.13.zip
 URL_HASH SHA256=c2856951bbf30e30861ace3765595d86ba13f2cf01279d901f6c62258c57f4ff)
FetchContent_MakeAvailable(zlib)
if(NOT TARGET ZLIB::ZLIB)
 add_library(ZLIB::ZLIB INTERFACE IMPORTED GLOBAL)
 set_target_properties(ZLIB::ZLIB PROPERTIES INTERFACE_LINK_LIBRARIES "$<BUILD_INTERFACE:zlibstatic>")
 set_target_properties(ZLIB::ZLIB PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}")
endif()
set(ZLIB_LIBRARY zlibstatic CACHE STRING "" FORCE)
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE STRING "" FORCE)
FetchContent_Declare(png URL https://github.com/glennrp/libpng/archive/refs/tags/v1.6.59.zip
 URL_HASH SHA256=6fd97a1e5e2ac55cec0a538a9800a351eb3844086736291844ebd12db72ba433)
FetchContent_MakeAvailable(png)
# Expose the desktop libnoise/ include layout in this build directory.
# Stage only public headers; dependency sources and shared core stay unchanged.
FetchContent_GetProperties(libnoise)
if(NOT EXISTS "${libnoise_SOURCE_DIR}/src/module/modulebase.h")
    message(FATAL_ERROR "Pinned libnoise source headers are missing")
endif()
file(COPY "${libnoise_SOURCE_DIR}/src/"
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/generated/libnoise"
     FILES_MATCHING PATTERN "*.h")
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/generated/libnoise/module/modulebase.h")
    message(FATAL_ERROR "Could not stage the libnoise public header layout")
endif()
if(TARGET noise AND NOT TARGET noise::noise)
    add_library(noise::noise ALIAS noise)
endif()

set(EXPAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${ORCA_DEPS_SOURCE}/nlohmann" "deps/nlohmann")
# Reuse the desktop pure SmartSlicing core; this subdirectory has no GUI/JNI sources.
set(SLIC3R_ENC_CHECK OFF CACHE BOOL "" FORCE)
add_subdirectory("${ORCA_SOURCE_ROOT}/src/slic3r/AI" "orca_ai")
add_subdirectory("${ORCA_DEPS_SOURCE}/fast_float" "deps/fast_float")
add_subdirectory("${ORCA_DEPS_SOURCE}/admesh" "deps/admesh")
target_include_directories(admesh SYSTEM PUBLIC "${ORCA_SOURCE_ROOT}/src")
target_link_libraries(admesh PUBLIC orca_boost_headers Boost::log)
# Upstream v2.5 uses Clipper2; the removed legacy clipper target is not rebuilt.
add_subdirectory("${ORCA_DEPS_SOURCE}/clipper2" "deps/clipper2")
add_subdirectory("${ORCA_DEPS_SOURCE}/expat" "deps/expat")
add_subdirectory("${ORCA_DEPS_SOURCE}/glu-libtess" "deps/glu-libtess")
add_subdirectory("${ORCA_DEPS_SOURCE}/libigl" "deps/libigl")
add_subdirectory("${ORCA_DEPS_SOURCE}/mcut" "deps/mcut")
add_subdirectory("${ORCA_DEPS_SOURCE}/miniz" "deps/miniz")
add_subdirectory("${ORCA_DEPS_SOURCE}/qhull" "deps/qhull")
add_subdirectory("${ORCA_DEPS_SOURCE}/qoi" "deps/qoi")
add_subdirectory("${ORCA_DEPS_SOURCE}/semver" "deps/semver")
if(MSVC)
 # Clipper2's upstream headers do not annotate Windows DLL exports. Its
 # standalone shared target must generate a real import library for consumers.
 set_target_properties(Clipper2 PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
 foreach(_sdk_dependency IN ITEMS admesh Clipper2)
  target_compile_definitions(${_sdk_dependency} PRIVATE _USE_MATH_DEFINES BOOST_ALL_NO_LIB __TBB_NO_IMPLICIT_LINKAGE=1 __TBBMALLOC_NO_IMPLICIT_LINKAGE=1)
  target_compile_options(${_sdk_dependency} PRIVATE /utf-8 /bigobj)
 endforeach()
endif()

add_library(libnest2d INTERFACE)
target_include_directories(libnest2d INTERFACE "${ORCA_DEPS_SOURCE}/libnest2d/include")
target_link_libraries(libnest2d INTERFACE NLopt::nlopt orca_boost_headers TBB::tbb)

set(ORCA_SLIC3R_CORE_SOURCE_DIR "${ORCA_SOURCE_ROOT}/src/libslic3r")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(orca_slicer_core_manifest_check ALL
        COMMAND ${Python3_EXECUTABLE}
                ${ORCA_SOURCE_ROOT}/scripts/verify_core_source_manifest.py
                --source-root ${ORCA_SOURCE_ROOT}
        WORKING_DIRECTORY ${ORCA_SOURCE_ROOT}
        COMMENT "Checking shared OrcaSlicer::Core source manifest"
        VERBATIM
    )
endif()

include("${ORCA_SLIC3R_CORE_SOURCE_DIR}/core_sources.cmake")
set(ORCA_LIBSLIC3R_CPP ${ORCA_SLIC3R_CORE_SOURCES})
list(FILTER ORCA_LIBSLIC3R_CPP INCLUDE REGEX "\\.(cpp|mm)$")
# Android-only support units are part of the mobile build but are intentionally
# outside the desktop source inventory.
list(APPEND ORCA_LIBSLIC3R_CPP
    "${ORCA_SOURCE_ROOT}/src/libslic3r/ExPolygonCollection.cpp"
    "${ORCA_SOURCE_ROOT}/src/libslic3r/JumpPointSearch.cpp"
    "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/platform/AndroidSha256Digest.cpp")
if(NOT ORCA_MOBILE_REAL_TRIANGULATION)
 list(APPEND ORCA_LIBSLIC3R_CPP "${ORCA_SOURCE_ROOT}/src/libslic3r/MobileTriangulation.cpp")
endif()
set(_mobile_excludes
    "/Sha256Digest.cpp$"
    "/SLA/"
    "/SLAPrint.cpp$" "/SLAPrintSteps.cpp$"
    "/Format/AssimpImport.cpp$" "/Format/DRC.cpp$" "/Format/OBJ.cpp$"
    "/Format/SL1.cpp$" "/Format/STEP.cpp$" "/Format/svg.cpp$"
    "/ObjColorUtils.cpp$"
    "/TexturePainting.cpp$" "/TextureToColor/"
    "/PNGReadWrite.cpp$" "/MeshBoolean.cpp$" "/CutSurface.cpp$" "/IntersectionPoints.cpp$"
    "/Triangulation.cpp$" "/TryCatchSignal"
    "/OpenVDBUtils.cpp$" "/Geometry/VoronoiUtilsCgal.cpp$"
    "/Shape/TextShape.cpp$" "/Emboss.cpp"
    "/GCodeSender.cpp$" "/ProjectTask.cpp"
)
foreach(_pattern IN LISTS _mobile_excludes)
    list(FILTER ORCA_LIBSLIC3R_CPP EXCLUDE REGEX "${_pattern}")
endforeach()
if(ORCA_MOBILE_REAL_TRIANGULATION)
 find_package(Python3 COMPONENTS Interpreter REQUIRED)
 set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ORCA_SOURCE_ROOT}/src/libslic3r/MeshBoolean.cpp")
 set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/extract_parameterization.py")
 execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/extract_parameterization.py"
  "${ORCA_SOURCE_ROOT}/src/libslic3r/MeshBoolean.cpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/MeshParameterization.cpp"
  COMMAND_ERROR_IS_FATAL ANY)
 if(NOT EXISTS "${ORCA_MOBILE_CGAL_INCLUDE}/CGAL/version.h")
  message(FATAL_ERROR "Executable triangulation requires verified CGAL 6.2.1 public headers")
 endif()
 list(APPEND ORCA_LIBSLIC3R_CPP
  "${ORCA_SOURCE_ROOT}/src/libslic3r/Emboss.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/MeshParameterization.cpp"
  "${ORCA_SOURCE_ROOT}/src/libslic3r/PNGReadWrite.cpp"
  "${ORCA_SOURCE_ROOT}/src/libslic3r/SLA/IndexedMesh.cpp"
  "${ORCA_SOURCE_ROOT}/src/libslic3r/Triangulation.cpp"
  "${ORCA_SOURCE_ROOT}/src/libslic3r/IntersectionPoints.cpp"
  "${ORCA_SOURCE_ROOT}/src/libslic3r/MeshRepair.cpp")
 # CGAL's existing Boost multiprecision backend avoids importing host GMP DLLs
 # into the Android artifact. Core algorithms and their public API stay unchanged.
 set_source_files_properties(
  "${ORCA_SOURCE_ROOT}/src/libslic3r/Triangulation.cpp"
  "${ORCA_SOURCE_ROOT}/src/libslic3r/IntersectionPoints.cpp"
  "${ORCA_SOURCE_ROOT}/src/libslic3r/MeshRepair.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/MeshParameterization.cpp"
  PROPERTIES COMPILE_DEFINITIONS "CGAL_DISABLE_GMP=1;CGAL_NO_CORE=1"
  SKIP_PRECOMPILE_HEADERS ON
  INCLUDE_DIRECTORIES "${ORCA_MOBILE_CGAL_INCLUDE}")
endif()
set_source_files_properties("${ORCA_SOURCE_ROOT}/src/libslic3r/PNGReadWrite.cpp"
 PROPERTIES INCLUDE_DIRECTORIES "${png_SOURCE_DIR};${png_BINARY_DIR};${zlib_SOURCE_DIR};${zlib_BINARY_DIR}")
file(READ "${ORCA_SOURCE_ROOT}/resources/info/nozzle_info.json" ORCA_MOBILE_NOZZLE_INFO)
file(READ "${ORCA_SOURCE_ROOT}/resources/info/nozzle_incompatibles.json" ORCA_MOBILE_NOZZLE_INCOMPATIBLES)
configure_file("${CMAKE_CURRENT_LIST_DIR}/nozzle_resources.hpp.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/orca_mobile_nozzle_resources.hpp" @ONLY)
add_library(orca_slicer_core STATIC ${ORCA_LIBSLIC3R_CPP})
if(MSVC)
 add_library(orca_sdk_nowide STATIC
  "${boost_SOURCE_DIR}/libs/nowide/src/console_buffer.cpp"
  "${boost_SOURCE_DIR}/libs/nowide/src/cstdio.cpp"
  "${boost_SOURCE_DIR}/libs/nowide/src/cstdlib.cpp"
  "${boost_SOURCE_DIR}/libs/nowide/src/filebuf.cpp"
  "${boost_SOURCE_DIR}/libs/nowide/src/iostream.cpp"
  "${boost_SOURCE_DIR}/libs/nowide/src/stat.cpp")
 target_link_libraries(orca_sdk_nowide PRIVATE orca_boost_headers)
 target_compile_definitions(orca_sdk_nowide PRIVATE BOOST_ALL_NO_LIB BOOST_NOWIDE_STATIC_LINK)
 set_source_files_properties("${ORCA_SOURCE_ROOT}/src/libslic3r/Emboss.cpp"
  PROPERTIES COMPILE_DEFINITIONS "UNICODE;_UNICODE" SKIP_PRECOMPILE_HEADERS ON)
 target_sources(orca_slicer_core PRIVATE "${ORCA_SOURCE_ROOT}/src/libslic3r/TryCatchSignalSEH.cpp")
 target_link_libraries(orca_slicer_core PRIVATE orca_sdk_nowide Gdi32.lib User32.lib)
endif()
target_sources(orca_slicer_core PRIVATE "${CMAKE_CURRENT_LIST_DIR}/nanosvg_impl.cpp")
target_link_libraries(orca_slicer_core PRIVATE "$<LINK_ONLY:png_static>" "$<LINK_ONLY:zlibstatic>")
# The mobile implementation is a platform-specific build of the same public
# core target consumed by the desktop configure.
add_library(OrcaSlicer::Core ALIAS orca_slicer_core)
target_compile_definitions(orca_slicer_core PUBLIC
    ORCA_MOBILE_ANDROID=1
    USE_TBB=1
    TBB_USE_CAPTURED_EXCEPTION=0
    BOOST_ALL_NO_LIB=1
    BOOST_ERROR_CODE_HEADER_ONLY=1
    BOOST_SYSTEM_NO_DEPRECATED=1
    ORCA_CHECK_GCODE_PLACEHOLDERS=0)
if(NOT MSVC)
 target_compile_options(orca_slicer_core PRIVATE -fvisibility=hidden)
else()
 target_compile_definitions(orca_slicer_core PUBLIC _USE_MATH_DEFINES PRIVATE __TBB_NO_IMPLICIT_LINKAGE=1 __TBBMALLOC_NO_IMPLICIT_LINKAGE=1)
 target_compile_options(orca_slicer_core PRIVATE /utf-8 /bigobj)
 # The desktop root enables the conforming lambda frontend. Preserve that
 # behavior for the implementation units that require constexpr local lookup,
 # without invalidating unrelated SDK Core objects or changing upstream source.
 foreach(_sdk_lambda_source IN ITEMS Fill/FillRectilinear.cpp GCode/GCodeProcessor.cpp
  GCode/SeamPlacer.cpp Model.cpp TextureDisplacement.cpp)
  set_source_files_properties("${ORCA_SOURCE_ROOT}/src/libslic3r/${_sdk_lambda_source}"
   PROPERTIES COMPILE_OPTIONS "/Zc:lambda" SKIP_PRECOMPILE_HEADERS ON)
 endforeach()
 target_link_libraries(orca_slicer_core PRIVATE Psapi.lib bcrypt.lib Crypt32.lib)
endif()
if(ORCA_SDK_ACCEPTANCE_BUILD)
 if(MSVC)
  target_compile_options(orca_slicer_core PRIVATE /O1)
 else()
  target_compile_options(orca_slicer_core PRIVATE -O1)
 endif()
endif()
if(ORCA_SDK_PRECOMPILED_HEADERS)
 # These implementation units configure Boost/Eigen/CGAL before their first
 # domain include. Do not move that configuration across a forced PCH boundary.
 foreach(_local_macro_source IN ITEMS Arrange.cpp Geometry.cpp MinAreaBoundingBox.cpp
  OpenVDBUtils.cpp Orient.cpp Preset.cpp Sha256Digest.cpp Fill/FillAdaptive.cpp
  SLA/SupportTreeBuilder.cpp)
  set_source_files_properties("${ORCA_SOURCE_ROOT}/src/libslic3r/${_local_macro_source}"
   PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
 endforeach()
 target_precompile_headers(orca_slicer_core PRIVATE
  <algorithm> <array> <cmath> <memory> <string> <vector>
  <Eigen/Core> <Eigen/Geometry> <boost/filesystem/path.hpp>
  "${ORCA_SOURCE_ROOT}/src/libslic3r/Point.hpp"
  "${ORCA_SOURCE_ROOT}/src/libslic3r/TriangleMesh.hpp")
endif()
if(ORCA_MOBILE_REAL_TRIANGULATION)
 target_compile_definitions(orca_slicer_core PUBLIC ORCA_MOBILE_REAL_TRIANGULATION=1)
endif()
target_include_directories(orca_slicer_core
    PRIVATE "${ORCA_SOURCE_ROOT}/src/libslic3r"
    PUBLIC "${ORCA_SOURCE_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}/generated"
           "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/platform")
set(ORCA_CHECK_GCODE_PLACEHOLDERS 0 CACHE STRING "" FORCE)
configure_file("${ORCA_SOURCE_ROOT}/src/libslic3r/libslic3r_version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/libslic3r_version.h" @ONLY)

target_link_libraries(orca_slicer_core PUBLIC
    Eigen3::Eigen admesh libigl libnest2d miniz qoi semver::semver expat Clipper2 glu-libtess mcut qhull
    TBB::tbb TBB::tbbmalloc
    orca_boost_headers cereal
    Boost::filesystem Boost::system Boost::thread Boost::log
    Boost::locale Boost::regex Boost::chrono Boost::date_time Boost::iostreams Boost::atomic
    noise::noise
    ${CMAKE_DL_LIBS} ${ORCA_SDK_PLATFORM_LIBS})

if(EXISTS "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/c_api.cpp" AND EXISTS "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/engine.cpp")
    add_library(orca_mobile_slicing_sdk SHARED
        "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/c_api.cpp"
        "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/engine.cpp"
        "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/project_support.cpp"
        "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/recommendation_support.cpp"
        "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/smart_slice_bridge.cpp"
        "${ORCA_SOURCE_ROOT}/src/slic3r/GUI/AI/Orca/OrcaModelPreparation.cpp"
)
    execute_process(COMMAND git -C "${ORCA_SOURCE_ROOT}" rev-parse HEAD
        OUTPUT_VARIABLE ORCA_MOBILE_SLICING_SOURCE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _orca_identity_status)
    if(NOT _orca_identity_status EQUAL 0 OR NOT ORCA_MOBILE_SLICING_SOURCE_COMMIT MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "Native slicing requires a verified source commit")
    endif()
    target_compile_definitions(orca_mobile_slicing_sdk PRIVATE ORCA_MOBILE_ANDROID=1 ORCA_SLICING_BUILD
        ORCA_MOBILE_SLICING_SOURCE_COMMIT="${ORCA_MOBILE_SLICING_SOURCE_COMMIT}")
    target_compile_definitions(orcaslicer_ai_smart_slicing PRIVATE ORCA_SMART_SLICING_SOURCE_COMMIT="${ORCA_MOBILE_SLICING_SOURCE_COMMIT}")
    target_include_directories(orca_mobile_slicing_sdk PRIVATE "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/include" "${ORCA_SOURCE_ROOT}/src")
    target_link_libraries(orca_mobile_slicing_sdk PRIVATE OrcaSlicer::Core OrcaSlicer::SmartSlicingCore ${ORCA_SDK_PLATFORM_LIBS})
    set_target_properties(orca_mobile_slicing_sdk PROPERTIES OUTPUT_NAME orca_mobile_slicing_sdk_c CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    if(ANDROID)
     target_link_options(orca_mobile_slicing_sdk PRIVATE "-Wl,--no-undefined" "-Wl,--version-script=${CMAKE_CURRENT_LIST_DIR}/exports/slicing.map")
    endif()
    if(MSVC)
     target_compile_definitions(orca_mobile_slicing_sdk PRIVATE _USE_MATH_DEFINES __TBB_NO_IMPLICIT_LINKAGE=1 __TBBMALLOC_NO_IMPLICIT_LINKAGE=1)
     target_compile_options(orca_mobile_slicing_sdk PRIVATE /utf-8 /bigobj /Zc:lambda)
    endif()
    add_executable(orca_mobile_slicing_json_gate "${CMAKE_CURRENT_LIST_DIR}/slicing_json_gate.cpp")
    target_include_directories(orca_mobile_slicing_json_gate PRIVATE "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/include")
    target_link_libraries(orca_mobile_slicing_json_gate PRIVATE orca_mobile_slicing_sdk)
endif()

add_executable(orca_mobile_stl_gate "${CMAKE_CURRENT_LIST_DIR}/stl_gcode_gate.cpp")
target_compile_definitions(orca_mobile_stl_gate PRIVATE ORCA_MOBILE_ANDROID=1)
target_include_directories(orca_mobile_stl_gate PRIVATE "${ORCA_SOURCE_ROOT}/src")
target_link_libraries(orca_mobile_stl_gate PRIVATE OrcaSlicer::Core ${ORCA_SDK_PLATFORM_LIBS})

install(TARGETS orca_slicer_core ARCHIVE DESTINATION lib)
if(TARGET orca_mobile_slicing_sdk)
    install(TARGETS orca_mobile_slicing_sdk LIBRARY DESTINATION lib)
endif()
install(TARGETS orca_mobile_stl_gate RUNTIME DESTINATION bin)
