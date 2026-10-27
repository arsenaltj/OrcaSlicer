# Also usable as an offline CMake -P verifier. Binary hashes are repeated here
# deliberately so build/install inputs are checked independently of the JSON.
if(NOT IS_DIRECTORY "${SEMANTIC_RUNTIME_SOURCE}")
    message(FATAL_ERROR "Semantic runtime is missing: run scripts/prepare_semantic_runtime.ps1 first")
endif()
set(_orca_semantic_runtime_files
    libmediapipe.dll
    selfie_multiclass_256x256.tflite
    face_landmarker.task
    LICENSE
    NOTICE
    runtime-manifest.json
    MODEL_LICENSES.md
    providers.json)
set(_orca_semantic_runtime_hashes
    a8970c645c8c87c25ec9965cb5c898e803c6c42f7192b7de9a0541c62ae48cef
    c6748b1253a99067ef71f7e26ca71096cd449baefa8f101900ea23016507e0e0
    64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff
    8707eef0533987efc5b155d64761eeb6e20793f50b9bd1a68dad1cf4719d0ed8
    d3b4a80a24a01fd445d4b70a610fd836ec3547c3a62eb835a1041956c38d9f56
    e3a3cf149163a263f5ac0e10a045a6a474096263170b65eba2d01f547728f588
    da2614c3402f4f88478800032e61bb5a8004a5ce75073412f2043902d7e2bb28
    07c9b971c5bda8db4007d2396cdb9fb221c6328220f89d77159468b002dc9298)
set(_orca_semantic_runtime_sizes
    43301376
    16371837
    3758596
    12331
    1540836
    2276
    2053
    85)
set(_semantic_index 0)
foreach(_semantic_file IN LISTS _orca_semantic_runtime_files)
    if(NOT EXISTS "${SEMANTIC_RUNTIME_SOURCE}/${_semantic_file}")
        message(FATAL_ERROR "Incomplete semantic runtime: ${_semantic_file}")
    endif()
    list(GET _orca_semantic_runtime_sizes ${_semantic_index} _semantic_expected_size)
    file(SIZE "${SEMANTIC_RUNTIME_SOURCE}/${_semantic_file}" _semantic_actual_size)
    if(NOT _semantic_actual_size EQUAL _semantic_expected_size)
        message(FATAL_ERROR "Semantic runtime size mismatch: ${_semantic_file}")
    endif()
    list(GET _orca_semantic_runtime_hashes ${_semantic_index} _semantic_expected)
    file(SHA256 "${SEMANTIC_RUNTIME_SOURCE}/${_semantic_file}" _semantic_actual)
    if(NOT _semantic_actual STREQUAL _semantic_expected)
        message(FATAL_ERROR "Semantic runtime checksum mismatch: ${_semantic_file}")
    endif()
    math(EXPR _semantic_index "${_semantic_index} + 1")
endforeach()
if(SEMANTIC_RUNTIME_DESTINATION)
    file(MAKE_DIRECTORY "${SEMANTIC_RUNTIME_DESTINATION}")
    foreach(_semantic_file IN LISTS _orca_semantic_runtime_files)
        configure_file("${SEMANTIC_RUNTIME_SOURCE}/${_semantic_file}"
            "${SEMANTIC_RUNTIME_DESTINATION}/${_semantic_file}" COPYONLY)
    endforeach()
endif()
