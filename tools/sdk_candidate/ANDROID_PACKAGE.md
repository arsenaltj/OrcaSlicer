# Android-only b5f source artifact

`package_android.py` is independent of the original four-platform adapter. It accepts exactly `model-android` and `slicing-android`; extra Windows stages fail. It preserves their literal qualified seal/raw SHA+size constants, original terminal/raw-source/recovery origin, source/command/object inventories and original resource summary/execution association. Full execution files are streamed for SHA, never JSON-loaded. No Windows SDK, runner, capabilities or Host token is required or produced.

Plan schema is `orca-android-sdk-plan-v1`. Required concrete fields:

- `source` b5f078f52a8db9bb8eb3b347b155679c852d5c32, `tree` cd7a99640b2c396917176e1e7e9ee1e356b6915a, separate `toolSource` full current tool commit, clean `sdkRepository`, `bindingPath`.
- `producers` exact two-stage original receipts: terminal/rawSource/seal/fullIndex/originalRecords. `resourceSummaries` exact same stage keys and original sealed resource summary pins. No new typed producer success is invented.
- `runtimeInputs`: map basename `.so` to `{stage,source:{path,bytes,sha256}}`. It includes both raw SDKs and approved byte-identical libtbb.so; additional runtimes must belong to corresponding source/companion graph and the complete transitive NEEDED closure. Extra runtimes fail. Only the original normal script's literal Android system-library set is exempt.
- `companions`: each stage `{receipt,seal}` for the existing first-observation `b5f-producer-companion-v1` schema. Each must include cache and approved binding and original drift/sourceAppendix graph. Approved TBB verifier/rule/header inventory are checked by the original `companion` Android basis. Missing or stale support fails.
- Fresh, separate `output` basename `sdk-android-b5f078f52a8d-rN`, `debug`, `evidence`; none may overlap SDK/tool/producer build/raw/seal roots. The resulting archive in evidence has basename `<output.name>.zip` and entries relative to the package root.
- `artifactOwner` exact current local artifact_owner.py; original fixed `ownerDirectory` three cores; `python`,`git`,`strip`,`readelf` exact absolute images, current `dependencyManifest` and fixed CGAL archive pins; `toolFiles` complete source/core/image/script/data closure. Python must equal the current interpreter. LLVM strip/readelf are the only native roots. No CDLL is imported.
- `qualification.review` and `.qa` are separately verified `android-artifact-qualification-v1` receipts containing role/outcome=passed/toolSource/toolFiles/lifecycle/inputs/report-pin. `inputs` equals `input_contract(plan)` exactly; no old e2b/fixed-fixture or four-platform qualification applies. Missing receipts stay unbound until independent review/QA bind the concrete contract.
- `lifecycle = life(plan)` is disabled by default. Execution explicitly requires `life(plan,True)` and identical typed GO lifecycle: SDK_SUSPENDED_ROOT_V1, flags0x80404, never-resumed abort74, CURRENT_CREATING_PROCESS, 7200-second command budget, source/tool/cwd/derived commands/toolFiles. Root must independently authorize it. No implicit legacy fallback.

CLI uses the pinned Python with `-B`:

```text
python.exe -B tools/sdk_candidate/package_android.py --binding <absolute-plan.json> --binding-sha256 <sha>
```

Without GO this only admits a metadata plan shape and prints NO_EXECUTION; it neither certifies a complete binding nor reads/hashes real libraries. Actual packaging, after independent review/QA and fresh root artifact-only GO:

```text
python.exe -B tools/sdk_candidate/package_android.py --binding <plan> --binding-sha256 <sha> --go <go> --go-sha256 <sha>
```

A separate `--observe-inputs` flag uses GO operation `observe-inputs`. It observes exactly the two producers' existing cache/approved-binding files and appends real `FIRST_OBSERVED_NOW_NOT_HISTORICAL_COMPILE_TIME_HASH` receipts. It does not invoke strip/readelf, build or copy libraries. It requires both successful origin graphs, original full drift closure, concrete `sourceAppendices` and exact `observationRequests` stage arrays `[cache,approved-binding]` with basis/producerBuildRoot/producerOutputPath. The old four-stage collector is not called. After observation the packaging plan binds these new receipts and new concrete-contract qualification. No observation or actual GO is run in development.

