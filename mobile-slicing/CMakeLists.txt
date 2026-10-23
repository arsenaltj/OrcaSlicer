cmake_minimum_required(VERSION 3.24)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
project(orca_mobile_slicing LANGUAGES C CXX)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "This entry is for Android arm64-v8a; use the pinned NDK toolchain and -DANDROID_ABI=arm64-v8a")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_ANDROID_API 24)
set(ANDROID_STL c++_static CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 -Wl,--exclude-libs,ALL")
set(ORCA_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "OrcaSlicer source root")
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
    "${ORCA_SOURCE_ROOT}/src/libslic3r/MobileTriangulation.cpp"
    "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/platform/AndroidSha256Digest.cpp")
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
file(READ "${ORCA_SOURCE_ROOT}/resources/info/nozzle_info.json" ORCA_MOBILE_NOZZLE_INFO)
file(READ "${ORCA_SOURCE_ROOT}/resources/info/nozzle_incompatibles.json" ORCA_MOBILE_NOZZLE_INCOMPATIBLES)
configure_file("${CMAKE_CURRENT_LIST_DIR}/nozzle_resources.hpp.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/orca_mobile_nozzle_resources.hpp" @ONLY)
add_library(orca_slicer_core STATIC ${ORCA_LIBSLIC3R_CPP})
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
target_compile_options(orca_slicer_core PRIVATE -fvisibility=hidden)
target_include_directories(orca_slicer_core
    PRIVATE "${ORCA_SOURCE_ROOT}/src/libslic3r"
    PUBLIC "${ORCA_SOURCE_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ORCA_CHECK_GCODE_PLACEHOLDERS 0 CACHE STRING "" FORCE)
configure_file("${ORCA_SOURCE_ROOT}/src/libslic3r/libslic3r_version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/libslic3r_version.h" @ONLY)

target_link_libraries(orca_slicer_core PUBLIC
    Eigen3::Eigen admesh libigl libnest2d miniz qoi semver::semver expat Clipper2 glu-libtess mcut qhull
    TBB::tbb TBB::tbbmalloc
    orca_boost_headers cereal
    Boost::filesystem Boost::system Boost::thread Boost::log
    Boost::locale Boost::regex Boost::chrono Boost::date_time Boost::iostreams Boost::atomic
    noise::noise
    ${CMAKE_DL_LIBS} log)

if(EXISTS "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/c_api.cpp" AND EXISTS "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/engine.cpp")
    add_library(orca_mobile_slicing_sdk SHARED
        "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/c_api.cpp"
        "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/engine.cpp"
        "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/project_support.cpp"
        "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/recommendation_support.cpp"
)
    execute_process(COMMAND git -C "${ORCA_SOURCE_ROOT}" rev-parse HEAD
        OUTPUT_VARIABLE ORCA_MOBILE_SLICING_SOURCE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _orca_identity_status)
    if(NOT _orca_identity_status EQUAL 0 OR NOT ORCA_MOBILE_SLICING_SOURCE_COMMIT MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "Native slicing requires a verified source commit")
    endif()
    target_compile_definitions(orca_mobile_slicing_sdk PRIVATE ORCA_MOBILE_ANDROID=1
        ORCA_MOBILE_SLICING_SOURCE_COMMIT="${ORCA_MOBILE_SLICING_SOURCE_COMMIT}")
    target_include_directories(orca_mobile_slicing_sdk PRIVATE "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/include" "${ORCA_SOURCE_ROOT}/src")
    target_link_libraries(orca_mobile_slicing_sdk PRIVATE OrcaSlicer::Core log)
    set_target_properties(orca_mobile_slicing_sdk PROPERTIES OUTPUT_NAME orca_mobile_slicing_sdk_c CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_link_options(orca_mobile_slicing_sdk PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_LIST_DIR}/exports/slicing.map")
    add_executable(orca_mobile_slicing_json_gate "${CMAKE_CURRENT_LIST_DIR}/slicing_json_gate.cpp")
    target_include_directories(orca_mobile_slicing_json_gate PRIVATE "${ORCA_SOURCE_ROOT}/src/orca_mobile_slicing_sdk/include")
    target_link_libraries(orca_mobile_slicing_json_gate PRIVATE orca_mobile_slicing_sdk)
endif()

add_executable(orca_mobile_stl_gate "${CMAKE_CURRENT_LIST_DIR}/stl_gcode_gate.cpp")
target_compile_definitions(orca_mobile_stl_gate PRIVATE ORCA_MOBILE_ANDROID=1)
target_include_directories(orca_mobile_stl_gate PRIVATE "${ORCA_SOURCE_ROOT}/src")
target_link_libraries(orca_mobile_stl_gate PRIVATE OrcaSlicer::Core log)

install(TARGETS orca_slicer_core ARCHIVE DESTINATION lib)
if(TARGET orca_mobile_slicing_sdk)
    install(TARGETS orca_mobile_slicing_sdk LIBRARY DESTINATION lib)
endif()
install(TARGETS orca_mobile_stl_gate RUNTIME DESTINATION bin)
