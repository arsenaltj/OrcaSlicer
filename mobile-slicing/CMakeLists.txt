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
set(CMAKE_ANDROID_API 35)
set(ANDROID_STL c++_static CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384")
set(ORCA_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "OrcaSlicer source root")
set(ORCA_DEPS_SOURCE "${ORCA_SOURCE_ROOT}/deps_src")
include(FetchContent)
set(BOOST_ENABLE_CMAKE ON CACHE BOOL "" FORCE)
set(BOOST_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(BOOST_SKIP_INSTALL_RULES ON CACHE BOOL "" FORCE)
set(BOOST_INCLUDE_LIBRARIES "system;filesystem;thread;log;locale;regex;chrono;date_time;iostreams;atomic" CACHE STRING "" FORCE)
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
if(TARGET noise AND NOT TARGET noise::noise)
    add_library(noise::noise ALIAS noise)
endif()

set(EXPAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${ORCA_DEPS_SOURCE}/nlohmann" "deps/nlohmann")
add_subdirectory("${ORCA_DEPS_SOURCE}/fast_float" "deps/fast_float")
add_subdirectory("${ORCA_DEPS_SOURCE}/admesh" "deps/admesh")
target_include_directories(admesh SYSTEM PUBLIC "${ORCA_SOURCE_ROOT}/src")
target_link_libraries(admesh PUBLIC orca_boost_headers Boost::log)
add_library(clipper STATIC "${ORCA_DEPS_SOURCE}/clipper/clipper_z.cpp")
target_include_directories(clipper SYSTEM PUBLIC "${ORCA_DEPS_SOURCE}/clipper" "${ORCA_SOURCE_ROOT}/src")
target_link_libraries(clipper PUBLIC Eigen3::Eigen TBB::tbb)
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

file(GLOB_RECURSE ORCA_LIBSLIC3R_CPP CONFIGURE_DEPENDS "${ORCA_SOURCE_ROOT}/src/libslic3r/*.cpp")
set(_mobile_excludes
    "/SLA/"
    "/SLAPrint.cpp$" "/SLAPrintSteps.cpp$"
    "/Format/AssimpImport.cpp$" "/Format/DRC.cpp$" "/Format/OBJ.cpp$" "/Format/objparser.cpp$"
    "/Format/SL1.cpp$" "/Format/STEP.cpp$" "/Format/svg.cpp$"
    "/ObjColorUtils.cpp$"
    "/TexturePainting.cpp$" "/TextureToColor/"
    "/PNGReadWrite.cpp$" "/MeshBoolean.cpp$" "/CutSurface.cpp$" "/IntersectionPoints.cpp$"
    "/Triangulation.cpp$" "/TryCatchSignal"
    "/OpenVDBUtils.cpp$" "/Geometry/VoronoiUtilsCgal.cpp$"
    "/Shape/TextShape.cpp$" "/Emboss.cpp"
    "/pchheader.cpp$"
    "/GCodeSender.cpp$" "/ProjectTask.cpp"
)
foreach(_pattern IN LISTS _mobile_excludes)
    list(FILTER ORCA_LIBSLIC3R_CPP EXCLUDE REGEX "${_pattern}")
endforeach()

add_library(libslic3r_mobile STATIC ${ORCA_LIBSLIC3R_CPP})
target_compile_definitions(libslic3r_mobile PUBLIC
    ORCA_MOBILE_ANDROID=1
    USE_TBB=1
    TBB_USE_CAPTURED_EXCEPTION=0
    BOOST_ALL_NO_LIB=1
    BOOST_ERROR_CODE_HEADER_ONLY=1
    BOOST_SYSTEM_NO_DEPRECATED=1
    ORCA_CHECK_GCODE_PLACEHOLDERS=0)
target_compile_options(libslic3r_mobile PRIVATE -fvisibility=hidden)
target_include_directories(libslic3r_mobile
    PRIVATE "${ORCA_SOURCE_ROOT}/src/libslic3r"
    PUBLIC "${ORCA_SOURCE_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ORCA_CHECK_GCODE_PLACEHOLDERS 0 CACHE STRING "" FORCE)
configure_file("${ORCA_SOURCE_ROOT}/src/libslic3r/libslic3r_version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/libslic3r_version.h" @ONLY)

target_link_libraries(libslic3r_mobile PUBLIC
    Eigen3::Eigen admesh libigl libnest2d miniz qoi semver::semver expat clipper Clipper2 glu-libtess mcut qhull
    TBB::tbb TBB::tbbmalloc
    orca_boost_headers cereal
    Boost::filesystem Boost::system Boost::thread Boost::log
    Boost::locale Boost::regex Boost::chrono Boost::date_time Boost::iostreams Boost::atomic
    noise::noise
    ${CMAKE_DL_LIBS} log)

if(EXISTS "${ORCA_SOURCE_ROOT}/src/orca_slicing/c_api.cpp" AND EXISTS "${ORCA_SOURCE_ROOT}/src/orca_slicing/engine.cpp")
    add_library(orca_slicing_c SHARED
        "${ORCA_SOURCE_ROOT}/src/orca_slicing/c_api.cpp"
        "${ORCA_SOURCE_ROOT}/src/orca_slicing/engine.cpp"
        "${ORCA_SOURCE_ROOT}/src/orca_slicing/project_support.cpp"
        "${ORCA_SOURCE_ROOT}/src/orca_slicing/recommendation_support.cpp"
        "${ORCA_SOURCE_ROOT}/src/orca_slicing/android_jni.cpp")
    target_compile_definitions(orca_slicing_c PRIVATE ORCA_MOBILE_ANDROID=1)
    target_include_directories(orca_slicing_c PRIVATE "${ORCA_SOURCE_ROOT}/src/orca_slicing/include" "${ORCA_SOURCE_ROOT}/src")
    target_link_libraries(orca_slicing_c PRIVATE libslic3r_mobile log)
    set_target_properties(orca_slicing_c PROPERTIES OUTPUT_NAME orca_slicing_c)
endif()

add_executable(orca_mobile_stl_gate "${CMAKE_CURRENT_LIST_DIR}/stl_gcode_gate.cpp")
target_compile_definitions(orca_mobile_stl_gate PRIVATE ORCA_MOBILE_ANDROID=1)
target_include_directories(orca_mobile_stl_gate PRIVATE "${ORCA_SOURCE_ROOT}/src")
target_link_libraries(orca_mobile_stl_gate PRIVATE libslic3r_mobile log)

install(TARGETS libslic3r_mobile ARCHIVE DESTINATION lib)
if(TARGET orca_slicing_c)
    install(TARGETS orca_slicing_c LIBRARY DESTINATION lib)
endif()
install(TARGETS orca_mobile_stl_gate RUNTIME DESTINATION bin)
