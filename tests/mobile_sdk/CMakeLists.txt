get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

if (NOT COMMAND orcaslicer_discover_tests)
    function(orcaslicer_discover_tests TARGET)
        catch_discover_tests(${TARGET} ADD_TAGS_AS_LABELS)
    endfunction()
endif()

add_executable(${_TEST_NAME}_tests
    test_mobile_sdk_contracts.cpp
    test_moonraker_adapter.cpp
    test_wondermaker_adapter.cpp
)

target_link_libraries(${_TEST_NAME}_tests PRIVATE
    OrcaSlicer::MobileDeviceSDK
    Catch2::Catch2WithMain
)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")
orcaslicer_discover_tests(${_TEST_NAME}_tests)

add_executable(${_TEST_NAME}_cabi_tests
    test_mobile_sdk_c_header.c
    test_mobile_sdk_c_abi.cpp
    test_mobile_sdk_c_exports.cpp
    test_mobile_sdk_threading.cpp
)
target_link_libraries(${_TEST_NAME}_cabi_tests PRIVATE
    OrcaSlicer::MobileDeviceSDKShared
    Catch2::Catch2WithMain
)
target_compile_features(${_TEST_NAME}_cabi_tests PRIVATE c_std_11 cxx_std_17)
set_target_properties(${_TEST_NAME}_cabi_tests PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    FOLDER "tests"
)
target_compile_definitions(${_TEST_NAME}_cabi_tests PRIVATE ORCA_MOBILE_DEVICE_BUILD_SHARED)
add_custom_command(TARGET ${_TEST_NAME}_cabi_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:orca_mobile_device_sdk_ffi>
            $<TARGET_FILE_DIR:${_TEST_NAME}_cabi_tests>
)
orcaslicer_discover_tests(${_TEST_NAME}_cabi_tests)

# Run the shared digest contract against the Android backend on the host,
# without pulling desktop/OpenSSL dependencies into the SDK-only build.
add_executable(mobile_slicing_platform_digest_tests
    ../libslic3r/test_sha256_digest.cpp
    ../../src/orca_mobile_slicing_sdk/platform/AndroidSha256Digest.cpp)
target_include_directories(mobile_slicing_platform_digest_tests PRIVATE ../../src)
target_link_libraries(mobile_slicing_platform_digest_tests PRIVATE Catch2::Catch2WithMain)
target_compile_features(mobile_slicing_platform_digest_tests PRIVATE cxx_std_17)
orcaslicer_discover_tests(mobile_slicing_platform_digest_tests)

# Optional pinned Boost headers let the SDK-only host build validate the Android
# MD5 compatibility adapter without adding Boost to the Device SDK dependency graph.
if(ORCA_PLATFORM_TEST_BOOST_SOURCE_DIR)
    file(GLOB _orca_test_boost_headers LIST_DIRECTORIES true
         "${ORCA_PLATFORM_TEST_BOOST_SOURCE_DIR}/libs/*/include")
    add_executable(mobile_slicing_platform_md5_tests test_mobile_digest.cpp)
    target_compile_features(mobile_slicing_platform_md5_tests PRIVATE cxx_std_17)
    target_compile_definitions(mobile_slicing_platform_md5_tests PRIVATE ORCA_MOBILE_ANDROID=1)
    target_include_directories(mobile_slicing_platform_md5_tests PRIVATE ../../src
        ${_orca_test_boost_headers}
        "${ORCA_PLATFORM_TEST_BOOST_SOURCE_DIR}/libs/numeric/conversion/include")
    target_link_libraries(mobile_slicing_platform_md5_tests PRIVATE Catch2::Catch2WithMain)
    orcaslicer_discover_tests(mobile_slicing_platform_md5_tests)
endif()
