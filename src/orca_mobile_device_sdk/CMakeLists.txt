cmake_minimum_required(VERSION 3.13)

if (ORCA_MOBILE_SDK_ONLY)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
else()
    find_package(Python3 COMPONENTS Interpreter QUIET)
endif()

set(ORCA_MOBILE_SDK_CONTRACT_VERSION "1.0.0")

set(ORCA_MOBILE_SDK_SOURCES
    c_api.cpp
    c_api_internal.hpp
    include/orca/sdk_c.h
    Connectivity.cpp
    CommandEnvelope.cpp
    ModelGeneration.cpp
    Slicing.cpp
    include/orca/sdk/CommandEnvelope.hpp
    include/orca/sdk/Connectivity.hpp
    include/orca/sdk/MobileSDK.hpp
    include/orca/sdk/ModelGeneration.hpp
    include/orca/sdk/Slicing.hpp
)

set(ORCA_MOBILE_SDK_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${ORCA_MOBILE_SDK_GENERATED_INCLUDE_DIR}/orca/sdk")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/MobileSDKBuildInfo.hpp.in"
    "${ORCA_MOBILE_SDK_GENERATED_INCLUDE_DIR}/orca/sdk/MobileSDKBuildInfo.hpp"
    @ONLY
)

add_library(orca_mobile_sdk STATIC ${ORCA_MOBILE_SDK_SOURCES})
add_library(OrcaSlicer::MobileSDK ALIAS orca_mobile_sdk)

# The FFI target is a separate shared library so desktop code can continue to
# consume the static C++ SDK while Flutter loads a stable native artifact.
add_library(orca_mobile_sdk_ffi SHARED ${ORCA_MOBILE_SDK_SOURCES})
add_library(OrcaSlicer::MobileSDKShared ALIAS orca_mobile_sdk_ffi)
set_target_properties(orca_mobile_sdk_ffi PROPERTIES
    OUTPUT_NAME "orca_mobile_sdk_c"
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
)

target_include_directories(orca_mobile_sdk
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${ORCA_MOBILE_SDK_GENERATED_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/deps_src
)
target_include_directories(orca_mobile_sdk_ffi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${ORCA_MOBILE_SDK_GENERATED_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/deps_src
)

target_link_libraries(orca_mobile_sdk PUBLIC nlohmann_json)
target_link_libraries(orca_mobile_sdk_ffi PUBLIC nlohmann_json)
target_compile_features(orca_mobile_sdk PUBLIC cxx_std_17)
target_compile_features(orca_mobile_sdk_ffi PUBLIC cxx_std_17)
target_compile_definitions(orca_mobile_sdk PUBLIC ORCA_MOBILE_SDK_OPEN_SOURCE_BUILD=1)
target_compile_definitions(orca_mobile_sdk_ffi
    PUBLIC ORCA_MOBILE_SDK_OPEN_SOURCE_BUILD=1
    PRIVATE ORCA_SDK_BUILD_SHARED ORCA_SDK_BUILDING
    INTERFACE ORCA_SDK_BUILD_SHARED
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ORCA_MOBILE_SDK_SOURCES})

# The SDK is a public-source target. It deliberately has no dependency on
# wxWidgets, Python, GUI targets, vendor DLLs, or commercial service code.
if (Python3_Interpreter_FOUND)
    add_custom_target(orca_mobile_sdk_boundary_check ALL
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/verify_mobile_sdk_boundary.py
                --source-root ${CMAKE_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking public MobileSDK boundaries"
        VERBATIM
    )
endif()

install(TARGETS orca_mobile_sdk orca_mobile_sdk_ffi
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)
install(FILES "${ORCA_MOBILE_SDK_GENERATED_INCLUDE_DIR}/orca/sdk/MobileSDKBuildInfo.hpp"
        DESTINATION include/orca/sdk)
