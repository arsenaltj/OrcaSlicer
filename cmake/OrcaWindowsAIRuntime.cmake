if(WIN32)
    option(ORCA_AI_WINDOWS_INSTALLER "Package the integrated local AI runtime in the Windows installer" OFF)
endif()

# The image preprocessing pipeline imports Pillow at Sidecar startup. Keep the
# Windows AI runtime self-contained: fetch one architecture-specific CPython
# wheel from the official PyPI file host, verify its immutable digest, and stage
# its contents for both build-tree smoke tests and the offline installer.
if(WIN32 AND ORCA_AI_WINDOWS_INSTALLER)
    set(ORCA_AI_PILLOW_VERSION "12.2.0")
    string(TOLOWER "${CMAKE_GENERATOR_PLATFORM};${CMAKE_SYSTEM_PROCESSOR}" _orca_ai_target_arch)
    if(_orca_ai_target_arch MATCHES "arm64|aarch64")
        set(ORCA_AI_RUNTIME_ARCHITECTURE "arm64")
        set(ORCA_AI_PILLOW_WHEEL_FILENAME "pillow-12.2.0-cp312-cp312-win_arm64.whl")
        set(ORCA_AI_PILLOW_WHEEL_URL
            "https://files.pythonhosted.org/packages/10/e1/542a474affab20fd4a0f1836cb234e8493519da6b76899e30bcc5d990b8b/pillow-12.2.0-cp312-cp312-win_arm64.whl")
        set(ORCA_AI_PILLOW_WHEEL_SHA256
            "af73337013e0b3b46f175e79492d96845b16126ddf79c438d7ea7ff27783a414")
    elseif(_orca_ai_target_arch MATCHES "amd64|x86_64|x64")
        set(ORCA_AI_RUNTIME_ARCHITECTURE "x64")
        set(ORCA_AI_PILLOW_WHEEL_FILENAME "pillow-12.2.0-cp312-cp312-win_amd64.whl")
        set(ORCA_AI_PILLOW_WHEEL_URL
            "https://files.pythonhosted.org/packages/5d/7b/25a221d2c761c6a8ae21bfa3874988ff2583e19cf8a27bf2fee358df7942/pillow-12.2.0-cp312-cp312-win_amd64.whl")
        set(ORCA_AI_PILLOW_WHEEL_SHA256
            "7f84204dee22a783350679a0333981df803dac21a0190d706a50475e361c93f5")
    else()
        message(FATAL_ERROR
            "The Windows AI installer supports only x64 and arm64; target architecture was '${_orca_ai_target_arch}'")
    endif()

    set(ORCA_AI_WHEEL_CACHE_DIR "${CMAKE_BINARY_DIR}/_deps/orca_ai_wheels")
    set(ORCA_AI_PILLOW_WHEEL "${ORCA_AI_WHEEL_CACHE_DIR}/${ORCA_AI_PILLOW_WHEEL_FILENAME}")
    set(ORCA_AI_PILLOW_STAGE_DIR "${CMAKE_BINARY_DIR}/_deps/orca_ai_python_packages/pillow-${ORCA_AI_PILLOW_VERSION}")
    file(MAKE_DIRECTORY "${ORCA_AI_WHEEL_CACHE_DIR}")
    if(EXISTS "${ORCA_AI_PILLOW_WHEEL}")
        file(SHA256 "${ORCA_AI_PILLOW_WHEEL}" _orca_ai_cached_pillow_sha256)
        if(NOT "${_orca_ai_cached_pillow_sha256}" STREQUAL "${ORCA_AI_PILLOW_WHEEL_SHA256}")
            file(REMOVE "${ORCA_AI_PILLOW_WHEEL}")
        endif()
    endif()
    if(NOT EXISTS "${ORCA_AI_PILLOW_WHEEL}")
        message(STATUS "Downloading pinned Pillow ${ORCA_AI_PILLOW_VERSION} wheel for ${ORCA_AI_RUNTIME_ARCHITECTURE}")
        file(DOWNLOAD
            "${ORCA_AI_PILLOW_WHEEL_URL}"
            "${ORCA_AI_PILLOW_WHEEL}"
            EXPECTED_HASH "SHA256=${ORCA_AI_PILLOW_WHEEL_SHA256}"
            STATUS _orca_ai_pillow_download_status
            TLS_VERIFY ON
            TIMEOUT 120
            INACTIVITY_TIMEOUT 30)
        list(GET _orca_ai_pillow_download_status 0 _orca_ai_pillow_download_code)
        if(NOT _orca_ai_pillow_download_code EQUAL 0)
            message(FATAL_ERROR
                "Unable to download the pinned Pillow wheel (status ${_orca_ai_pillow_download_code})")
        endif()
    endif()

    file(REMOVE_RECURSE "${ORCA_AI_PILLOW_STAGE_DIR}")
    file(MAKE_DIRECTORY "${ORCA_AI_PILLOW_STAGE_DIR}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xvf "${ORCA_AI_PILLOW_WHEEL}"
        WORKING_DIRECTORY "${ORCA_AI_PILLOW_STAGE_DIR}"
        RESULT_VARIABLE _orca_ai_pillow_extract_result
        OUTPUT_QUIET
        ERROR_QUIET)
    if(NOT _orca_ai_pillow_extract_result EQUAL 0)
        message(FATAL_ERROR "Unable to extract the pinned Pillow wheel")
    endif()
    file(GLOB _orca_ai_pillow_native_modules
        "${ORCA_AI_PILLOW_STAGE_DIR}/PIL/_imaging.*.pyd")
    if(NOT EXISTS "${ORCA_AI_PILLOW_STAGE_DIR}/PIL/__init__.py"
       OR NOT EXISTS "${ORCA_AI_PILLOW_STAGE_DIR}/pillow-${ORCA_AI_PILLOW_VERSION}.dist-info/METADATA"
       OR NOT _orca_ai_pillow_native_modules)
        message(FATAL_ERROR "The verified Pillow wheel did not contain the expected CPython runtime files")
    endif()

    set(ORCA_AI_RUNTIME_DEPENDENCIES_FILE "${CMAKE_BINARY_DIR}/orca_ai_runtime_dependencies.json")
    configure_file(
        "${CMAKE_SOURCE_DIR}/tools/ai/orca_ai_runtime_dependencies.json.in"
        "${ORCA_AI_RUNTIME_DEPENDENCIES_FILE}"
        @ONLY)
endif()
