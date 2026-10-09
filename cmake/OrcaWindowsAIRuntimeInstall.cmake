# Included from src after the executable and base Windows runtime install rules.
if(SLIC3R_GUI)
    include("${CMAKE_SOURCE_DIR}/cmake/OrcaSemanticRuntime.cmake")
    orca_stage_semantic_runtime(OrcaSlicer)
endif()

if(ORCA_AI_WINDOWS_INSTALLER)
    # These rules intentionally follow the base runtime install above. CPack
    # executes them while staging the NSIS payload, so a package cannot be
    # produced unless its own isolated Python can import the pinned local stack.
    install(DIRECTORY "${ORCA_AI_PILLOW_STAGE_DIR}/" DESTINATION "python/Lib/site-packages")
    if(ORCA_PORTRAIT_STAGE_DIR)
        install(DIRECTORY "${ORCA_PORTRAIT_STAGE_DIR}/" DESTINATION "resources/beauty-runtime")
        if(ORCA_BEAUTY_RASTER_BINARY)
            set(_portrait_raster "${ORCA_BEAUTY_RASTER_BINARY}")
        else()
            set(_portrait_raster "$<TARGET_FILE:orca_semantic_raster>")
        endif()
        install(FILES "${_portrait_raster}" DESTINATION "resources/beauty-runtime/modules"
            RENAME "local_semantic_raster.dll")
        install(CODE "
            execute_process(COMMAND \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/resources/beauty-runtime/python/python.exe\" -I
                \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/resources/beauty-runtime/modules/bundled_portrait_runtime.py\"
                --root \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/resources/beauty-runtime\"
                --raster \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/resources/beauty-runtime/modules/local_semantic_raster.dll\"
                RESULT_VARIABLE _portrait_verified)
            if(NOT _portrait_verified EQUAL 0)
                message(FATAL_ERROR \"Installed offline portrait runtime failed integrity/import verification\")
            endif()")
    endif()
    install(CODE "
        set(_orca_ai_installed_python \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/python/python.exe\")
        if(NOT EXISTS \"\${_orca_ai_installed_python}\")
            message(FATAL_ERROR \"Bundled Python is missing from the Windows AI install tree\")
        endif()
        execute_process(
            COMMAND \"\${_orca_ai_installed_python}\" -I
                \"${CMAKE_SOURCE_DIR}/tools/ai/verify_bundled_runtime.py\"
                --python-root \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/python\"
                --expect-python \"${_bundled_python_version}\"
                --expect-pillow \"${ORCA_AI_PILLOW_VERSION}\"
            RESULT_VARIABLE _orca_ai_pillow_import_result
            OUTPUT_QUIET
            ERROR_QUIET)
        if(NOT _orca_ai_pillow_import_result EQUAL 0)
            message(FATAL_ERROR
                \"Bundled local Python stack failed the isolated install-tree import check\")
        endif()
    ")
endif()
