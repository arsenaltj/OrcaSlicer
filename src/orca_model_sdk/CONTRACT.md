# Model SDK v1 / ABI 1

## Host integration and source publication

The standalone CMake default keeps SDK consumer and IO diagnostics tests enabled,
with optional RegionCore and RepairCore disabled. The IO diagnostic target copies
the SDK's actual link dependencies only when present; the SDK-only profile has no
link dependency and must configure and generate without a NOTFOUND library.
RepairCore profiles retain their real core and transitive runtime dependencies.

The Model SDK is a separately built library. Updating an Android consumer's SDK
bundle does not update the desktop GUI: that GUI currently calls the desktop
finishing core directly, while its Device SDK is a different target. Shared
Model SDK changes can affect any host that actually adopts this library; the
Android-only publication branch does not make the whole library Android-only.

The qualified Android bundle `sdk-native05-android-exclusive-publish-20261119-r1`
was built and packaged from source `6aa69c260afe3d811f4a7a8eaf350d45f1b51385`.
Later integration merges or documentation commits do not change that immutable
producer identity and are not evidence of a rebuilt bundle or desktop installer.
Software/package checks and Android device acceptance remain separate gates.

Model repair is local processing, not account task admission. A desktop or mobile
host integrating platform generation must implement the platform run and download
admission protocols; this C ABI does not allocate account concurrency slots,
acquire a remote execution lease, or authorize a new paid generation.

## Original GLB safety policy v1

`capabilities.glbSafetyV1` advertises `policyVersion:"glb-safety-v1"`, the
`inspectGlbArtifact`/`repairGlbArtifact` operations, `strictSelfIntersection`,
`preservesOriginal`, `preservesEncodedAppearance`, `safeRepairStrategies`, and
`generalTopologyRepair:false`. Only builds with `ORCA_MODEL_GLB_CGAL=ON` run the
real CGAL EPICK self-intersection predicate. Missing predicates are unsupported,
not zero defects. The ordinary mesh editing and C ABI remain compatible.

Both requests require `identity:{accountId,jobId,assetId,assetVersion,sourceSha256,
operationId}` and absolute `sourcePath`. The trusted host creates the context
under the account/run's application-private directory. Source paths must remain
inside that root and may not traverse symbolic links or reparse points. Repair
publishing additionally requires an existing private `outputDirectory`.

Results return `policyVersion`, `identity`, `source:{path,sha256,bytes}`,
`candidate:{path,sha256,bytes}|null`, `repairApplied`, `checks`, and `completion`.
Source and candidate also identify `format:"glb",mediaType:"model/gltf-binary"`.
Derived files above 100 MiB fail before publication, matching the original
artifact byte boundary; the viewer's smaller display profile is independent.
Each check exposes `performed`, `status:passed|failed|not_run|unsupported`, nullable
`count`, and diagnostic details. `verified` requires actual passed structure,
geometry, topology, selfIntersection and appearance checks, and no other
performed failed check. Incomplete/failed checks return `needs_attention` or
`unsupported` and publish no candidate. Canceled operations fail with `CANCELED`.
Inspect can return a verified source with null candidate. Repair always publishes
a distinct verified file on success; an already-safe model uses an identical-byte
copy with `repairApplied:false`, not an invented changed hash.

Successful publication returns optional `receipt:{path,sha256,bytes}` for an
atomic JSON sidecar containing the complete result before the receipt field is
added. This is local algorithm evidence, not a server attestation. Names bind the
SHA256 of all six identity fields. A fresh-context retry repeats source and
derived checks and only reuses identical existing candidate/receipt files;
different existing bytes are rejected without overwrite. A crash between model
and receipt publication can be recovered only after repeating the original-to-
candidate geometry and appearance proof. Hosts persist and validate both hashes.
Safety publication exclusively creates a unique private temporary file, flushes
it, and atomically publishes without replacing any existing node. Existing
candidate and receipt aliases of the source (canonical or hard-link equivalent)
are rejected before any write; fixed legacy `.tmp` paths are never opened.
On Windows, private-path identity/containment compare ordinary canonical identities;
filesystem queries and final CreateFileW/MoveFileExW/DeleteFileW arguments use
extended-length spelling internally. This supports real long private run directories without
requiring the caller to change source/workspace identity or relax path guards.
Safety-local file reads and filesystem queries use the same extended API
spelling, so a non-longPathAware host can reread long candidate and receipt
paths. Canonical containment still compares the ordinary identity; callers do
not need to introduce extended namespaces into input JSON. Context initialization
uses the same spelling for workspace creation, and the standalone runner applies
it to control-file I/O while preserving canonical result containment. Ordinary Model SDK
save/export/read behavior and Android publication semantics remain unchanged.

