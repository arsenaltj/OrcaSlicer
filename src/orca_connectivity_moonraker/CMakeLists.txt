cmake_minimum_required(VERSION 3.13)

set(_target orca_connectivity_moonraker)

add_library(${_target} STATIC
    MoonrakerAdapter.cpp
    include/orca/connectivity/moonraker/MoonrakerAdapter.hpp
)
add_library(OrcaSlicer::MoonrakerAdapter ALIAS ${_target})

target_include_directories(${_target}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(${_target}
    PUBLIC
        OrcaSlicer::MobileSDK
)
target_compile_features(${_target} PUBLIC cxx_std_17)
set_property(TARGET ${_target} PROPERTY FOLDER "mobile_sdk")
