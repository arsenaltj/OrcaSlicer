cmake_minimum_required(VERSION 3.13)

set(_target orca_connectivity_moonraker)

# The SDK-only entry point returns before the desktop dependency block, so keep
# the transport's public dependencies local to this target. The desktop build
# already provides the same imported targets before src/ is added.
if (POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost 1.83.0 CONFIG REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

add_library(${_target} STATIC
    MoonrakerAdapter.cpp
    MoonrakerTransport.cpp
    include/orca/connectivity/moonraker/MoonrakerAdapter.hpp
    include/orca/connectivity/moonraker/MoonrakerTransport.hpp
)
add_library(OrcaSlicer::MoonrakerAdapter ALIAS ${_target})

target_include_directories(${_target}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if (Boost_INCLUDE_DIRS)
    target_include_directories(${_target} SYSTEM PUBLIC ${Boost_INCLUDE_DIRS})
endif()
target_link_libraries(${_target}
    PUBLIC
        OrcaSlicer::MobileSDK
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
)
if (TARGET Boost::system)
    target_link_libraries(${_target} PUBLIC Boost::system)
endif()
if (WIN32)
    target_link_libraries(${_target} PUBLIC crypt32)
endif()
target_compile_features(${_target} PUBLIC cxx_std_17)
set_property(TARGET ${_target} PROPERTY FOLDER "mobile_sdk")