The supported subset is one static untransformed triangle mesh in an identity
node hierarchy, dense float positions/optional normals/UV0, embedded PNG/JPEG,
and bounded unsigned indices. Required/used extensions, animations, skinning,
arbitrary transforms and external resources are explicitly rejected. GLB2/chunk/
accessor references, finite coordinates, triangle indices, UV binding, encoded
image dimensions (at most 64 Mi pixels, matching desktop), and material texture
references are checked. This preserves encoded image bytes; it is not a full
pixel decoder or a visual-print quality certificate. Limits remain 100 MiB input,
2.5 million combined position vertices and 2 million triangles.

`glbSafetyV1.maxGeneratedFaceLimit` is currently 1,000,000: hosts must reject the
unsupported 2M generation/local-processing combination before purchasing a
generation, without silently substituting 1M. In bounded RepairCore builds,
`inspectionWorkspacePolicy:streamed-inspect-live-containers-v1` admits inspection
using the maximum live-container phase plus allocator/library/JSON reserve after
actual accessor streaming and exact dense-capacity reservation. The response
reports `workspaceAdmission` phases and dense capacities. Limited v1 repair
keeps `inputBytes*4 + positionVertices*96 + triangles*448` because its removal and
publication storage differs; `repairWorkspacePolicy` states that boundary.
Both reject above768MiB. These are bounded-admission estimates, not OS RSS
guarantees. A cooperative
120-second deadline is checked in controlled loops and at stage boundaries;
the synchronous CGAL predicate can overrun before returning. Existing 2M model
parameter support is unchanged elsewhere; expanding this local SDK combination
requires separate actual Android resource validation.

Topology checking uses exact-position seam identification, complete indexed-soup
validity, closed/manifold/oriented edge and vertex-link checks, followed by CGAL
`does_self_intersect`. A boolean self-intersection diagnostic is a risk flag, not
the number of all intersections. Invalid soup is not silently reoriented.

The only automatic repair strategy is removal of isolated coincident two-face
shells (at most 64 removed faces); attached duplicates, holes, nonmanifold fans,
general retriangulation or shape reconstruction are not guessed. It appends new
indices while retaining all original BIN bytes and material/UV/image definitions.
Automatic deletion additionally requires a single primitive using the default
opaque material, no explicit material/texture/UV/normal attributes, and exact
coverage of every removed triangle by one retained triangle. Merely nearby or
small surfaces are not appearance-equivalent. Textured or explicitly shaded
surfaces can still be verified/copied unchanged, but deletion is needs_attention
until per-corner appearance equivalence is actually implemented and proved.
Retained geometry is identical. Every removed triangle's vertices and centroid
are measured against the retained surface with CGAL distance predicates, at
least as strict as desktop's sampled 0.002 diagonal deviation and 0.02 unique
surface-area change ratios. The derived GLB is reparsed, topology and actual
self-intersection are rechecked, encoded appearance is proved unchanged, and
source/candidate SHA are reread before return. Raw originals are never replaced.

