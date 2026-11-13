# Shared by retained GUI presenters and their independent lifecycle tests.
add_library(orca_ui_runtime STATIC ${CMAKE_CURRENT_LIST_DIR}/UiRuntime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/AsyncPreviewImage.cpp
    ${CMAKE_CURRENT_LIST_DIR}/WindowCloseController.cpp)
target_compile_definitions(orca_ui_runtime PUBLIC wxDEBUG_LEVEL=0)
target_include_directories(orca_ui_runtime PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(orca_ui_runtime PUBLIC boost_libs TBB::tbb ${wxWidgets_LIBRARIES})
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_include_directories(orca_ui_runtime SYSTEM PUBLIC ${wxWidgets_INCLUDE_DIRS})
    target_compile_definitions(orca_ui_runtime PUBLIC ${wxWidgets_DEFINITIONS}
        $<$<CONFIG:Debug>:${wxWidgets_DEFINITIONS_DEBUG}>)
endif ()
