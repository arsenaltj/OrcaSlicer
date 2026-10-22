cmake_minimum_required(VERSION 3.13)

if (ORCA_MOBILE_DEVICE_SDK_ONLY)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
else()
    find_package(Python3 COMPONENTS Interpreter QUIET)
endif()

set(ORCA_MOBILE_DEVICE_SDK_CONTRACT_VERSION "1.0.0")

set(ORCA_MOBILE_DEVICE_SDK_SOURCES
    c_api.cpp
    c_api_internal.hpp
    include/orca/mobile/device_sdk_c.h
    Connectivity.cpp
    CommandEnvelope.cpp
    ../orca_connectivity_moonraker/MoonrakerAdapter.cpp
    ../orca_connectivity_wondermaker/WonderMakerAdapter.cpp
    include/orca/mobile/device/CommandEnvelope.hpp
    include/orca/mobile/device/Connectivity.hpp
    include/orca/mobile/device/DeviceSDK.hpp
)

set(ORCA_MOBILE_DEVICE_SDK_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${ORCA_MOBILE_DEVICE_SDK_GENERATED_INCLUDE_DIR}/orca/mobile/device")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/MobileDeviceSDKBuildInfo.hpp.in"
    "${ORCA_MOBILE_DEVICE_SDK_GENERATED_INCLUDE_DIR}/orca/mobile/device/MobileDeviceSDKBuildInfo.hpp"
    @ONLY
)

add_library(orca_mobile_device_sdk STATIC ${ORCA_MOBILE_DEVICE_SDK_SOURCES})
add_library(OrcaSlicer::MobileDeviceSDK ALIAS orca_mobile_device_sdk)

# The FFI target is a separate shared library so desktop code can continue to
# consume the static C++ SDK while Flutter loads a stable native artifact.
add_library(orca_mobile_device_sdk_ffi SHARED ${ORCA_MOBILE_DEVICE_SDK_SOURCES})
add_library(OrcaSlicer::MobileDeviceSDKShared ALIAS orca_mobile_device_sdk_ffi)
set_target_properties(orca_mobile_device_sdk_ffi PROPERTIES
    OUTPUT_NAME "orca_mobile_device_sdk_c"
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
if(UNIX AND NOT APPLE)
    target_link_options(orca_mobile_device_sdk_ffi PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports/device.map"
    )
endif()

target_include_directories(orca_mobile_device_sdk
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${ORCA_MOBILE_DEVICE_SDK_GENERATED_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/deps_src
        ${CMAKE_CURRENT_SOURCE_DIR}/../orca_connectivity_moonraker/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../orca_connectivity_wondermaker/include
)
target_include_directories(orca_mobile_device_sdk_ffi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${ORCA_MOBILE_DEVICE_SDK_GENERATED_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/deps_src
)

target_include_directories(orca_mobile_device_sdk_ffi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${ORCA_MOBILE_DEVICE_SDK_GENERATED_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/deps_src
        ${CMAKE_CURRENT_SOURCE_DIR}/../orca_connectivity_moonraker/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../orca_connectivity_wondermaker/include
)

target_link_libraries(orca_mobile_device_sdk PUBLIC nlohmann_json)
target_link_libraries(orca_mobile_device_sdk_ffi PUBLIC nlohmann_json)
target_compile_features(orca_mobile_device_sdk PUBLIC cxx_std_17)
target_compile_features(orca_mobile_device_sdk_ffi PUBLIC cxx_std_17)
target_compile_definitions(orca_mobile_device_sdk PUBLIC ORCA_MOBILE_DEVICE_SDK_OPEN_SOURCE_BUILD=1)
target_compile_definitions(orca_mobile_device_sdk_ffi
    PUBLIC ORCA_MOBILE_DEVICE_SDK_OPEN_SOURCE_BUILD=1
    PRIVATE ORCA_MOBILE_DEVICE_BUILD_SHARED ORCA_MOBILE_DEVICE_BUILDING
    INTERFACE ORCA_MOBILE_DEVICE_BUILD_SHARED
)
source_group(TREE ${CMAKE_SOURCE_DIR}/src FILES ${ORCA_MOBILE_DEVICE_SDK_SOURCES})

# The SDK is a public-source target. It deliberately has no dependency on
# wxWidgets, Python, GUI targets, vendor DLLs, or commercial service code.
if (Python3_Interpreter_FOUND)
    add_custom_target(orca_mobile_device_sdk_boundary_check ALL
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/verify_mobile_sdk_boundary.py
                --source-root ${CMAKE_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking public MobileDeviceSDK boundaries"
        VERBATIM
    )
endif()

install(TARGETS orca_mobile_device_sdk orca_mobile_device_sdk_ffi
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)
install(FILES "${ORCA_MOBILE_DEVICE_SDK_GENERATED_INCLUDE_DIR}/orca/mobile/device/MobileDeviceSDKBuildInfo.hpp"
        DESTINATION include/orca/mobile/device)


