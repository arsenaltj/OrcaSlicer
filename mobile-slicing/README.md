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

## Color manufacturing contract

`exportColorDocument` exports the replayed native project as authoritative world-space OBJ plus appearance and original object/volume/instance bindings. SC01 remains preview-only. Unsupported annotations or local process semantics fail explicitly. `applyColorDocument` validates the original source/config/revision and geometry/parts before applying a confirmed Model material plan to a detached native 3MF derivative.

`prepareColorManufacturingConfig` resolves the native device/material catalog, preserves native source print options, and returns complete serialized config and canonical fingerprints. Bindings explicitly separate logical `materialId`, physical `slot` and physical `tool`; logical IDs are continuous but physical positions may be sparse. Support bindings are excluded from the model color palette and route support/interface materials while retaining the current support switch. Native preset identity is the exact id/version/family, not a family alias.

The user-confirmed APEX alias is native `honor/forge-6-head-0.4`, sourced from the repository Honor Forge profile inheritance. Six nozzles do not imply six loaded spools. Unused tools require no invented bindings. Actual material configuration is verified by the native helper; loading is an operator declaration (`filamentConfirmationSource=operator`, `actualFilamentLoadingVerified=false`). No physical verification is implied.

Enhanced `repair_mesh` is generated from the complete original enhanced implementation behind a whole-source SHA guard. The separate desktop full strict repair remains unsupported. No fake strict implementation or GUI object is linked. Run the real host regression with `python mobile-slicing/tests/color_bridge_native_test.py <fresh-slicing-build> <full-model-dll> <new-evidence-dir>`; it exercises authoritative editing/round-trip, actual G-code, sparse physical tools, independent support, negative bindings/config/source cases and repair consent. Build and runtime evidence must be evaluated for the exact candidate; these source contracts do not qualify an older Android binary or a physical print.
