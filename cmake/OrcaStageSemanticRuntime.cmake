# Also usable as an offline CMake -P verifier. Binary hashes are repeated here
# deliberately so build/install inputs are checked independently of the JSON.
if(NOT IS_DIRECTORY "${SEMANTIC_RUNTIME_SOURCE}")
    message(FATAL_ERROR "Semantic runtime is missing: run scripts/prepare_semantic_runtime.ps1 first")
endif()
set(_orca_semantic_runtime_files
    libmediapipe.dll
    selfie_multiclass_256x256.tflite
    face_landmarker.task
    pose_landmarker_lite.task
    LICENSE
    NOTICE
    runtime-manifest.json
    MODEL_LICENSES.md
    providers.json)
set(_orca_semantic_runtime_hashes
    a8970c645c8c87c25ec9965cb5c898e803c6c42f7192b7de9a0541c62ae48cef
    c6748b1253a99067ef71f7e26ca71096cd449baefa8f101900ea23016507e0e0
    64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff
    59929e1d1ee95287735ddd833b19cf4ac46d29bc7afddbbf6753c459690d574a
    8707eef0533987efc5b155d64761eeb6e20793f50b9bd1a68dad1cf4719d0ed8
    d3b4a80a24a01fd445d4b70a610fd836ec3547c3a62eb835a1041956c38d9f56)
set(_semantic_index 0)
foreach(_semantic_file IN LISTS _orca_semantic_runtime_files)
    if(NOT EXISTS "${SEMANTIC_RUNTIME_SOURCE}/${_semantic_file}")
        message(FATAL_ERROR "Incomplete semantic runtime: ${_semantic_file}")
    endif()
    if(_semantic_index LESS 5)
        list(GET _orca_semantic_runtime_hashes ${_semantic_index} _semantic_expected)
        file(SHA256 "${SEMANTIC_RUNTIME_SOURCE}/${_semantic_file}" _semantic_actual)
        if(NOT _semantic_actual STREQUAL _semantic_expected)
            message(FATAL_ERROR "Semantic runtime checksum mismatch: ${_semantic_file}")
        endif()
    endif()
    math(EXPR _semantic_index "${_semantic_index} + 1")
endforeach()
if(EXISTS "${SEMANTIC_RUNTIME_SOURCE}/boundary/runtime-manifest.json")
    set(_boundary_files onnxruntime.dll mobile_sam_encoder.onnx mobile_sam_decoder.onnx)
    set(_boundary_hashes
        c7151fd9844ad7c7d18525f1177e9ef62d91e4a6ac3583d0be700554a2b2b1d6
        83398d336e1e95140df654a7be83ddb35b4c78221dd984f4f62d2a78d95ace32
        43c7655a81b62c0f2b0e31d2bc91d3811b48d7f736ecf93b3a4a1254318241cf)
    foreach(_i RANGE 0 2)
        list(GET _boundary_files ${_i} _file)
        list(GET _boundary_hashes ${_i} _expected)
        if(NOT EXISTS "${SEMANTIC_RUNTIME_SOURCE}/boundary/${_file}")
            message(FATAL_ERROR "Incomplete boundary runtime: ${_file}")
        endif()
        file(SHA256 "${SEMANTIC_RUNTIME_SOURCE}/boundary/${_file}" _actual)
        if(NOT _actual STREQUAL _expected)
            message(FATAL_ERROR "Boundary runtime checksum mismatch: ${_file}")
        endif()
    endforeach()
    list(APPEND _boundary_files MobileSAM-LICENSE ONNXRuntime-LICENSE ONNXRuntime-ThirdPartyNotices.txt runtime-manifest.json)
    foreach(_file IN LISTS _boundary_files)
        if(NOT EXISTS "${SEMANTIC_RUNTIME_SOURCE}/boundary/${_file}")
            message(FATAL_ERROR "Incomplete boundary notices: ${_file}")
        endif()
        list(APPEND _orca_semantic_runtime_files "boundary/${_file}")
    endforeach()
endif()
if(SEMANTIC_RUNTIME_DESTINATION)
    file(MAKE_DIRECTORY "${SEMANTIC_RUNTIME_DESTINATION}")
    foreach(_semantic_file IN LISTS _orca_semantic_runtime_files)
        get_filename_component(_destination_parent "${SEMANTIC_RUNTIME_DESTINATION}/${_semantic_file}" DIRECTORY)
        file(MAKE_DIRECTORY "${_destination_parent}")
        configure_file("${SEMANTIC_RUNTIME_SOURCE}/${_semantic_file}"
            "${SEMANTIC_RUNTIME_DESTINATION}/${_semantic_file}" COPYONLY)
    endforeach()
endif()
