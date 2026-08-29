# Orca Nightly upstream sync: db29f570

## Fixed source

- Official repository: `https://github.com/OrcaSlicer/OrcaSlicer.git`
- Release channel: standard `nightly-builds` build from `main`
- Annotated tag object observed on 2026-08-29: `6e56d08eb50a3b26300ad49ee30de050c0f48f93`
- Peeled source commit: `db29f570bd2f77742ab04e0bb8f0aa55237bd70a`
- Previous locked upstream: `6fdd4945c19348cc5fc9ed9ae2f26f22a778786b`
- Integration method: explicit non-fast-forward merge; no history rewrite

The moving Nightly tag was resolved once. This integration does not follow later tag movements automatically.

## Upstream delta

The fixed target adds three official commits:

1. `6d1584844e` fixes UTF-8 STEP/OBJ part-name handling and adds regression fixtures.
2. `cc390f11ee` builds dependencies missing from the main CMake configure path and updates Clang-CL dependency handling.
3. `db29f570bd` clears warning-producing pessimizing moves and impossible null checks.

The delta changes 36 upstream paths. It does not change the AI Sidecar protocol, provider policy, model-generation or smart-slicing feature contracts, product port, 3MF schema, or printer/profile formats.

## Conflict resolution

`git merge-tree --write-tree --messages` predicted, and the real merge produced, exactly two textual conflicts:

- `build_release_vs.bat`: composed upstream `-l` Clang-CL support with the existing explicit AI installer/cache tuple. Default non-AI behavior remains unchanged.
- `deps/wxWidgets/wxWidgets.cmake`: accepted upstream removal of the obsolete patch command. The upstream deletion of `deps/wxWidgets/0001-Clang-CL-fix.patch` is retained.

Four other overlapping paths (`MeshBoolean.cpp`, `Model.cpp`, `Plater.cpp`, and `tests/libslic3r/CMakeLists.txt`) merged automatically. `Plater.cpp` remains a thin bridge to `SmartSlicingFeatureHost`; no feature orchestration was copied back into the shared file.

## Preserved product contracts

- Model-generation accepted SHA: `5081891766e64a35ad3ff586d722499a80e5e8d6`
- Smart-slicing accepted SHA: `1c163d68906e287b946b40a975feb3bfd9aab68d`
- Product Sidecar port: 18764
- Sidecar/protocol: v8 / v2
- Orca defaults, 3MF/profile compatibility, output directories, and safe offline degradation: unchanged
- Credentials and paid provider calls: none

## Verification results

| Check | Result |
|---|---|
| Merge parent and ancestry | PASS — first parent `813b068d77030f9f177bb03a284c2fc6137d74ae`; second parent and ancestor `db29f570bd2f77742ab04e0bb8f0aa55237bd70a` |
| Integration guardrails | PASS — focused suite 34/34; pre-commit structural verifier `ok: true` with Git-receipt check deferred until commit |
| AI Python suite | PASS — 403/403 in 69.450s; provider traffic was mocked/offline and no paid task was submitted |
| Native Catch2 suites | PASS — STEP 3 cases/24 assertions; libslic3r 304 cases/58,078 assertions; slic3rutils 190 cases (157 pass, 33 conditional skip)/1,231 assertions |
| Dependency and main configure | PASS — both configure/generate; only pre-existing deprecation and CMake policy warnings |
| Windows Release build and bundled runtime | PASS — GUI plus both native suites; isolated Python 3.12.13/Pillow 12.2.0 self-check passed |
| Isolated GUI Chinese/navigation/wizard smoke | PASS — Chinese navigation, first-run welcome wizard, 3D generation workflow, diagnostics and safe offline state are visible |
| Whitespace and credential scan | PASS — no unmerged paths, staged diff clean, no provider credential pattern detected |
