cmake_minimum_required(VERSION 3.24)
project(SmartSliceCoreRegression LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ORCA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# Use the bundled amalgamation for both implementation and public headers.
# The split Catch headers can belong to a newer revision than extras/.
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated/catch2/matchers)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/catch2/catch_test_macros.hpp
    "#include <catch_amalgamated.hpp>\n")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/catch2/matchers/catch_matchers_floating_point.hpp
    "#include <catch_amalgamated.hpp>\n")
add_executable(smart_slice_core_tests
    ../catch2/extras/catch_amalgamated.cpp
    ../slic3rutils/test_smart_slice_engine.cpp
    ${ORCA_ROOT}/src/slic3r/AI/SmartSlicing/Application/SmartSlicingEngine.cpp
    ${ORCA_ROOT}/src/slic3r/AI/SmartSlicing/Domain/SmartSliceTypes.cpp)
target_include_directories(smart_slice_core_tests PRIVATE
    ${ORCA_ROOT}/src ${CMAKE_CURRENT_BINARY_DIR}/generated ../catch2/extras)
if(MSVC)
    target_compile_options(smart_slice_core_tests PRIVATE /utf-8 /EHsc)
endif()
enable_testing()
add_test(NAME SmartSliceCoreRegression COMMAND smart_slice_core_tests "[SmartSlice]" --order rand)

# Optional native bridge regression. The generated config supplies only pinned
# Core/dependency libraries and include paths; all adapter sources build here.
if(ORCA_BRIDGE_REFERENCE_CONFIG)
    include(${ORCA_BRIDGE_REFERENCE_CONFIG})
    add_executable(smart_slice_bridge_tests bridge_contract.cpp
        ${ORCA_ROOT}/src/orca_mobile_slicing_sdk/smart_slice_bridge.cpp
        ${ORCA_ROOT}/src/orca_mobile_slicing_sdk/project_support.cpp
        ${ORCA_ROOT}/src/slic3r/GUI/AI/Orca/OrcaModelPreparation.cpp
        ${ORCA_ROOT}/src/slic3r/AI/SmartSlicing/Application/SmartSlicingEngine.cpp
        ${ORCA_ROOT}/src/slic3r/AI/SmartSlicing/Domain/SmartSliceTypes.cpp)
    target_include_directories(smart_slice_bridge_tests PRIVATE ${ORCA_BRIDGE_INCLUDES})
    target_compile_definitions(smart_slice_bridge_tests PRIVATE ${ORCA_BRIDGE_DEFINITIONS})
    target_compile_options(smart_slice_bridge_tests PRIVATE /utf-8 /EHsc /bigobj)
    target_link_libraries(smart_slice_bridge_tests PRIVATE ${ORCA_BRIDGE_LIBRARIES})
    add_test(NAME SmartSliceBridgeRegression COMMAND smart_slice_bridge_tests)
endif()

# Compile the immutable pre-sync source in a separate namespace against exactly
# the same input as the new engine. This is executable evidence, not a copied oracle.
find_package(Git REQUIRED)
set(_legacy_root "${CMAKE_CURRENT_BINARY_DIR}/legacy")
foreach(_file Application/SmartSlicingEngine.cpp Application/SmartSlicingEngine.hpp Domain/SmartSliceTypes.cpp Domain/SmartSliceTypes.hpp)
    execute_process(COMMAND "${GIT_EXECUTABLE}" show "e8d8dea3b2cb5705c62a41ac7a1a2273cec6539a:src/slic3r/AI/SmartSlicing/${_file}"
        WORKING_DIRECTORY "${ORCA_ROOT}" OUTPUT_VARIABLE _old RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Pinned e8 baseline source is unavailable")
    endif()
    string(REPLACE "Slic3r::AI::SmartSlicing" "Slic3r::AI::LegacySmartSlicing" _old "${_old}")
    get_filename_component(_parent "${_legacy_root}/${_file}" DIRECTORY)
    file(MAKE_DIRECTORY "${_parent}")
    file(WRITE "${_legacy_root}/${_file}" "${_old}")
endforeach()
add_executable(smart_slice_old_new old_new.cpp
    "${_legacy_root}/Application/SmartSlicingEngine.cpp" "${_legacy_root}/Domain/SmartSliceTypes.cpp"
    "${ORCA_ROOT}/src/slic3r/AI/SmartSlicing/Application/SmartSlicingEngine.cpp"
    "${ORCA_ROOT}/src/slic3r/AI/SmartSlicing/Domain/SmartSliceTypes.cpp")
target_include_directories(smart_slice_old_new PRIVATE "${ORCA_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(smart_slice_old_new PRIVATE /utf-8 /EHsc)
endif()
add_test(NAME SmartSliceOldNew COMMAND smart_slice_old_new)
