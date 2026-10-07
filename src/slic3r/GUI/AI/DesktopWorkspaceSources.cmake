# The desktop navigation and printer view share the existing GUI target, flags,
# encoding checks and PCH. Keep their assembly out of the main source list.
set(ORCA_DESKTOP_WORKSPACE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/../MainFrameWorkspace.ipp
    ${CMAKE_CURRENT_LIST_DIR}/../PlaterAIFeatureHosts.ipp
    ${CMAKE_CURRENT_LIST_DIR}/DesktopWorkspaceNavigation.cpp
    ${CMAKE_CURRENT_LIST_DIR}/DesktopWorkspaceNavigation.hpp
    ${CMAKE_CURRENT_LIST_DIR}/Orca/OrcaPrintConfirmation.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Orca/OrcaPrintConfirmation.hpp
    ${CMAKE_CURRENT_LIST_DIR}/Orca/OrcaFilamentSelection.cpp
    ${CMAKE_CURRENT_LIST_DIR}/Orca/OrcaFilamentSelection.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../Redesign/PrinterWorkspace.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../Redesign/PrinterWorkspace.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../Redesign/PrinterWorkspaceState.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../Redesign/OrcaPrinterAdapter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../Redesign/OrcaPrinterAdapter.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../Redesign/RedesignMessageDialog.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../Redesign/RedesignMessageDialog.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../Redesign/RedesignWidgets.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../Redesign/RedesignTheme.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../Redesign/RedesignControls.hpp
    ${CMAKE_CURRENT_LIST_DIR}/../Redesign/RedesignFlowGuide.hpp
)
