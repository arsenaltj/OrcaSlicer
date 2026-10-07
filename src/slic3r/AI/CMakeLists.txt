set(ORCA_AI_SMART_SLICING_SOURCES
    SmartSlicing/Application/ApplyService.cpp
    SmartSlicing/Application/ApplyService.hpp
    SmartSlicing/Application/ApplyWorkflow.hpp
    SmartSlicing/Application/CandidatePlanningWorkflow.hpp
    SmartSlicing/Application/CandidateSearchPipeline.cpp
    SmartSlicing/Application/CandidateSearchPipeline.hpp
    SmartSlicing/Application/CandidateSearchSessionPlanner.cpp
    SmartSlicing/Application/CandidateSearchSessionPlanner.hpp
    SmartSlicing/Application/CandidateSelectionTaskMapper.cpp
    SmartSlicing/Application/CandidateSelectionTaskMapper.hpp
    SmartSlicing/Application/LocalModelFeatureAnalyzer.cpp
    SmartSlicing/Application/LocalModelFeatureAnalyzer.hpp
    SmartSlicing/Application/PrintabilityInspector.cpp
    SmartSlicing/Application/PrintabilityInspector.hpp
    SmartSlicing/Application/ProtectedRegionBinder.cpp
    SmartSlicing/Application/ProtectedRegionBinder.hpp
    SmartSlicing/Application/OwnerThreadCallbackGate.hpp
    SmartSlicing/Application/RecommendationSessionCoordinator.cpp
    SmartSlicing/Application/RecommendationSessionCoordinator.hpp
    SmartSlicing/Application/RecommendationWorkerFailure.cpp
    SmartSlicing/Application/RecommendationWorkerFailure.hpp
    SmartSlicing/Application/SmartSlicingCoordinator.cpp
    SmartSlicing/Application/SmartSlicingCoordinator.hpp
    SmartSlicing/Application/TrialSliceScheduler.cpp
    SmartSlicing/Application/TrialSliceScheduler.hpp
    SmartSlicing/Application/TrialSlicingWorkflow.hpp
    SmartSlicing/Application/VersionedApplyWorkflow.cpp
    SmartSlicing/Application/VersionedApplyWorkflow.hpp
    SmartSlicing/Domain/CandidateComparison.cpp
    SmartSlicing/Domain/CandidateComparison.hpp
    SmartSlicing/Domain/CandidateEvaluation.cpp
    SmartSlicing/Domain/CandidateEvaluation.hpp
    SmartSlicing/Domain/CandidateSelection.cpp
    SmartSlicing/Domain/CandidateSelection.hpp
    SmartSlicing/Domain/GoalContract.cpp
    SmartSlicing/Domain/GoalContract.hpp
    SmartSlicing/Domain/IntentConstraintSnapshot.cpp
    SmartSlicing/Domain/IntentConstraintSnapshot.hpp
    SmartSlicing/Domain/MachineCapabilitySnapshot.cpp
    SmartSlicing/Domain/MachineCapabilitySnapshot.hpp
    SmartSlicing/Domain/MaterialCompatibility.cpp
    SmartSlicing/Domain/MaterialCompatibility.hpp
    SmartSlicing/Domain/ModelFeatureSnapshot.cpp
    SmartSlicing/Domain/ModelFeatureSnapshot.hpp
    SmartSlicing/Domain/ParameterProposal.hpp
    SmartSlicing/Domain/ApplyContract.cpp
    SmartSlicing/Domain/ApplyContract.hpp
    SmartSlicing/Domain/ParameterPolicy.cpp
    SmartSlicing/Domain/ParameterPolicy.hpp
    SmartSlicing/Domain/ParameterProposalValidator.cpp
    SmartSlicing/Domain/ParameterProposalValidator.hpp
    SmartSlicing/Domain/PerformanceAcceptance.cpp
    SmartSlicing/Domain/PerformanceAcceptance.hpp
    SmartSlicing/Domain/SixNozzleValidation.cpp
    SmartSlicing/Domain/SixNozzleValidation.hpp
    SmartSlicing/Domain/PlacementCandidate.hpp
    SmartSlicing/Domain/PrintabilityReport.hpp
    SmartSlicing/Domain/ProtectedRegionBinding.cpp
    SmartSlicing/Domain/ProtectedRegionBinding.hpp
    SmartSlicing/Domain/RecommendationTypes.cpp
    SmartSlicing/Domain/RecommendationTypes.hpp
    SmartSlicing/Domain/RepairPlan.hpp
    SmartSlicing/Domain/RiskAssessment.cpp
    SmartSlicing/Domain/RiskAssessment.hpp
    SmartSlicing/Domain/RiskConfirmationPolicy.cpp
    SmartSlicing/Domain/RiskConfirmationPolicy.hpp
    SmartSlicing/Domain/SliceCandidate.hpp
    SmartSlicing/Domain/SlicingMetrics.hpp
    SmartSlicing/Domain/SmartSlicingTypes.hpp
    SmartSlicing/Domain/TrialMetrics.cpp
    SmartSlicing/Domain/TrialMetrics.hpp
    SmartSlicing/Domain/WorkflowResourceBudget.hpp
    SmartSlicing/Domain/WorkflowState.hpp
    SmartSlicing/Domain/WorkspaceContext.hpp
    SmartSlicing/Domain/WorkspaceRevision.hpp
    SmartSlicing/Infrastructure/FileRecommendationSessionJournal.cpp
    SmartSlicing/Infrastructure/FileRecommendationSessionJournal.hpp
    SmartSlicing/Infrastructure/InMemoryUserMarkedRegionRegistry.cpp
    SmartSlicing/Infrastructure/InMemoryUserMarkedRegionRegistry.hpp
    SmartSlicing/Ports/IOfficialSliceGateway.hpp
    SmartSlicing/Ports/IOrcaWorkspace.hpp
    SmartSlicing/Ports/IParameterAdvisor.hpp
    SmartSlicing/Ports/IModelFeatureAnalyzer.hpp
    SmartSlicing/Ports/IProtectedRegionSource.hpp
    SmartSlicing/Ports/IRecommendationSessionJournal.hpp
    SmartSlicing/Ports/ITrialSliceExecutor.hpp
    SmartSlicing/Ports/IWorkflowRuntimeStore.hpp
)

add_library(orcaslicer_ai_contracts INTERFACE)
add_library(OrcaSlicer::AIContracts ALIAS orcaslicer_ai_contracts)
target_include_directories(orcaslicer_ai_contracts INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(orcaslicer_ai_contracts INTERFACE boost_headeronly)

add_library(orcaslicer_ai_smart_slicing STATIC ${ORCA_AI_SMART_SLICING_SOURCES})
add_library(OrcaSlicer::SmartSlicing ALIAS orcaslicer_ai_smart_slicing)
target_include_directories(orcaslicer_ai_smart_slicing PRIVATE ${CMAKE_SOURCE_DIR}/deps_src)
target_link_libraries(orcaslicer_ai_smart_slicing PUBLIC OrcaSlicer::AIContracts)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ORCA_AI_SMART_SLICING_SOURCES})
encoding_check(orcaslicer_ai_smart_slicing)
