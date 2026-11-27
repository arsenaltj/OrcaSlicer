# Android arm64 native slicing build

This entry configures and builds the real `libslic3r` FFF core for Android `arm64-v8a` with NDK `28.2.13676358`. It keeps the existing STL/3MF/BBS 3MF import and material/paint facet data paths while excluding wx, Python, OpenGL, SLA, STEP, Assimp, and desktop texture/CAD implementation units.

## Reproducible entry

From the repository worktree:

```text
node mobile-slicing/build_android_arm64.cjs
```

The script uses the Visual Studio bundled Ninja when `NINJA` is unset and the pinned NDK when `ANDROID_NDK` is unset. It configures with `-DANDROID_ABI=arm64-v8a`, `-DANDROID_PLATFORM=android-24`, and 16 KiB ELF page alignment linker flags. It builds the `OrcaSlicer::Core` static archive first; when coordinator `src/orca_mobile_slicing_sdk/c_api.cpp` and `engine.cpp` exist, the same entry also defines `orca_mobile_slicing_sdk`.

Set `ORCA_SLICING_TBB_RUNTIME` to the explicit absolute, already qualified shared Model/Slicing `libtbb.so`; its bytes and same-source headers are verified against `src/orca_model_sdk/repair_core/approved_tbb.json`. It is copied unchanged, never stripped or replaced by a newly compiled same-name runtime. The allocator and all native Slicing implementation objects are built in this task's own build directory. `ORCA_SLICING_BUILD_DIR` and `ORCA_SLICING_EVIDENCE` select explicit build/evidence locations. `ORCA_SLICING_CONFIGURE_ARGS` may contain a JSON array of exact CMake arguments for pinned pure dependency source roots, real CGAL triangulation and the explicit Expat source root; it does not discover another build's objects.

Fetched dependency URLs and SHA256 pins are in `CMakeLists.txt`. Local dependency sources are referenced from `deps_src`; no desktop superbuild is used.

Choose Boost linkage explicitly with `-DORCA_SDK_BOOST_LINKAGE=STATIC` or `SHARED` before configuring. The default is `SHARED` on Windows and `STATIC` on Android. This choice applies while Boost targets are created, independently of later dependency changes to `BUILD_SHARED_LIBS`. Configuration checks the actual compiled Boost target types and writes `boost-linkage.json`; package runtime dependencies must follow the resulting real imports/NEEDED closure.

Host `ctest -R public_three_mf_component_world --output-on-failure` exercises the actual public SDK import and scene vertices, triangle winding and default face material IDs. It preserves two original component-mirror 3MF fixtures byte for byte and covers first/shared references, translations, rotations, scaling, nonuniform scaling, shear, nested noncommuting transforms, zero-centered meshes and ordinary build-item reflection. Original fixtures also run public recommend/validate/slice. Each run retains a fresh evidence directory printed by the test. This regression does not establish Android execution or complete manufacturing qualification.

## Color manufacturing contract

`exportColorDocument` exports the replayed native project as authoritative world-space OBJ plus appearance and original object/volume/instance bindings. SC01 remains preview-only. Unsupported annotations or local process semantics fail explicitly. `applyColorDocument` validates the original source/config/revision and geometry/parts before applying a confirmed Model material plan to a detached native 3MF derivative.

`prepareColorManufacturingConfig` resolves the native device/material catalog, preserves native source print options, and returns complete serialized config and canonical fingerprints. Bindings explicitly separate logical `materialId`, physical `slot` and physical `tool`; logical IDs are continuous but physical positions may be sparse. Support bindings are excluded from the model color palette and route support/interface materials while retaining the current support switch. Native preset identity is the exact id/version/family, not a family alias.

The trusted material producer selects the preset variant for the resolved physical tool's extruder/nozzle type, then writes one parameter row per logical material with `filament_self_index=1..N`. Variant parameters use that selected source row; registered singleton values keep their shared meaning and nonvariant material singletons stay independent of the tool. Unknown, ambiguous, unmatched or malformed variant layouts fail explicitly. Repeated presets and binding input order do not merge logical rows. The existing registered Direct Drive Standard default remains valid for presets without explicit variants. After `Print::apply`, material parameters and logical variant ownership are checked against the authorized configuration before any slicing output is published; a different normalized hash cannot authorize a drifted temperature, fan or flow.

Every ordinary SDK `slice` entry also checks material layout before execution, including generic projects and explicit `singleMaterial`. Input self IDs must cover the project's logical materials, with one unambiguous known variant for each selected tool. A logical material can own several different variant rows (including repeated self1 in a one-material project); only in an actual one-material domain may the registered singleton ID1 also name its complete known variant table. Values may be shared singletons, one per material, or one per explicitly owned variant. The SDK selects and expands these known meanings into its detached execution configuration, then checks actual `Print::apply` material parameters and routing against that expectation for every entry. Missing material IDs, duplicate ownership, out-of-range IDs, unknown/unmatched variants and unexplained option lengths fail before G-code/preview publication. Optional GUI expansion metadata is not mandatory device identity; explicit invalid metadata still fails, while resolved device enums are always checked. Compatibility-profile sets and development tables retain their declared record meaning and must remain unchanged. Geometry import and historical preview remain readable; a bad execution layout needs a normal material rebind. This does not change the desktop Core fallback or assert native preset authority for generic configurations: `nativeMaterialConfigurationVerified` remains false there. The raw `requestedConfig` is retained while `effectiveConfig` records only validated normalization.

