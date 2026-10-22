# Android arm64 native slicing build

This entry configures and builds the real `libslic3r` FFF core for Android `arm64-v8a` with NDK `28.2.13676358`. It keeps the existing STL/3MF/BBS 3MF import and material/paint facet data paths while excluding wx, Python, OpenGL, SLA, STEP, Assimp, and desktop texture/CAD implementation units.

## Reproducible entry

From the repository worktree:

```text
node mobile-slicing/build_android_arm64.cjs
```

The script uses the Visual Studio bundled Ninja when `NINJA` is unset and the pinned NDK when `ANDROID_NDK` is unset. It configures with `-DANDROID_ABI=arm64-v8a`, `-DANDROID_PLATFORM=android-24`, and 16 KiB ELF page alignment linker flags. It builds the `OrcaSlicer::Core` static archive first; when coordinator `src/orca_mobile_slicing_sdk/c_api.cpp` and `engine.cpp` exist, the same entry also defines `orca_mobile_slicing_sdk`.

Fetched dependency URLs and SHA256 pins are in `CMakeLists.txt`. Local dependency sources are referenced from `deps_src`; no desktop superbuild is used.

## Current verified progress

The configure path reaches Android Clang 19, Boost, oneTBB, Eigen 5.0.1, NLopt 2.5.0, libnoise, MCUT, qhull, and the local dependency graph. The first complete compile attempt reached local dependency compilation and currently stops at `admesh` because its upstream target does not propagate Boost headers; the mobile entry now adds `Boost::headers` to that target. Evidence logs are kept under the task artifact evidence directory.

The original first gate failed before source configuration because plain CMake could not find Ninja. The reproducible entry fixes this with the absolute bundled Ninja path.

