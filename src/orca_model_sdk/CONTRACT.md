# Model SDK v1 / ABI 1

Standalone C++17 target; configure this directory directly. No root CMake or GUI
target is required or modified. Shared filename: orca_model_sdk_c.dll / liborca_model_sdk_c.so.

Public header: include/orca/model/model_sdk_c.h. All strings UTF-8, zero terminated.
version/capabilities return borrowed static storage. execute result always freed
with orca_model_free_string. Context is an opaque registry token; destroy removes
it safely and signals cancellation. One operation per context; cancel is thread
safe; event is borrowed and synchronous. Caller must serialize destroy after its
execute returns and must not destroy from an event callback. Duplicate destroy and
cancel of stale handles are safe. Repeated free_string on the same allocation is not.

Request: {schemaVersion:1,operation:string,...}; response:
{schemaVersion:1,ok:true,result:{...}} or
{schemaVersion:1,ok:false,error:{code:string,message:string}}.
The C integer return is zero only for successful operations. Callback exception
must never cross the C ABI.

Operations: capabilities; inspect/import {inputPath, units:"mm"}; create
{vertices:[[x,y,z]],triangles:[[a,b,c]],faceColors?:[[r,g,b,a]],materialIds?:[int]}; getScene;
transform {expectedRevision,entityId?:string,translation?:[x,y,z],rotationDegrees?:[x,y,z],scale?:[x,y,z]};
paint {expectedRevision,faces:[index],color:[r,g,b,a]}; assignMaterial
{expectedRevision,faces:[index],materialId:integer}; undo/redo
{expectedRevision}; save {expectedRevision}; load {documentId};
exportManufacturing {expectedRevision,format:"obj"|"stl",outputPath?:string}.
inspect is read-only; import/create establish an editable document. scene is a
snapshot {documentId,revision,geometryRevision,units:"mm",upAxis:"Z",vertices,
triangles,faceColors,materialIds,bounds}. Face ordinals are valid only for the
same geometryRevision. All editing operates on a detached copy, never source.
save/load uses workspace/documents, original input paths remain unchanged.
Successful mutation increases revision; undo/redo also produce a new revision.

Runner: orca_model_runner --workspace DIR --request request.json --output result.json.
One request per invocation. Durable state uses documentId + load then operation;
runner may instead receive {schemaVersion:1,operations:[request,...]} for an ordered
batch in one context. Result is {ok,results:[envelopes]}; nonzero exit for any failure.
All outputs must remain inside workspace. Callers provide isolated workspace per
job and freeze executable/dependency hashes. Process stderr is diagnostic only.

Default scene responses are compact file descriptors: scenePath (ORCASC01 binary),
sceneMetadataPath, sceneSha256, bounds, source/revision/geometry identity and
sceneRanges. They preserve the existing mobile preview file contract. The binary
is preview-only; manufacture uses the source-precision document. The explicit
returnInlineScene:true request option returns arrays for SDK tests/local tools,
not the default JNI/worker path. Preview files include content identity in names.

Additional operations: pick {origin,direction}; analyze (index-topology report,
not a self-intersection or printability certificate); weldExact (exact-coordinate
welding and duplicate/degenerate indexed face removal); selectRegion {seedFace,
colorDistance?,normalAngleDegrees?,localRadiusRatio?} uses the existing native
VertexColorRegionEditor when built with ORCA_MODEL_REGION_CORE=ON.
prepareCandidate {expectedRevision,command}, acceptCandidate {expectedRevision,
candidateId}, discardCandidate {expectedRevision}; accepting stale input fails.

Optional trusted host setters configure platform HTTP and a verified existing
Slicing SDK dynamic library. platformAI requests only /v1/ai routes from the
platform's unified contract; the host owns authentication/network and its response
storage. slicing {request} forwards the actual Slicing SDK JSON without replacing
its algorithm or claiming support beyond its own capabilities. Host must pin
the library/dependencies through an immutable manifest before configuring it.

`importConverted` accepts {inputPath,appearancePath,units:"mm"} after the host
downloads verified artifacts from POST/GET /v1/model-conversions. The appearance
schema is {schemaVersion:1,units:"mm",upAxis:"Z",geometrySha256,faceColors,
materialIds,parts?,source?,warnings?}; SHA and exact triangle count are checked
before adopting colors/materials. Remaining source/dependency/warning metadata
is preserved. Texture-to-region conversion warnings are not silently cleared.
`platformAI` command convertModel/queryConversion calls that platform endpoint;
it does not submit a paid provider AI task or automatically download arbitrary URLs.

Runner accepts trusted --slicing-backend PATH, or the fixed sibling Slicing SDK
library in the immutable bundle's bin directory when present. Hosts verify every
bundle file/hash before running the package. No operation JSON can select a DLL.

Native modules use a process-resident cache keyed by canonical path and file SHA,
bounded to four modules per process. Repeated contexts reuse one loader reference;
destroy cancels and releases the native session. Loaded files cannot be changed
in place. Restart before changing an existing runtime or exceeding the cache.
Module unload is deferred to process exit because runtimes can own global workers.

v1 implementation capabilities are reported honestly. Native v1 mesh IO supports
STL/OBJ/OFF/ASCII PLY. 3MF/3DS/WRL/DAE/STEP/STP/GLB need platform conversion or
the existing Slicing SDK; they must not be reported as local native support.
No generated G-code, AI provider results, multicolor execution or unsupported
advanced repair is synthesized. Existing Slicing SDK remains authoritative for
actual slicing/SmartSlicing; missing bridges are capability gaps, not success.

`manufacturingSnapshot` requires expectedRevision and produces an immutable
binding file plus the exact manufacturing OBJ/appearance pair and a native v2
request. `sliceDocument` / `recommendDocument` execute that same request through
the configured real Slicing SDK. Native progress events and cancellation cross
the existing ABI; results retain the document, geometry and appearance hashes.
`request` may supply configuration/job identity/budgets, but cannot override the
document source or inject a second native edit history. Change the document
first, then create a fresh snapshot. Configuration still requires explicit
material slots/mapping and multiMaterialConfirmed when using multiple materials.

Converted `parts` uniquely partition vertices/triangles and retain stable entityId
through transforms. An entityId transform updates only that verified part. SC01
sceneRanges carry those identities; topology-changing weld clears invalid ranges
and retains the previous ranges as source metadata. Original attributed or grouped
OBJ, colored/reordered/binary PLY and STL facet attributes require platform
conversion rather than silently losing source appearance or entity semantics.
Large appearance/UV/texture dependency metadata is referenced by a hashed file,
not embedded in the compact default scene result.

Native Slicing requests remain schemaVersion 2. `presets` with
parameterScope:"all" exposes the actual full PrintConfig definitions including
type, native serialization, limits and UI mode; code/readonly fields are labelled.
`exportProject` with confirmDerivedProject:true saves the loaded native model and
effective config through the existing 3MF writer. The result is explicitly a
derived project; unloaded plates, auxiliary payloads and original vendor preset
payloads remain in the preserved source. Unknown/blocked semantics fail rather
than pretending to round-trip an arbitrary source project. `repairMesh` invalidates
face/appearance identity after an explicitly confirmed topology change.
