# OrcaSlicer::Core target

`OrcaSlicer::Core` is the cross-platform CMake dependency name for the slicer core.
The desktop and Android configure trees build their own platform library from the
same `src/libslic3r/core_sources.cmake` source inventory, then expose that library
through the same target name:

```text
desktop:  orca_slicer_core  -> OrcaSlicer::Core
Android:  orca_slicer_core  -> OrcaSlicer::Core
```

The desktop configure also keeps `libslic3r` as an alias to the implementation
target. Existing tests and upstream build fragments can continue to use the old
target name while new product and SDK targets use `OrcaSlicer::Core`. The old
Android-only target name `orca_mobile_core` is removed.

The shared manifest contains the desktop `libslic3r` source inventory. The Android
entry filters desktop-only units such as SLA, CAD, Assimp, texture painting and
GUI-facing helpers, then adds the mobile support units required by the Android
configuration. This keeps platform dependency differences explicit without
maintaining a second copy of the common source list.

## Upstream synchronization

The target rename is intentionally isolated to CMake target definitions and link
consumers. The `libslic3r` compatibility alias reduces merge pressure on existing
upstream test and utility targets. New source additions should be made in
`src/libslic3r/core_sources.cmake`; the desktop and mobile configure files should
only contain platform filters and dependency wiring. Android-specific behavior in
`libslic3r` remains guarded by `ORCA_MOBILE_ANDROID` until it is moved behind a
platform adapter.
