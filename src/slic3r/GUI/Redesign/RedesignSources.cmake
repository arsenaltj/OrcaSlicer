# Assemble the redesigned workflows on the existing native GUI target.
include(${CMAKE_CURRENT_LIST_DIR}/../../Utils/HonorAccount/HonorAccountSources.cmake)
set(ORCA_REDESIGN_WORKFLOW_SOURCES
    ${ORCA_HONOR_ACCOUNT_SOURCES}
    GUI/AI/ModelGeneration/ModelGenerationAssetsView.cpp
    GUI/AI/SmartSlicing/SmartSlicingPanel.cpp
    GUI/AI/SmartSlicing/SmartSlicingPanel.hpp
    GUI/AI/SmartSlicing/SmartSlicingPresenter.cpp
    GUI/AI/SmartSlicing/SmartSlicingPresenter.hpp
    GUI/AI/SmartSlicing/SmartSlicingViewModel.cpp
    GUI/AI/SmartSlicing/SmartSlicingViewModel.hpp
    GUI/AI/Model/VertexColorRegionEditor.cpp GUI/AI/Model/ModelFinishing.cpp GUI/AI/ModelGeneration/ModelGenerationBeautyView.cpp GUI/AI/Model/BeautySurface.cpp GUI/AI/Model/BeautyAppearance.cpp GUI/AI/Model/BeautyFinishing.cpp GUI/AI/ModelGeneration/BeautyWorkbenchControls.cpp GUI/AI/ModelGeneration/BeautyGuidanceProtectedRegions.cpp GUI/AI/ModelGeneration/BeautyGuidanceProtectedRegions.hpp
    GUI/AI/ModelGeneration/PostGenerationWorkbenchView.cpp
    GUI/AI/ModelGeneration/PostGenerationWorkbenchPresentation.cpp
    GUI/AI/ModelGeneration/PostGenerationWorkbenchHost.cpp
    GUI/AI/ModelGeneration/ModelGenerationPortrait.cpp
    GUI/AI/ModelGeneration/ModelGenerationPortraitDraft.cpp
    GUI/AI/ModelGeneration/PostGenerationWorkbenchPalette.cpp
    GUI/AI/ModelGeneration/PostGenerationWorkbenchCheck.cpp
    GUI/AI/ModelGeneration/ModelGenerationBeautySession.cpp
    GUI/AI/ModelGeneration/ModelGenerationBeautyPreview.cpp
    GUI/AI/ModelGeneration/ModelGenerationBeautyExport.cpp
    GUI/AI/ModelGeneration/ModelGenerationBeautyVersions.cpp
    GUI/AI/ModelGeneration/ModelGenerationWorkbenchHistory.cpp
    GUI/AI/ModelGeneration/ModelPreviewSemantics.cpp
    GUI/AI/ModelGeneration/ModelSemanticColoring.cpp
    GUI/AI/ModelGeneration/PortraitColorBlockPostprocess.cpp
    GUI/AI/ColorMatching/LocalPrintColorPanel.cpp GUI/AI/ModelGeneration/LocalSemanticWorkerClient.cpp GUI/AI/ModelGeneration/ModelGenerationPreviewLayout.cpp GUI/AI/ModelGeneration/ModelGenerationSubmission.cpp
    GUI/AI/ModelGeneration/PortraitResidualProposalClient.cpp
    GUI/AI/ModelGeneration/PortraitParentCleanup.hpp
    GUI/AI/Model/VertexColorRegionEditor.hpp GUI/AI/Model/ModelArtifact.cpp GUI/AI/Model/ModelArtifact.hpp GUI/AI/Model/GlbGeometryEditing.cpp GUI/AI/ModelGeneration/ModelGenerationLocalImport.cpp
    GUI/Redesign/RedesignCommand.cpp
    GUI/Redesign/RedesignCommand.hpp
    GUI/Redesign/RedesignFeatureFlags.cpp
    GUI/Redesign/RedesignFeatureFlags.hpp
    GUI/Redesign/OrcaBusinessAdapter.cpp
    GUI/Redesign/OrcaBusinessAdapter.hpp
    GUI/Redesign/RedesignMessageDialog.cpp
    GUI/Redesign/RedesignMessageDialog.hpp
    GUI/Redesign/ImageHistorySidebar.cpp
    GUI/Redesign/ImageHistorySidebar.hpp
    GUI/Redesign/ImageHistoryPagination.hpp
    GUI/Redesign/AssetsWorkspace.cpp
    GUI/Redesign/AssetsWorkspace.hpp
    GUI/Redesign/AssetsWorkspacePolicy.hpp
    GUI/Redesign/AccountWorkspace.cpp
    GUI/Redesign/AccountWorkspace.hpp
    GUI/Redesign/AccountAvatar.hpp
    GUI/Redesign/HonorBrowserProfile.cpp
    GUI/Redesign/HonorBrowserProfile.hpp
    GUI/Redesign/HonorAccountWebTheme.hpp
    GUI/Redesign/RedesignModelRoute.hpp
    GUI/Redesign/RedesignShell.cpp
    GUI/Redesign/RedesignModelWorkflow.cpp
    GUI/Redesign/RedesignShell.hpp
    GUI/Redesign/RedesignState.cpp
    GUI/Redesign/RedesignState.hpp
    GUI/Redesign/RedesignTheme.hpp
    GUI/Redesign/StartupSetupService.cpp
    GUI/Redesign/StartupSetupService.hpp
    GUI/Redesign/StartupSetupDialog.cpp
    GUI/Redesign/StartupSetupDialog.hpp
    GUI/Redesign/StartupSplashView.hpp
    GUI/Redesign/StartupBufferView.hpp
    GUI/FilamentColourPalette.hpp
    GUI/Redesign/RedesignWidgets.hpp
    GUI/Redesign/PrinterWorkspaceState.hpp
    GUI/Redesign/OrcaPrinterAdapter.cpp
    GUI/Redesign/OrcaPrinterAdapter.hpp
    GUI/Redesign/PrinterWorkspace.cpp
    GUI/Redesign/PrinterWorkspace.hpp
)

