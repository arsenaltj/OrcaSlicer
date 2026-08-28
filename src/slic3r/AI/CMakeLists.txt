set(ORCA_AI_SMART_SLICING_SOURCES
    SmartSlicing/Application/ApplyWorkflow.hpp
    SmartSlicing/Application/CandidatePlanningWorkflow.hpp
    SmartSlicing/Application/PrintabilityInspector.cpp
    SmartSlicing/Application/PrintabilityInspector.hpp
    SmartSlicing/Application/SmartSlicingCoordinator.cpp
    SmartSlicing/Application/SmartSlicingCoordinator.hpp
    SmartSlicing/Application/TrialSlicingWorkflow.hpp
    SmartSlicing/Domain/CandidateComparison.cpp
    SmartSlicing/Domain/CandidateComparison.hpp
    SmartSlicing/Domain/ParameterProposal.hpp
    SmartSlicing/Domain/ParameterProposalValidator.cpp
    SmartSlicing/Domain/ParameterProposalValidator.hpp
    SmartSlicing/Domain/PlacementCandidate.hpp
    SmartSlicing/Domain/PrintabilityReport.hpp
    SmartSlicing/Domain/RepairPlan.hpp
    SmartSlicing/Domain/SliceCandidate.hpp
    SmartSlicing/Domain/SlicingMetrics.hpp
    SmartSlicing/Domain/SmartSlicingTypes.hpp
    SmartSlicing/Domain/WorkflowResourceBudget.hpp
    SmartSlicing/Domain/WorkflowState.hpp
    SmartSlicing/Domain/WorkspaceContext.hpp
    SmartSlicing/Domain/WorkspaceRevision.hpp
    SmartSlicing/Ports/IOfficialSliceGateway.hpp
    SmartSlicing/Ports/IOrcaWorkspace.hpp
    SmartSlicing/Ports/IParameterAdvisor.hpp
    SmartSlicing/Ports/ITrialSliceExecutor.hpp
    SmartSlicing/Ports/IWorkflowRuntimeStore.hpp
)

add_library(orcaslicer_ai_contracts INTERFACE)
add_library(OrcaSlicer::AIContracts ALIAS orcaslicer_ai_contracts)
target_include_directories(orcaslicer_ai_contracts INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(orcaslicer_ai_contracts INTERFACE boost_headeronly)

add_library(orcaslicer_ai_smart_slicing STATIC ${ORCA_AI_SMART_SLICING_SOURCES})
add_library(OrcaSlicer::SmartSlicing ALIAS orcaslicer_ai_smart_slicing)
target_link_libraries(orcaslicer_ai_smart_slicing PUBLIC OrcaSlicer::AIContracts)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ORCA_AI_SMART_SLICING_SOURCES})
encoding_check(orcaslicer_ai_smart_slicing)
