# Keep product packaging policy outside the application's core target wiring.
if(WIN32)
    set_target_properties(OrcaSlicer PROPERTIES OUTPUT_NAME "${HONOR_PRODUCT_NAME}")
    set_target_properties(OrcaSlicer_app_gui PROPERTIES OUTPUT_NAME "${HONOR_PRODUCT_NAME}"
        PDB_NAME "${HONOR_PRODUCT_NAME} Launcher"
        ARCHIVE_OUTPUT_NAME "${HONOR_PRODUCT_NAME} Launcher")
    target_include_directories(OrcaSlicer_app_gui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/libslic3r")
    set(HONOR_RESOURCE_FILENAME "${HONOR_PRODUCT_DLL}")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/dev-utils/platform/msw/OrcaSlicer.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/HonorCore.rc @ONLY)
    target_sources(OrcaSlicer PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/HonorCore.rc)
elseif(APPLE)
    set_target_properties(OrcaSlicer PROPERTIES OUTPUT_NAME "${HONOR_PRODUCT_NAME}"
        MACOSX_BUNDLE_BUNDLE_NAME "${HONOR_PRODUCT_NAME}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${HONOR_PRODUCT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${HONOR_PRODUCT_VERSION}")
else()
    # Preserve the public CLI alias used by existing Flatpak/automation consumers.
    add_custom_command(TARGET OrcaSlicer POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E create_symlink "${HONOR_PRODUCT_COMMAND}" "$<TARGET_FILE_DIR:OrcaSlicer>/orca-slicer"
        VERBATIM)
    install(CODE "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink \"${HONOR_PRODUCT_COMMAND}\" \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/bin/orca-slicer\")")
endif()