Progress callbacks carry `schemaVersion:1,type:"glbSafetyProgress",identity`,
monotonic per-call `sequence`, and actual `phase` (reading, structure, geometry,
topology, repair, revalidation, publishing). No fabricated percentages.
Strict predicate boundaries additionally report residualSoup, residualSurface,
residualVertexLinks, residualIntersections and residualComplete. They preserve
the same identity and monotonic sequence, and add cooperative cancellation
points; they do not imply preemption inside the underlying CGAL call. The host
owns queued/active scheduling. Cancellation is checked at stage boundaries and
in parsing/mesh/distance loops; the synchronous CGAL predicate itself has no
mid-call cancellation hook, so host worker termination must handle an imposed
hard timeout. iOS availability must be determined by an actual platform build.

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

An experimental, opt-in `ORCA_MODEL_REPAIR_CORE` build target exposes the exact
desktop enhanced `repair_mesh` Core to a standalone link consumer. Its M1 result
is `geometry_only_stage`, not a Model SDK operation/receipt or strict completion.
See [repair Core boundary](repair_core/README.md) for actual source/dependency
binding, tested scope and the unsupported full strict numeric dependency.


## Enhanced GLB repair policy v2

Only builds with `ORCA_MODEL_REPAIR_CORE=ON` advertise `glbRepairV2`.
`repairGlbArtifact` accepts top-level `policyVersion:"glb-repair-v2"` with the
same six-field `identity`, private `sourcePath` and `outputDirectory`. Missing
policy continues v1; unknown policies fail. The current v2 profile is static,
embedded, unextended GLB, one primitive/material with POSITION, optional NORMAL
and TEXCOORD_0. Additional primitives, transformed input nodes, tangent/color
attributes or extensions are explicitly unsupported; they are never stripped.
Two/three texture channels of the measured witnesses are supported, not a claim
of universal GLTF material conversion. Input/original bytes remain immutable.

The actual entry is desktop323c `repair_mesh`, not `strict_repair_mesh`.
`completion:"native_compatible"` requires actual native success, finite indexed
nonempty output, actual serialized GLB readback with identical ordered geometryId,
structure and appearance passed. Full bidirectional geometric deviation is
`performed:false/status:not_run/count:null`; topology/self-intersection are actual
CGAL facts and residual failures remain visible. This completion does not grant
manufacturing safety. The current build does not advertise strict_verified.

Raw result and private receipt retain sibling `identity`, `source`, `candidate`,
`checks` (five facts), `completion`, `repairApplied`, and `nativeProof`. Candidate
and source retain path/sha256/bytes/format/mediaType. `receipt` in the raw response
has path/sha256/bytes; the receipt itself has no self hash, receiptSha256 or bundle
hash. The App verifies and attaches receipt/bundle identities to its API summary.
NativeProof records actual repairEntry/coreSourceCommit, source/output/readback
geometry IDs, nativeRepairSucceeded, geometryValid/readbackVerified, cancellation
and resource facts, appearancePolicyVersion; private evidence may be richer than
API's closed summary. Runtime timings are in the response, excluded from stable
receipt equality. Replays actually re-execute and verify source -> candidate;
they do not trust a preexisting sidecar or replace different bytes.

Geometry is converted once into Core Z-up millimetre float coordinates. Carrier
POSITION preserves those raw floats (including signed zero); a single canonical
root matrix expresses Y-up metres. Actual readback decodes those raw coordinates
in the declared Core frame, avoiding a float divide/multiply roundtrip. Triangle
order is preserved. Per-corner appearance uses exact-face lookup first, then the
existing Core AABB tree and barycentric projection, bounded by 0.002 of the repaired model
scale. UV seams split vertices, subject to the original vertex cap. Source encoded
image bytes and material/texture/sampler semantics are retained, including PBR,
alpha and normal-texture factors. Normals, when provided, are projected, normalized
and aligned to repaired winding; tangent basis is derived from repaired UV/normal
by the GLTF renderer, not claimed byte-identical. Files without authored NORMAL
retain GLTF's derived normals. CPU/bytes/projection checks do not claim rendered
pixels are identical; visual/device validation is reported separately.