GO schema `android-artifact-go-v1` defaults to disabled in any preparation. Actual fields: `authorized:true`, `artifactOnlyAuthorized:true`, `allowSDKLoad:false`, `rootConversation` original root01a20286-30e2-7a20-a134-d11895959c44, `operation:package-android` (or observe-inputs), issuedUtc/expiresUtc valid for at most360seconds, exact planPath/SHA, lifecycle, closure and `cancel` equal evidence root with `.cancel` suffix. The closure exactly contains all origin/full/read/object/raw identities, actual raw build path, source seal, runtime refs, toolFiles, concrete qualification receipts+reports, companion/appendix/drift basis. Observation substitutes its appendices/requests for still absent companions. Every ref is verified; duplicate/conflicting paths fail. Only root can supply a fresh actual GO; no executable example with invented authority is shipped.

Canonical native command table comes solely from `commands(plan)`:

```text
llvm-strip.exe --strip-debug <qualified Model raw> -o <output>/android/arm64-v8a/liborca_model_sdk_c.so
llvm-strip.exe --strip-unneeded <qualified Slicing/nonTBB runtime raw> -o <matching output>
llvm-readelf.exe -d <each exact raw and release runtime>
llvm-readelf.exe --dyn-syms --wide <each exact raw and release runtime>
```

TBB is copied unchanged, never stripped. Model raw is also preserved in external debug output. The streaming ELF implementation produces the original Model `elf64-load-identity-v2` policy: complete loader header (only e_shoff/e_shnum/e_shstrndx normalized), program headers, full PT_LOAD payload hashes, ALLOC sections and file/VA mapping, full dynsym/NEEDED equality, source markers, 16KiB alignment. Model raw must have debug sections and release must not. Slicing and other runtimes use the original strip-unneeded rule with the same complete runtime identity and 16KiB proof; both SDK public ABIs are exact9/7 functions. The raw704MB library is never loaded wholesale.

Artifact protocol: verify inert three-core pins before importing admission, original8.5GiB dual admission gate, strict>3GiB IO and single-heavy guard, exact command/image/cwd/budget contract, original retained Job suspended root/PI proof/unique Resume/natural wait/Job-double-zero/Close unchanged. Metadata Git uses only pinned Git HEAD/tree/status/ls-files. All source/dependency IO remains under monitor. First failure records partial state and prevents candidate-result publication. Final closure/source/tail checks precede atomic completion receipt.

Manifest schema is **`schemaVersion: "orca-android-sdk-v1"`**, independent of old numeric schema1. Exact fields:

- bundleId=output basename; platformScope=[android-arm64-v8a]; sourceCommit/buildSourceCommit/packagingSourceCommit=b5f; toolSource; sourceStatus clean; uncommitted false.
- android={abi:arm64-v8a,api:24,ndk:28.2.13676358}; files entries `{path,sha256,sizeBytes}` list every actual package file except manifest itself, including headers/licenses/provenance/source ZIP.
- Both canonical `include/orca/model/model_sdk_c.h` and same-byte alias `include/orca/mobile/model_sdk_c.h`; `include/orca/mobile/slicing_sdk_c.h`; actual shared libraries under android/arm64-v8a.
- sourceFiles `{path,sha256}` matches source-snapshot.zip/source-files.json exactly. sourceSnapshot=SHA256(json.dumps(sourceFiles,sort_keys=True).encode()). ZIP entries exactly sourceFiles paths plus source-files.json and dependency-source-inputs.json. Includes permanent tests/native source/presets and complete dependency source inventory, original notices and fixed CGAL archive; paths are relative, unique and verified. Dependency-source entries are namespaced dependencies/<manifest-key>/<relative>.
- producerProofs={origins,resourceSummaries,companions}; androidStaticProof={path:provenance/android-static-proof.json,bytes,sha256}; androidModelPostprocessing={proof:same-ref,library:liborca_model_sdk_c.so,operation:strip-debug,runtimeEqual:true}. The actual proof lists per-library original/release identities, complete runtimeIdentity, operation, removedDebugSections and dynamicTablesEqual, and all NEEDED edges/system exceptions.
- runtimeCapabilities={status:pending-phone,evidence:null}; functionalQualification=null. No capabilities.json or Windows field is fabricated. Phone qualification is recorded separately and does not mutate the source artifact. Device/AI/iOS locks remain unchanged. This source artifact alone never claims phone behavior or physical printing/affectedAI/GLB completion.

Development only uses tiny synthetic files and fakes, including a byte-for-byte runtime identity comparison to the original Model implementation and the actual approved-TBB validator on a tiny explicitly patched fixture rule. Production literal SHA guards cannot be overridden by plan fields. Old97+8 tests/semantics remain. INPUT-CLOSURE-01 pre3/post4 remains; another independent failure of that same closure defect is post5 and stops at root, regardless of this new platform scope.
