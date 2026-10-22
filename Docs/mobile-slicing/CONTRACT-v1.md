# Android local slicing contract v1

Authoritative contract for P1 tasks task-20261020172216196-253d95 / task-20261020172248383-93d97f.

## Channels and operations
Flutter MethodChannel: com.orca.mobile/slicing. EventChannel: com.orca.mobile/slicing/events.
Methods: importProject({uri|path}), updateProject({projectId, changes}), startSlice({projectId, quality, options}), startRecommendation({projectId}), applyRecommendation({projectId,candidateId}), undoRecommendation({projectId}), getJob({jobId}), listJobs({}), cancelJob({jobId}), resumeJob({jobId}), getResult({jobId}), exportResult({jobId}), getProject({projectId}). Heavy calls return a durable {jobId,projectId} ticket. Read-only calls return persisted snapshots. Events are complete job snapshots, not deltas.
Job: jobId, attemptId, projectId, projectRevision, sequence, operation, state, phase, progress(nullable 0..1), elapsedMs, message, error(nullable), result(nullable). States: queued,running,cancel_requested,canceled,succeeded,failed,interrupted,paused. Terminal succeeded requires committed artifacts.
Project: projectId, revision, inputPath, inputSha256, name, model{vertices,triangles,bounds{min,max},colors,filamentMap}, config, transforms, scenePath.
Quality keys: standard,fine,fast,smart. Default 0.4mm heights: .20,.12,.28; smart performs local analysis and native trials. No remote/AI service.
Results: inputSha256, configSha256, engineVersion, sourceCommit, projectRevision, gcodePath,gcodeSha256,previewPath,layerCount,printTimeSeconds,filamentVolumeMm3,filamentGrams,warnings,candidates(optional),selectedCandidateId(optional).
Recommendation candidates: candidateId,quality,config,configDiff,reasons,metrics{printTimeSeconds,filamentGrams},warnings,artifact paths,validated. Max baseline+three serial trials. Apply/undo changes project revision and invalidates previous outputs.

## Native execute JSON
Synchronous worker-only C ABI. Request operation: import,update,validate,analyze,slice,recommend,save. Common: jobId,attemptId,projectId,projectRevision,inputPath,projectPath,outputDir,quality,config,transforms,maxThreads(default4),maxMemoryBytes, resumeManifest. Context owns one operation at a time; cancel is thread-safe. Callback JSON has type(progress/checkpoint/warning), jobId,attemptId,phase,progress nullable,message,checkpoint optional. Result JSON follows Project/Results above. No C++ exception/STL across ABI. Caller frees returned strings.

## Portable project module boundaries
Project/recommendation worker owns src/orca_mobile_slicing_sdk/project_support.hpp/.cpp and recommendation_support.hpp/.cpp only. Expose orca::mobile::slicing::load_project(inputPath,configOverrides,transforms,workspaceRoot), describe_project, write_scene, quality_config, recommendation_candidates. Native runtime coordinator owns engine.cpp/c_api.cpp/public C header; build worker owns mobile-slicing/**. Communicate any signature changes via coordinator before editing shared contracts.

## Scene binary ORCASC01
Little endian: magic8; uint32 vertexCount,triangleCount,paletteCount,flags; uint64 reserved => 32-byte header. Float32 positions[vertexCount*3]; UInt32 triangle indices[triangleCount*3]; UInt16 facePalette[triangleCount]; UInt8 paletteRGBA[paletteCount*4]. Coordinates in mm, transforms already applied. Scene reduction allowed only in renderer, native source model untouched.
## Toolpath binary ORCAPV01
Header32: magic8; uint32 layerCount,segmentCount; uint64 indexOffset,dataOffset. Layer index16: float32 z,uint32 firstSegment,count,reserved. Segment32: float32 x0,y0,z0,x1,y1,z1,width; uint16 paletteIndex,uint8 role,flags. Palette supplied in small result/project JSON. Render current layer/range only.

## Ownership/recovery
Kotlin foreground service in :slicing process owns native handle and SQLite. UI never owns the worker. Import streams to app-private files. Persist config/input identities before native execution. Native completed candidate artifacts checkpoint atomically; recreate context after process death, reuse validated completed candidate manifests, rerun interrupted current candidate. User cancel never auto-resumes. No printer connected means export enabled and send/start disabled with deferred reason.

## Frozen scopes
Coordinator owns pubspec/lock, MainActivity registration, AndroidManifest, Gradle root wiring, public headers/contracts, native runtime adapter, sdk/slicing locks. Each worker edits own independent worktree. No phone writes except coordinator/QA scheduled acceptance.
