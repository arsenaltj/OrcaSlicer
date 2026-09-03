set(_orca_ai_installed_python "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/python/python.exe")
set(_orca_ai_installed_bootstrap
    "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/resources/tools/ai/orca_ai_installed_bootstrap.py")
if(NOT EXISTS "${_orca_ai_installed_python}" OR NOT EXISTS "${_orca_ai_installed_bootstrap}")
    message(FATAL_ERROR "Installed AI Sidecar verification inputs are missing")
endif()
execute_process(
    COMMAND "${_orca_ai_installed_python}" -I "${_orca_ai_installed_bootstrap}" --verify-install
    RESULT_VARIABLE _orca_ai_installed_config_result
    OUTPUT_VARIABLE _orca_ai_installed_config_output
    ERROR_VARIABLE _orca_ai_installed_config_error)
if(NOT _orca_ai_installed_config_result EQUAL 0)
    message(FATAL_ERROR "Installed AI Sidecar configuration check failed: ${_orca_ai_installed_config_error}")
endif()
string(STRIP "${_orca_ai_installed_config_output}" _orca_ai_installed_config_output)
message(STATUS "${_orca_ai_installed_config_output}")
