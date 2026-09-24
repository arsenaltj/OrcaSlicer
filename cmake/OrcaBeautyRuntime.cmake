if(WIN32)
    add_library(orca_semantic_raster SHARED "${CMAKE_SOURCE_DIR}/tools/ai/local_semantic_raster.cpp")
    set_target_properties(orca_semantic_raster PROPERTIES OUTPUT_NAME "local_semantic_raster")
    target_compile_features(orca_semantic_raster PRIVATE cxx_std_17)
    if(TARGET OrcaSlicer_app_gui)
        add_dependencies(OrcaSlicer_app_gui orca_semantic_raster)
    endif()
    install(TARGETS orca_semantic_raster RUNTIME DESTINATION "./resources/tools/ai")
endif()

set(ORCA_BEAUTY_RUNTIME_ROOT "" CACHE PATH "Prepared portable beauty recognition runtime (no downloads)")
if(ORCA_BEAUTY_RUNTIME_ROOT)
    if(NOT EXISTS "${ORCA_BEAUTY_RUNTIME_ROOT}/runtime-manifest.json" OR
       NOT EXISTS "${ORCA_BEAUTY_RUNTIME_ROOT}/python/python.exe")
        message(FATAL_ERROR "Incomplete portable beauty runtime")
    endif()
    install(DIRECTORY "${ORCA_BEAUTY_RUNTIME_ROOT}/" DESTINATION "./resources/beauty-runtime"
        PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
endif()
