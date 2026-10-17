cmake_minimum_required(VERSION 3.13)

set(_target orca_connectivity_moonraker)

# The SDK-only entry point returns before the desktop dependency block, so keep
# the transport's public dependencies local to this target. The desktop build
# already provides the same imported targets before src/ is added.
# The SDK-only configure intentionally does not require BoostConfig.cmake. Beast
# and Asio are header based for this target; a repository/system include prefix
# plus an optional boost_system library is sufficient. This also lets the
# baseline SDK configure with the adapter-only target when optional dependencies
# are not installed.
set(_moonraker_transport_available FALSE)
set(_moonraker_boost_include_dirs "")
set(_moonraker_boost_system_target "")
set(_moonraker_missing_dependencies "")

if (NOT ORCA_MOONRAKER_CONCRETE_TRANSPORT STREQUAL "OFF")
    if (Boost_INCLUDE_DIRS)
        list(APPEND _moonraker_boost_include_dirs ${Boost_INCLUDE_DIRS})
    endif()
    if (Boost_INCLUDE_DIR)
        list(APPEND _moonraker_boost_include_dirs ${Boost_INCLUDE_DIR})
    endif()
    if (ORCA_SDK_DEPENDENCY_PREFIX)
        list(APPEND _moonraker_boost_include_dirs
            "${ORCA_SDK_DEPENDENCY_PREFIX}/include"
            "${ORCA_SDK_DEPENDENCY_PREFIX}")
    endif()
    list(APPEND _moonraker_boost_include_dirs
        "${CMAKE_SOURCE_DIR}/deps/include"
        "${CMAKE_SOURCE_DIR}/deps/Boost")
    find_path(_moonraker_boost_headers
        NAMES boost/beast/websocket.hpp boost/asio.hpp
        HINTS ${_moonraker_boost_include_dirs}
        PATH_SUFFIXES include)

    if (TARGET Boost::system)
        set(_moonraker_boost_system_target Boost::system)
    else()
        find_library(_moonraker_boost_system_library
            NAMES boost_system boost_system-mt
            HINTS
                "${ORCA_SDK_DEPENDENCY_PREFIX}/lib"
                "${ORCA_SDK_DEPENDENCY_PREFIX}/lib64"
                "${CMAKE_SOURCE_DIR}/deps/lib")
    endif()

    find_package(CURL QUIET)
    find_package(OpenSSL QUIET)
    find_package(ZLIB QUIET)

    if (NOT _moonraker_boost_headers)
        list(APPEND _moonraker_missing_dependencies "Boost headers")
    endif()
    if (NOT CURL_FOUND OR NOT (TARGET CURL::libcurl OR CURL_LIBRARIES))
        list(APPEND _moonraker_missing_dependencies "libcurl")
    endif()
    if (NOT OPENSSL_FOUND OR NOT (TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto))
        list(APPEND _moonraker_missing_dependencies "OpenSSL")
    endif()
    if (NOT ZLIB_FOUND OR NOT (TARGET ZLIB::ZLIB OR ZLIB_LIBRARIES))
        list(APPEND _moonraker_missing_dependencies "ZLIB")
    endif()

    if (NOT _moonraker_missing_dependencies)
        set(_moonraker_transport_available TRUE)
    endif()
endif()

if (ORCA_MOONRAKER_CONCRETE_TRANSPORT STREQUAL "ON" AND NOT _moonraker_transport_available)
    message(FATAL_ERROR
        "Concrete Moonraker transport requested but its dependencies were not found. "
        "Provide -DORCA_SDK_DEPENDENCY_PREFIX=<prefix> containing Boost headers "
        "(boost/asio.hpp and boost/beast/websocket.hpp), libcurl, OpenSSL and ZLIB, "
        "or use -DORCA_MOONRAKER_CONCRETE_TRANSPORT=AUTO.")
endif()

set(ORCA_MOONRAKER_CONCRETE_TRANSPORT_AVAILABLE ${_moonraker_transport_available}
    CACHE INTERNAL "Whether the concrete Moonraker transport is built")

set(_moonraker_sources
    MoonrakerAdapter.cpp
    include/orca/connectivity/moonraker/MoonrakerAdapter.hpp
    include/orca/connectivity/moonraker/MoonrakerTransport.hpp
)
if (_moonraker_transport_available)
    list(APPEND _moonraker_sources MoonrakerTransport.cpp)
    message(STATUS "Moonraker concrete transport: enabled")
else()
    if (_moonraker_missing_dependencies)
        string(JOIN ", " _moonraker_missing_text ${_moonraker_missing_dependencies})
    else()
        set(_moonraker_missing_text "disabled by ORCA_MOONRAKER_CONCRETE_TRANSPORT=OFF")
    endif()
    message(STATUS "Moonraker concrete transport: disabled (adapter-only SDK target; missing: ${_moonraker_missing_text})")
endif()

add_library(${_target} STATIC ${_moonraker_sources})
add_library(OrcaSlicer::MoonrakerAdapter ALIAS ${_target})

target_include_directories(${_target}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if (_moonraker_transport_available)
    target_include_directories(${_target} SYSTEM PUBLIC ${_moonraker_boost_headers})
endif()
target_link_libraries(${_target} PUBLIC OrcaSlicer::MobileSDK)
if (_moonraker_transport_available)
    # Keep the same wrapper boundary as the desktop build. In SDK-only mode
    # this target is created locally; in the desktop build the existing wrapper
    # supplies the package-specific libcurl dependencies.
    if (NOT TARGET libcurl)
        add_library(libcurl INTERFACE)
    endif()
    if (TARGET CURL::libcurl)
        target_link_libraries(libcurl INTERFACE CURL::libcurl)
    else()
        target_include_directories(libcurl SYSTEM INTERFACE ${CURL_INCLUDE_DIRS})
        target_link_libraries(libcurl INTERFACE ${CURL_LIBRARIES})
    endif()
    if (TARGET ZLIB::ZLIB)
        target_link_libraries(libcurl INTERFACE ZLIB::ZLIB)
    elseif (ZLIB_LIBRARIES)
        target_link_libraries(libcurl INTERFACE ${ZLIB_LIBRARIES})
    endif()
    if (WIN32)
        target_link_libraries(libcurl INTERFACE crypt32 ws2_32)
    endif()
    target_link_libraries(${_target} PUBLIC libcurl OpenSSL::SSL OpenSSL::Crypto)
    if (_moonraker_boost_system_target)
        target_link_libraries(${_target} PUBLIC ${_moonraker_boost_system_target})
    elseif (_moonraker_boost_system_library)
        target_link_libraries(${_target} PUBLIC ${_moonraker_boost_system_library})
    else()
        target_compile_definitions(${_target} PUBLIC BOOST_ERROR_CODE_HEADER_ONLY)
    endif()
endif()
target_compile_features(${_target} PUBLIC cxx_std_17)
set_property(TARGET ${_target} PROPERTY FOLDER "mobile_sdk")
