include_guard(GLOBAL)

# Local, display-only segmentation. Keep native headers/defines on its single
# source file so enabling it does not alter the rest of the GUI's ABI or PCH.
set(_foreground_default OFF)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
   NOT CMAKE_GENERATOR_PLATFORM MATCHES "[Aa][Rr][Mm]|aarch64" AND
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]|aarch64")
    set(_foreground_default ON)
endif()
option(ORCA_GENERATION_FOREGROUND_NATIVE "Enable local CPU foreground masks for generation particles" ${_foreground_default})
set(ORCA_GENERATION_FOREGROUND_CACHE "${CMAKE_BINARY_DIR}/_deps/generation_foreground" CACHE PATH "Cache for the pinned foreground SDK and model")
set_property(GLOBAL PROPERTY ORCA_FOREGROUND_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(_orca_foreground_download name url hash)
    set(_file "${ORCA_GENERATION_FOREGROUND_CACHE}/${name}")
    set(_candidate "${_file}")
    set(_code "0")
    if(NOT EXISTS "${_file}")
        set(_candidate "${_file}.download")
        set(_windows_curl "$ENV{SystemRoot}/System32/curl.exe")
        if(CMAKE_HOST_WIN32 AND EXISTS "${_windows_curl}")
            # Start each retry from the bytes already written, using Windows certificate validation.
            foreach(_attempt RANGE 1 3)
                execute_process(
                    COMMAND "${_windows_curl}" --disable --proto "=https" --proto-redir "=https"
                        --fail --location --silent --show-error --connect-timeout 20
                        --max-time 180 --speed-limit 1 --speed-time 45 --continue-at -
                        --output "${_candidate}" "${url}"
                    RESULT_VARIABLE _code ERROR_VARIABLE _detail)
                if(_code STREQUAL "0")
                    break()
                endif()
                if(_code STREQUAL "33" OR _code STREQUAL "36")
                    # Discard the partial only if the server cannot resume it.
                    file(REMOVE "${_candidate}")
                endif()
            endforeach()
        else()
            file(DOWNLOAD "${url}" "${_candidate}"
                TLS_VERIFY ON STATUS _status TIMEOUT 600 INACTIVITY_TIMEOUT 60)
            list(GET _status 0 _code)
            list(GET _status 1 _detail)
        endif()
    endif()

    set(_actual "")
    if(EXISTS "${_candidate}")
        file(SHA256 "${_candidate}" _actual)
    endif()
    if(NOT _actual STREQUAL hash)
        if(NOT _code STREQUAL "0")
            message(FATAL_ERROR "Cannot download foreground resource ${name} (${_code}): ${_detail}")
        endif()
        message(FATAL_ERROR "Foreground resource integrity mismatch: ${_candidate}; expected ${hash}, got ${_actual}")
    endif()
    if(NOT _candidate STREQUAL _file)
        file(RENAME "${_candidate}" "${_file}")
    endif()
endfunction()

function(orca_configure_generation_foreground)
    if(NOT ORCA_GENERATION_FOREGROUND_NATIVE)
        return()
    endif()
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
       CMAKE_GENERATOR_PLATFORM MATCHES "[Aa][Rr][Mm]|aarch64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]|aarch64")
        message(FATAL_ERROR "ORCA_GENERATION_FOREGROUND_NATIVE supports Windows x64 only; disable it on other targets")
    endif()
    file(MAKE_DIRECTORY "${ORCA_GENERATION_FOREGROUND_CACHE}")
    # Keep existing verified archives usable without a second download.
    set(_legacy_archive "onnxruntime-win-x64-1.20.1.zip")
    if(EXISTS "${ORCA_GENERATION_FOREGROUND_CACHE}/${_legacy_archive}")
        _orca_foreground_download("${_legacy_archive}"
            "https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/${_legacy_archive}"
            "78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581")
        file(ARCHIVE_EXTRACT INPUT "${ORCA_GENERATION_FOREGROUND_CACHE}/${_legacy_archive}"
            DESTINATION "${ORCA_GENERATION_FOREGROUND_CACHE}")
        set(_sdk "${ORCA_GENERATION_FOREGROUND_CACHE}/onnxruntime-win-x64-1.20.1")
        set(_sdk_include "${_sdk}/include")
        set(_sdk_dll "${_sdk}/lib/onnxruntime.dll")
    else()
        # The Microsoft NuGet package contains the same pinned CPU DLL.
        set(_sdk_package "microsoft.ml.onnxruntime.1.20.1.nupkg")
        _orca_foreground_download("${_sdk_package}"
            "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/1.20.1/${_sdk_package}"
            "9c32108e9a1c78b090a95c2a219f244dd5f067d4caf68fb0f967103f3f20839c")
        set(_sdk "${ORCA_GENERATION_FOREGROUND_CACHE}/onnxruntime-nuget-1.20.1")
        file(ARCHIVE_EXTRACT INPUT "${ORCA_GENERATION_FOREGROUND_CACHE}/${_sdk_package}"
            DESTINATION "${_sdk}"
            PATTERNS "build/native/include/*" "runtimes/win-x64/native/onnxruntime.dll"
                     "LICENSE" "ThirdPartyNotices.txt")
        set(_sdk_include "${_sdk}/build/native/include")
        set(_sdk_dll "${_sdk}/runtimes/win-x64/native/onnxruntime.dll")
    endif()
    _orca_foreground_download(u2netp.onnx
        "https://github.com/danielgatis/rembg/releases/download/v0.0.0/u2netp.onnx"
        "309c8469258dda742793dce0ebea8e6dd393174f89934733ecc8b14c76f4ddd8")
    set(_runtime "${ORCA_GENERATION_FOREGROUND_CACHE}/runtime")
    file(MAKE_DIRECTORY "${_runtime}")
    file(COPY "${_sdk_dll}" "${ORCA_GENERATION_FOREGROUND_CACHE}/u2netp.onnx" DESTINATION "${_runtime}")
    configure_file("${_sdk}/LICENSE" "${_runtime}/onnxruntime-LICENSE.txt" COPYONLY)
    configure_file("${_sdk}/ThirdPartyNotices.txt" "${_runtime}/onnxruntime-ThirdPartyNotices.txt" COPYONLY)
    get_property(_module GLOBAL PROPERTY ORCA_FOREGROUND_CMAKE_DIR)
    configure_file("${_module}/licenses/U2Net-LICENSE.txt" "${_runtime}/U2Net-LICENSE.txt" COPYONLY)
    file(SHA256 "${_runtime}/onnxruntime.dll" _runtime_hash)
    file(WRITE "${_runtime}/manifest.json"
        "{\n  \"schema\": \"generation-foreground-v1\",\n  \"runtime_version\": \"1.20.1\",\n  \"runtime_sha256\": \"${_runtime_hash}\",\n  \"model\": \"U2NetP\",\n  \"model_sha256\": \"309c8469258dda742793dce0ebea8e6dd393174f89934733ecc8b14c76f4ddd8\"\n}\n")
    set_property(SOURCE GUI/Redesign/GenerationForeground.cpp APPEND PROPERTY
        INCLUDE_DIRECTORIES "${_sdk_include}")
    set_property(SOURCE GUI/Redesign/GenerationForeground.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS ORCA_GENERATION_FOREGROUND_NATIVE=1 "ORCA_FOREGROUND_DLL_SHA256=\"${_runtime_hash}\"")
    set_property(GLOBAL PROPERTY ORCA_FOREGROUND_RUNTIME "${_runtime}")
endfunction()

function(orca_stage_generation_foreground target)
    get_property(_runtime GLOBAL PROPERTY ORCA_FOREGROUND_RUNTIME)
    if(NOT _runtime)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_runtime}" "$<TARGET_FILE_DIR:${target}>/ai/generation_foreground"
        COMMENT "Staging the pinned local foreground runtime" VERBATIM)
    install(DIRECTORY "${_runtime}/" DESTINATION "ai/generation_foreground")
endfunction()
