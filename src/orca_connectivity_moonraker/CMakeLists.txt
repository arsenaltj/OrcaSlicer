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
        OrcaSlicer::MobileDeviceSDK
)
target_compile_features(${_target} PUBLIC cxx_std_17)
set_property(TARGET ${_target} PROPERTY FOLDER "mobile_sdk")

# The concrete network backend is an opt-in host component. Device C ABI
# libraries retain their pure protocol dependency graph on every platform.
set(ORCA_MOONRAKER_CONCRETE_TRANSPORT "OFF" CACHE STRING "Build native Moonraker host transports (OFF, AUTO, ON)")
set_property(CACHE ORCA_MOONRAKER_CONCRETE_TRANSPORT PROPERTY STRINGS OFF AUTO ON)
if(NOT ORCA_MOONRAKER_CONCRETE_TRANSPORT MATCHES "^(OFF|AUTO|ON)$")
    message(FATAL_ERROR "ORCA_MOONRAKER_CONCRETE_TRANSPORT must be OFF, AUTO or ON")
endif()
set(ORCA_MOONRAKER_BOOST_SOURCE_DIR "" CACHE PATH "Optional pinned modular Boost source headers for the host transport")
set(ORCA_MOONRAKER_CONCRETE_TRANSPORT_AVAILABLE OFF)
if(NOT ORCA_MOONRAKER_CONCRETE_TRANSPORT STREQUAL "OFF")
    set(_moonraker_boost_includes)
    if(ORCA_MOONRAKER_BOOST_SOURCE_DIR)
        file(GLOB _moonraker_component_headers LIST_DIRECTORIES true
             "${ORCA_MOONRAKER_BOOST_SOURCE_DIR}/libs/*/include")
        list(APPEND _moonraker_boost_includes ${_moonraker_component_headers}
             "${ORCA_MOONRAKER_BOOST_SOURCE_DIR}/libs/numeric/conversion/include")
    endif()
    find_path(_moonraker_beast_headers boost/beast/websocket.hpp
        HINTS ${_moonraker_boost_includes} ${Boost_INCLUDE_DIRS}
              "${ORCA_SDK_DEPENDENCY_PREFIX}/include")
    find_package(CURL QUIET)
    find_package(OpenSSL QUIET)
    find_package(Threads REQUIRED)
    if(_moonraker_beast_headers AND TARGET CURL::libcurl AND TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
        set(ORCA_MOONRAKER_CONCRETE_TRANSPORT_AVAILABLE ON)
        add_library(orca_moonraker_transport STATIC MoonrakerTransport.cpp
            include/orca/connectivity/moonraker/MoonrakerTransport.hpp)
        add_library(OrcaSlicer::MoonrakerTransport ALIAS orca_moonraker_transport)
        target_compile_features(orca_moonraker_transport PUBLIC cxx_std_17)
        target_include_directories(orca_moonraker_transport SYSTEM PRIVATE
            ${_moonraker_boost_includes} ${_moonraker_beast_headers})
        target_compile_definitions(orca_moonraker_transport PRIVATE BOOST_ERROR_CODE_HEADER_ONLY)
        if(MSVC)
            target_compile_options(orca_moonraker_transport PRIVATE /bigobj)
        endif()
        target_link_libraries(orca_moonraker_transport PUBLIC OrcaSlicer::MobileDeviceSDK
            PRIVATE CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
        if(WIN32)
            target_link_libraries(orca_moonraker_transport PRIVATE ws2_32 crypt32)
        endif()
    elseif(ORCA_MOONRAKER_CONCRETE_TRANSPORT STREQUAL "ON")
        message(FATAL_ERROR "Native Moonraker transport requires Boost.Beast headers, CURL::libcurl, OpenSSL::SSL and OpenSSL::Crypto. Supply their verified dependency prefixes or select OFF/AUTO.")
    endif()
    message(STATUS "Native Moonraker host transport available: ${ORCA_MOONRAKER_CONCRETE_TRANSPORT_AVAILABLE}")
endif()
