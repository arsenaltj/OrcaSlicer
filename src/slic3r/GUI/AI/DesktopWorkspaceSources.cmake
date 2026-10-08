# The printer view and native adapters share the existing GUI target and PCH.
# RedesignShell now owns navigation; the legacy navigation is not assembled.
set(ORCA_DESKTOP_WORKSPACE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/../MainFrameWorkspace.ipp
    ${CMAKE_CURRENT_LIST_DIR}/../PlaterAIFeatureHosts.ipp
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