# ModelGenerationPanel is implemented across multiple translation units. Keep
# their object files on one class layout when its shared inline headers change;
# stale MSVC dependency state must not link old and new member offsets together.
set(ORCA_MODEL_GENERATION_PANEL_IMPLEMENTATIONS
    GUI/ModelGenerationPanel.cpp
    GUI/AI/ModelGeneration/ModelGenerationArtifactFlow.cpp
    GUI/AI/ModelGeneration/ModelGenerationAssetsView.cpp
    GUI/AI/ModelGeneration/ModelGenerationBeautyExport.cpp
    GUI/AI/ModelGeneration/ModelGenerationBeautyPreview.cpp
    GUI/AI/ModelGeneration/ModelGenerationBeautySession.cpp
    GUI/AI/ModelGeneration/ModelGenerationBeautyVersions.cpp
    GUI/AI/ModelGeneration/ModelGenerationBeautyView.cpp
    GUI/AI/ModelGeneration/ModelGenerationFeatureHost.cpp
    GUI/AI/ModelGeneration/ModelGenerationLibraryView.cpp
    GUI/AI/ModelGeneration/ModelGenerationLocalImport.cpp
    GUI/AI/ModelGeneration/ModelGenerationPortrait.cpp
    GUI/AI/ModelGeneration/ModelGenerationPortraitDraft.cpp
    GUI/AI/ModelGeneration/ModelGenerationPreviewLayout.cpp
    GUI/AI/ModelGeneration/ModelGenerationSubmission.cpp
    GUI/AI/ModelGeneration/ModelGenerationWorkbenchHistory.cpp
    GUI/AI/ModelGeneration/PostGenerationWorkbenchCheck.cpp
    GUI/AI/ModelGeneration/PostGenerationWorkbenchHost.cpp
    GUI/AI/ModelGeneration/PostGenerationWorkbenchPalette.cpp
    GUI/AI/ModelGeneration/PostGenerationWorkbenchPresentation.cpp
    GUI/AI/ModelGeneration/PostGenerationWorkbenchView.cpp
)

# The legacy PCH helper replaces OBJECT_DEPENDS for MSVC sources, so a direct
# source dependency is lost before the Visual Studio project is generated.
# Hash the layout-sensitive headers into a per-source compile definition. A
# header edit then changes the project command line and rebuilds every split
# implementation even when MSVC's incremental include state is stale.
file(GLOB ORCA_MODEL_GENERATION_LAYOUT_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/GUI/AI/ModelGeneration/*.hpp")
list(APPEND ORCA_MODEL_GENERATION_LAYOUT_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/GUI/ModelGenerationPanel.hpp")
list(SORT ORCA_MODEL_GENERATION_LAYOUT_HEADERS)
set(ORCA_MODEL_GENERATION_LAYOUT_MATERIAL "")
foreach(ORCA_MODEL_GENERATION_LAYOUT_HEADER IN LISTS ORCA_MODEL_GENERATION_LAYOUT_HEADERS)
    file(SHA256 "${ORCA_MODEL_GENERATION_LAYOUT_HEADER}" ORCA_MODEL_GENERATION_LAYOUT_HEADER_HASH)
    string(APPEND ORCA_MODEL_GENERATION_LAYOUT_MATERIAL "${ORCA_MODEL_GENERATION_LAYOUT_HEADER_HASH}")
endforeach()
string(SHA256 ORCA_MODEL_GENERATION_LAYOUT_HASH "${ORCA_MODEL_GENERATION_LAYOUT_MATERIAL}")
string(SUBSTRING "${ORCA_MODEL_GENERATION_LAYOUT_HASH}" 0 16 ORCA_MODEL_GENERATION_LAYOUT_REVISION)
set_property(SOURCE ${ORCA_MODEL_GENERATION_PANEL_IMPLEMENTATIONS} APPEND PROPERTY
    COMPILE_DEFINITIONS "ORCA_MODEL_GENERATION_LAYOUT_${ORCA_MODEL_GENERATION_LAYOUT_REVISION}")