Limits remain 100 MiB input/candidate, 2.5M vertices, 2M triangles, generated1M,
768 MiB **estimated** workspace admission and 120s cooperative deadline. This is
not an OS hard RSS cap or preemption inside every native instruction. Actual host
peak measurements are reported; cancellation and limit errors do not publish a
new accepted candidate/receipt. Publication uses exclusive temporary files,
flush and atomic no-replace rename/link; late cancel cleans newly created files,
never prior validated files or the original. Application-run/account/op fencing
remains the host's responsibility around the bound native identity.

Progress remains `type:glbSafetyProgress`, exact identity, monotonic sequence.
Core progress is real; mapping/carrier/readback/publishing phases are stage events.
The enhanced build requires private source-built tbb12.dll on Windows. Android
explicitly imports the already approved Slicing libtbb.so, pinned by byte size,
SHA256 and the same 139 source headers (see repair_core/approved_tbb.json).
Missing inputs, mismatched bytes or a conflicting TBB target fail configuration.
Model and its consumer are actually relinked against that imported library.
The package records existing-slicing-runtime provenance separately from the new
Model source commit; the old Model04/Slicing/TBB/malloc bundles are unchanged.

Release packaging may explicitly apply LLVM `--strip-debug` to the new Android
Model library only. It preserves the full unstripped library outside the SDK.
Every SHF_ALLOC section (including dynamic symbols/versions, NEEDED strings and
unwind data) remains byte-identical; all PT_LOAD entries, addresses, sizes, flags
and alignment must match. A difference refuses publication. The producer records
tool version/hash/command, raw and packaged hashes, runtime identity proof, and
separate buildSourceCommit/packagingSourceCommit. Consumer locks bind the actual
packaged hash. Approved Slicing, TBB/malloc and Model04 are never postprocessed.

The `elf64-load-identity-v2` proof additionally validates each file-backed ALLOC
section's file offset against its actual PT_LOAD virtual-address mapping and
hashes every loaded file byte, including padding outside sections. Only ELF64
`e_shoff`, `e_shnum` and `e_shstrndx` may differ; their section tables must still
be valid. Every other ELF-header byte and all program headers remain identical.
Section-header redirection cannot substitute out-of-load copies for loaded code.

A positive boolean `does_self_intersect` proves a risk but does not enumerate
intersection pairs: failed count is null and details.riskFlag=true. A negative
predicate permits passed/count0. Unknown topology enumeration is likewise null;
actual edge/face and non-manifold vertex counts are retained when available.

POSITION accessor min/max are computed per axis from the serialized Core float
bytes and checked against actual readback before structure may pass. They stay
in that raw accessor frame; the display root is not applied to these bounds.


## v2 2M resource candidate

The opt-in v2 Release candidate can advertise generated2M after actual bounded
host checks; v1 stays generated1M. See repair_core/RESOURCE_ENVELOPE.md for phase
admission, explicit carrier checks, preserved CGAL predicates and the private
pinned lifetime adapter. Limits remain768MiB estimated/120s cooperative/100MiB/
2.5M vertices, and resource errors publish nothing. Actual Android runtime and
independent review/QA remain required; no arbitrary-model success is promised.

Candidate layouts may compact inactive source geometry to fit the existing64MiB
display target, retaining every active encoded image and material/texture/sampler
semantic. Only image bufferView identities are rebased; encoded bytes are checked
per image and ordered native geometry is actually reread. Original files are
unchanged. Whole-source-BIN equality remains required for uncompacted layouts.

## Static scene and specular factor input profile

The static single-mesh scene may use finite, invertible affine node matrices or
exclusive TRS with a near-unit quaternion (squared norm tolerance 1e-5, normalized
before use). World = parent * local, TRS = T * R * S. Every node belongs to the
one active scene, with one mesh instance; cycles, shared children, orphans,
skins, animations, matrix/TRS conflicts and singular transforms are rejected.
World POSITION is baked in metres/Y-up before the existing single Core frame
conversion. Authored NORMAL uses the world inverse transpose and normalization;
negative determinant reverses source corners together with UV/normal references.
Output still uses the canonical Core mm/Z-up root, not the original transform.

