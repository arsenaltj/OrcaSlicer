get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

if (NOT COMMAND orcaslicer_discover_tests)
    function(orcaslicer_discover_tests TARGET)
        catch_discover_tests(${TARGET} ADD_TAGS_AS_LABELS)
    endfunction()
endif()
add_executable(${_TEST_NAME}_tests
    test_mobile_sdk_contracts.cpp
)

target_link_libraries(${_TEST_NAME}_tests PRIVATE
    OrcaSlicer::MobileSDK
    Catch2::Catch2WithMain
)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

orcaslicer_discover_tests(${_TEST_NAME}_tests)