`orca_material_variant_layout_test` covers strict selection (including a non-first internal trusted machine fixture), flattening, real Core variant gathering and the normalization guard. `material_variant_native_test.py <slicing-root> <model-dll> <new-evidence-dir> <route-json>` exercises the complete normal C ABI Model/manufacturing/slice path with native PLA/PETG temperature, fan, flow and slowdown differences; run routes `[0,1]`, `[4,5]` and `[1,0]` and compare the saved derived fan/feedrate views without rewriting raw G-code. Honor's released native machine currently resolves Direct Drive Standard and native 1.75 mm materials. The internal non-first fixture and generic 2.85 mm PE control do not expand those catalog capabilities.

`ordinary_material_execution_test.py <slicing-root> <model-dll> <new-evidence-dir> <ordinary-request.json>` replays a real public manufacturing-derived generic request. It retains a legal logical1 control with different temperature/fan/flow/diameter and nonzero PE, rejects the missing-self review counterexample and malformed variants with no executable artifacts, and checks legitimate multi-variant ownership and singleton/per-material broadcast. The companion C++ test runs the real Core gather on the old bad layout and verifies the independent postcondition rejects its material drift. Source requests, original models and raw G-code remain unchanged.

The user-confirmed APEX alias is native `honor/forge-6-head-0.4`, sourced from the repository Honor Forge profile inheritance. Six nozzles do not imply six loaded spools. Unused tools require no invented bindings. Actual material configuration is verified by the native helper; loading is an operator declaration (`filamentConfirmationSource=operator`, `actualFilamentLoadingVerified=false`). No physical verification is implied.

Only the exact SDK Honor mapped-start context on independent multihead Klipper
initializes each participating physical heater with an explicit target and wait
after `START_PRINT`. Participants come from sliced tool ordering (including actual
support/tower paths, and all sequential objects), rather than every configured
material or nozzle. The normal layer-temperature transition and user filament
G-code still run in their original order; ooze-prevention idle/standby targets
are used when nonzero standby delta enables the existing tool-change restore.
With zero delta, startup retains the actual first-layer target and wait. Single-head,
SEMM, ordinary desktop source templates and other printer contexts keep their
startup behavior. These commands do not prove real hardware reaches temperature.

The host-only `orca_honor_multihead_thermal_test <NEW_OUTPUT>` target exercises
13 real Core exports, including sparse heater/tool mapping, unused heads,
support, sequential objects, temperature transitions, ooze and user-macro
boundaries, plus zero standby delta with lower (177), higher
(235) and zero idle temperatures. In this SDK startup scope, noninitial heads
use idle/standby targets only when nonzero delta enables the existing
post-toolchange print-temperature restoration; zero delta keeps the actual
first-layer print target and wait. The existing negative-delta idle/restore
control and ordinary desktop behavior remain unchanged. These source controls
require a new frozen-source build and execution under the task's separate
resource approval; their presence is not a pass or new bundle qualification.


Honor routing emits physical `T` commands while cooling adjustments, fan configuration and pressure-equalization cross-sections remain indexed by logical material. Cooling parse/apply share the strict inverse mapping and active-material check; planner initial IDs are already logical. Pressure equalization receives the same final active logical set and initial logical ID before any layer filtering, including nonzero volumetric slope. Unmapped/inactive/invalid physical tools fail explicitly; configuration arrays are never expanded to physical positions and emitted G-code/filament_map retain their original identity. Generic valid tool semantics are unchanged, with invalid indices prevented from selecting an unrelated material.

The host `orca_cooling_tool_domain_test <new-evidence-dir>` uses the actual writer/configuration, CoolingBuffer and PressureEqualizer. It saves physical G-code, then compares derived logical analysis views across sparse T4/T5, independent support T3, permutation T1/T0, identity six tools and generic routing. Unequal fan/slowdown/bridge/support parameters and material diameters provide behavioral oracles; tests cover transitions, flush persistence, real fan/feedrate output, nonzero pressure slope and invalid/unmapped/inactive tools. This is a small Core integration regression, not device/firmware or complete manufacturing acceptance. The excluded baseline target is solely for explicitly recorded historical-Core comparison; `BuildProjectReferences=false` must never qualify a production SDK or a fixed implementation.

Enhanced `repair_mesh` is generated from the complete original enhanced implementation behind a whole-source SHA guard. The separate desktop full strict repair remains unsupported. No fake strict implementation or GUI object is linked. Run the real host regression with `python mobile-slicing/tests/color_bridge_native_test.py <fresh-slicing-build> <full-model-dll> <new-evidence-dir>`; it exercises authoritative editing/round-trip, actual G-code, sparse physical tools, independent support, negative bindings/config/source cases and repair consent. Build and runtime evidence must be evaluated for the exact candidate; these source contracts do not qualify an older Android binary or a physical print.

### Native plate skirt angle

The 3MF inspector accepts `skirt_start_angle` as an ordinary native plate option,
including the `-135` value emitted by native project export. It requires one
finite scalar in `[-180,180]`; conflicting duplicates, malformed values and
unknown directives remain output blockers on every plate, including unselected
plates. After native import, the SDK checks the actual PlateData angle against
the inspected value before applying the single or explicitly selected plate's
configuration. The unchanged source archive remains authoritative; request
configuration overrides cannot erase invalid source metadata.

`tests/skirt_angle_cases.py` provides copy-only fixtures and assertions used by
`plate_process_public_test.py` for real public-CABI import, source preservation,
explicit override, selected plates, export/reimport and effective slice config.
Its standalone unittest checks fixture preparation only, without loading an SDK.
Actual CABI/Android execution is a separate qualification step.