Only factor-only KHR_materials_specular is additionally supported: the extension
object contains exactly specularFactor, a finite number in [0,1]. Its used/required
declarations and full material descriptor survive export and independent readback.
Other material extensions, unknown keys, specular textures and color semantics
remain unsupported. Zero is preserved, not treated as a default/no-op. Encoded
image hashes and existing per-corner projection checks remain required. Private
appearance evidence records the actual source world matrix, reflection handling
and material descriptor hash. This is no device pixel-equivalence claim and does
not promote native-compatible output to strict manufacturing safety.

## Safe optional IO failure facts

Only explicit IO_ERROR throws at actual file_open/file_read/temp_create/temp_write/temp_flush/output_publish sites may add error.ioDiagnostic with schemaVersion1 and a closed kind (unknown/access_denied/no_space/quota_exceeded/read_only/not_found/too_many_files/unsupported/cross_device/io_failure/interrupted). OS errors are captured immediately before cleanup. Stream calls clear errno before each operation; no error or successful short writes are unknown. No numeric code/path/system text or guessed candidate/receipt role is added. Public C ABI, SDK version, IO actions, retry/cleanup/EEXIST/atomic publication and successful proof are unchanged. Test controls exist only in separately compiled orca_model_io_test, not installed production SDK or ENV. Diagnostic instrumentation is not a fix or diagnosis of any device IO fault.

## Direct print color workbench v1

Operations getPrintPalette, matchPrintColors, applyPrintColors, savePrintColorDraft require documentId plus expectedRevision. Shared desktop Direct matching (1..6 target/physical colors) and CIEDE2000 quality are compiled without GUI/recipe dependencies. The face palette, manual protectedFaces, materialLocks and materialId/slot binding refer to the authoritative Model document face order, not preview LOD. Match stages a standard real scene descriptor candidate; apply requires the same candidate/materialFingerprint/processFingerprint, confirmMaterialMapping=true and explicit acceptColorError=true when over tolerance. IDs are integer logical materials, not preset IDs; routingVerified remains false until verified by Slicing/device configuration.

trialOnly candidates cannot be applied; savePrintColorDraft commits trial colors while retaining original material IDs and marks a nonmanufacturing draft. Existing document save/load includes that state, save now returns documentPath. manufacturingSnapshot/sliceDocument/recommendDocument/exportManufacturing reject draft-only state; a real rematch and apply clears it. Generic acceptCandidate cannot bypass confirmation. Existing undo/redo remain revision-bound.

Slicing exportColorDocument is a new schema2 operation on the replayed authoritative project; it writes double world-baked OBJ and appearance/part bindings, not a SC01 import. Unsupported native annotations/local process semantics are explicitly rejected; returned sourceProjectBinding requires rebind for round-trip into the original native structure. This source contract does not imply old Android Slicing binary support or physical printing qualification.

Slicing prepareColorManufacturingConfig resolves the native device and versioned material catalog. Bindings contain explicit materialId (continuous logical index), slot (unique physical position, possibly sparse), tool (independent physical tool), materialPresetId, optional strictly checked materialPresetVersion, color and purpose. The SDK preserves native source print options, applies trusted native machine options and actual materials, then canonicalizes the complete config and returns the only authoritative fingerprints. applyColorDocument first verifies the unchanged source binding, then reruns this same helper before producing a native 3MF derivative. Support bindings configure support/interface routes without enabling support implicitly.

Honor Forge/Klipper/non-SEMM physical tooling uses a narrowly identified native adapter with an exact source start template, hashed prepared macro and shared bijective material/tool mapping in emission and parsing. Actual G-code is reparsed before native usage/preview evidence is returned. Native material configuration and operator declarations do not verify physical loading: nativeMaterialConfigurationVerified=true, filamentConfirmationSource=operator, actualFilamentLoadingVerified=false. Single activity remains evidence version1 with the same receipt; multiple active materials use version2 with per-material logical/physical/tool/preset-version/family routes.
