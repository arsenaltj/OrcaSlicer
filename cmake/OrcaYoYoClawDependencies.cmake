# YoYoClaw's authenticated client uses a small, pinned Python HTTP/crypto
# stack. Keep it inside the isolated Windows runtime so importing the optional
# provider cannot break the existing GPT/Tripo sidecar when it is unconfigured.

set(ORCA_AI_YOYOCLAW_REQUESTS_VERSION "2.32.3")
set(ORCA_AI_YOYOCLAW_CRYPTOGRAPHY_VERSION "46.0.3")
set(ORCA_AI_YOYOCLAW_CFFI_VERSION "2.1.1")
set(ORCA_AI_YOYOCLAW_CHARSET_VERSION "3.5.2")
set(ORCA_AI_YOYOCLAW_IDNA_VERSION "3.20")
set(ORCA_AI_YOYOCLAW_URLLIB3_VERSION "2.8.0")
set(ORCA_AI_YOYOCLAW_CERTIFI_VERSION "2026.10.19")
set(ORCA_AI_YOYOCLAW_PYCPARSER_VERSION "3.12")

string(TOLOWER "${CMAKE_GENERATOR_PLATFORM};${CMAKE_SYSTEM_PROCESSOR}" _orca_yoyoclaw_arch)
if(_orca_yoyoclaw_arch MATCHES "arm64|aarch64")
    set(ORCA_AI_YOYOCLAW_ARCHITECTURE "arm64")
    set(_orca_yoyoclaw_crypto_filename "cryptography-46.0.3-cp311-abi3-win_arm64.whl")
    set(_orca_yoyoclaw_crypto_url
        "https://files.pythonhosted.org/packages/2e/af/9b302da4c87b0beb9db4e756386a7c6c5b8003cd0e742277888d352ae91d/cryptography-46.0.3-cp311-abi3-win_arm64.whl")
    set(_orca_yoyoclaw_crypto_sha256 "5d7f93296ee28f68447397bf5198428c9aeeab45705a55d53a6343455dcb2c3c")
    set(_orca_yoyoclaw_cffi_filename "cffi-2.1.1-cp312-cp312-win_arm64.whl")
    set(_orca_yoyoclaw_cffi_url
        "https://files.pythonhosted.org/packages/70/c6/d0ea84713fe46b243a436a18fcd47d639732747e21635c8a27191b06dc30/cffi-2.1.1-cp312-cp312-win_arm64.whl")
    set(_orca_yoyoclaw_cffi_sha256 "7bde5e4cc5c10140859842b9d383af292b22639a4dffb725314baf45968cef80")
    set(_orca_yoyoclaw_charset_filename "charset_normalizer-3.5.2-cp312-cp312-win_arm64.whl")
    set(_orca_yoyoclaw_charset_url
        "https://files.pythonhosted.org/packages/c4/e3/017aea0911ada7405a825c7d937eb3a13009664e2f5b38e8c4bbf2abf894/charset_normalizer-3.5.2-cp312-cp312-win_arm64.whl")
    set(_orca_yoyoclaw_charset_sha256 "e2af3aad578aa6bd1384bcf4750fc285e5a9de53f40b7d41e5a0bf748edeb2b3")
else()
    set(ORCA_AI_YOYOCLAW_ARCHITECTURE "x64")
    set(_orca_yoyoclaw_crypto_filename "cryptography-46.0.3-cp311-abi3-win_amd64.whl")
    set(_orca_yoyoclaw_crypto_url
        "https://files.pythonhosted.org/packages/7e/bf/80fbf45253ea585a1e492a6a17efcb93467701fa79e71550a430c5e60df0/cryptography-46.0.3-cp311-abi3-win_amd64.whl")
    set(_orca_yoyoclaw_crypto_sha256 "a9a3008438615669153eb86b26b61e09993921ebdd75385ddd748702c5adfddb")
    set(_orca_yoyoclaw_cffi_filename "cffi-2.1.1-cp312-cp312-win_amd64.whl")
    set(_orca_yoyoclaw_cffi_url
        "https://files.pythonhosted.org/packages/d9/79/615cc094e2fb508cade7de88d3b4f6c4ec2bab695c97bce9153dc65aadf5/cffi-2.1.1-cp312-cp312-win_amd64.whl")
    set(_orca_yoyoclaw_cffi_sha256 "f53e442b08449d42821fa4a4fba000095af9f62742a500f978a9f557ec44339a")
    set(_orca_yoyoclaw_charset_filename "charset_normalizer-3.5.2-cp312-cp312-win_amd64.whl")
    set(_orca_yoyoclaw_charset_url
        "https://files.pythonhosted.org/packages/eb/e6/e6e083884cbcfd49c64865af05027fe7011be7b2d9179524f099a1b611f3/charset_normalizer-3.5.2-cp312-cp312-win_amd64.whl")
    set(_orca_yoyoclaw_charset_sha256 "780fbe7cab297b81dad9fb8dc5eb003c0468ffb0d9e5f65068c53a34661a96bc")
endif()

