cmake_minimum_required(VERSION 3.20)
project(OrcaImageMapProbe LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(IMAGEMAP_SOURCE_DIR "" CACHE PATH "Fixed, hash-verified upstream files prepared by prepare.ps1")
if(NOT EXISTS "${IMAGEMAP_SOURCE_DIR}/deps_src/colorsolver/ColorSolver.cpp")
    message(FATAL_ERROR "Run prepare.ps1 and pass IMAGEMAP_SOURCE_DIR")
endif()
get_filename_component(ORCA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
find_package(PNG REQUIRED)
add_executable(imagemap_probe
    main.cpp ImageMapColorMixSolver.cpp
    "${IMAGEMAP_SOURCE_DIR}/deps_src/colorsolver/ColorSolver.cpp"
    "${IMAGEMAP_SOURCE_DIR}/deps_src/pigment-painter/pigment_painter_mixer.cpp"
    "${IMAGEMAP_SOURCE_DIR}/deps_src/pigment-painter/lut_wide.png.c"
    "${IMAGEMAP_SOURCE_DIR}/deps_src/prusa-fdm-mixer/prusa_fdm_mixer.cpp")
target_include_directories(imagemap_probe PRIVATE
    "${ORCA_SOURCE_DIR}/deps_src" "${IMAGEMAP_SOURCE_DIR}"
    "${IMAGEMAP_SOURCE_DIR}/deps_src/colorsolver"
    "${IMAGEMAP_SOURCE_DIR}/deps_src/pigment-painter"
    "${IMAGEMAP_SOURCE_DIR}/deps_src/prusa-fdm-mixer")
target_link_libraries(imagemap_probe PRIVATE PNG::PNG)
if(MSVC)
    target_compile_definitions(imagemap_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(imagemap_probe PRIVATE /utf-8 /wd4309)
    target_link_libraries(imagemap_probe PRIVATE psapi)
endif()