set(_orca_yoyoclaw_specs
    "requests-2.32.3-py3-none-any.whl|https://files.pythonhosted.org/packages/f9/9b/335f9764261e915ed497fcdeb11df5dfd6f7bf257d4a6a2a686d80da4d54/requests-2.32.3-py3-none-any.whl|70761cfe03c773ceb22aa2f671b4757976145175cdfca038c02654d061d6dcc6"
    "${_orca_yoyoclaw_crypto_filename}|${_orca_yoyoclaw_crypto_url}|${_orca_yoyoclaw_crypto_sha256}"
    "${_orca_yoyoclaw_charset_filename}|${_orca_yoyoclaw_charset_url}|${_orca_yoyoclaw_charset_sha256}"
    "idna-3.20-py3-none-any.whl|https://files.pythonhosted.org/packages/58/a2/bb081bab032533a855d44de1d56f8e8426114ff1ba5d1f07a438a0a654f8/idna-3.20-py3-none-any.whl|ab7ae7122974553370f0bdb919e1a960b2cd1bc1ef0276416d896db81c14582c"
    "urllib3-2.8.0-py3-none-any.whl|https://files.pythonhosted.org/packages/92/9d/c4e665119135114480843e7ab388fa94d8480650450e6f8e26b70d323a4c/urllib3-2.8.0-py3-none-any.whl|0cf3cae568d36aa9576b28dfb35f11328f1cb974ca7647d9475ebb86c75ac6e3"
    "certifi-2026.10.19-py3-none-any.whl|https://files.pythonhosted.org/packages/c4/51/29046e48f693c54e5b7412216d447a1d8362c9653148d98c02cdf65fa733/certifi-2026.10.19-py3-none-any.whl|42d35340cc5b9ba78e818ddbff0c5b1c2b779de448a340fd1b989f5fa8670e1b"
    "${_orca_yoyoclaw_cffi_filename}|${_orca_yoyoclaw_cffi_url}|${_orca_yoyoclaw_cffi_sha256}"
    "pycparser-3.12-py3-none-any.whl|https://files.pythonhosted.org/packages/b6/ae/315bbeee126a769cbde43cf4908acb4b40408d8bf8bbf334dd7368d8dadf/pycparser-3.12-py3-none-any.whl|a076f3f489b39b43ed13563a1dafe42447445bec2e294154ec5c57fcdda4c85e"
)

set(ORCA_AI_YOYOCLAW_WHEEL_CACHE_DIR "${CMAKE_BINARY_DIR}/_deps/orca_ai_yoyoclaw_wheels")
set(ORCA_AI_YOYOCLAW_STAGE_DIR "${CMAKE_BINARY_DIR}/_deps/orca_ai_python_packages/yoyoclaw")
file(MAKE_DIRECTORY "${ORCA_AI_YOYOCLAW_WHEEL_CACHE_DIR}")
file(REMOVE_RECURSE "${ORCA_AI_YOYOCLAW_STAGE_DIR}")
file(MAKE_DIRECTORY "${ORCA_AI_YOYOCLAW_STAGE_DIR}")

foreach(_orca_yoyoclaw_spec IN LISTS _orca_yoyoclaw_specs)
    string(REPLACE "|" ";" _orca_yoyoclaw_fields "${_orca_yoyoclaw_spec}")
    list(GET _orca_yoyoclaw_fields 0 _orca_yoyoclaw_filename)
    list(GET _orca_yoyoclaw_fields 1 _orca_yoyoclaw_url)
    list(GET _orca_yoyoclaw_fields 2 _orca_yoyoclaw_sha256)
    set(_orca_yoyoclaw_wheel "${ORCA_AI_YOYOCLAW_WHEEL_CACHE_DIR}/${_orca_yoyoclaw_filename}")
    if(EXISTS "${_orca_yoyoclaw_wheel}")
        file(SHA256 "${_orca_yoyoclaw_wheel}" _orca_yoyoclaw_cached_sha256)
        if(NOT "${_orca_yoyoclaw_cached_sha256}" STREQUAL "${_orca_yoyoclaw_sha256}")
            file(REMOVE "${_orca_yoyoclaw_wheel}")
        endif()
    endif()
    if(NOT EXISTS "${_orca_yoyoclaw_wheel}")
        message(STATUS "Downloading pinned YoYoClaw runtime wheel ${_orca_yoyoclaw_filename}")
        file(DOWNLOAD
            "${_orca_yoyoclaw_url}"
            "${_orca_yoyoclaw_wheel}"
            EXPECTED_HASH "SHA256=${_orca_yoyoclaw_sha256}"
            STATUS _orca_yoyoclaw_download_status
            TLS_VERIFY ON
            TIMEOUT 120
            INACTIVITY_TIMEOUT 30)
        list(GET _orca_yoyoclaw_download_status 0 _orca_yoyoclaw_download_code)
        if(NOT _orca_yoyoclaw_download_code EQUAL 0)
            message(FATAL_ERROR "Unable to download ${_orca_yoyoclaw_filename} (status ${_orca_yoyoclaw_download_status})")
        endif()
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xvf "${_orca_yoyoclaw_wheel}"
        WORKING_DIRECTORY "${ORCA_AI_YOYOCLAW_STAGE_DIR}"
        RESULT_VARIABLE _orca_yoyoclaw_extract_result
        OUTPUT_QUIET
        ERROR_QUIET)
    if(NOT _orca_yoyoclaw_extract_result EQUAL 0)
        message(FATAL_ERROR "Unable to extract ${_orca_yoyoclaw_filename}")
    endif()
endforeach()

if(NOT EXISTS "${ORCA_AI_YOYOCLAW_STAGE_DIR}/requests/__init__.py"
   OR NOT EXISTS "${ORCA_AI_YOYOCLAW_STAGE_DIR}/cryptography/__init__.py"
   OR NOT EXISTS "${ORCA_AI_YOYOCLAW_STAGE_DIR}/cffi/__init__.py"
   OR NOT EXISTS "${ORCA_AI_YOYOCLAW_STAGE_DIR}/certifi/__init__.py")
    message(FATAL_ERROR "The verified YoYoClaw wheels did not contain the expected Python packages")
endif()
