# Smart slice core regressions

This standalone CMake target runs the same `test_smart_slice_engine.cpp` cases as
`slic3rutils_tests`, without the GUI, Python, or full slicing build. It compiles
this checkout's engine and DTO implementation and the bundled Catch amalgamation.

```text
cmake -S tests/smart_slice_core -B .tmp/recommendation-core
cmake --build .tmp/recommendation-core --config Release --parallel 1
ctest --test-dir .tmp/recommendation-core -C Release --output-on-failure
```

On Windows, an optional native bridge regression compiles this checkout's actual
`smart_slice_bridge.cpp` and `project_support.cpp`. This checkout's own normal frozen-source Core build supplies dependency headers/libraries.
Record its source, header, flags, dependencies and command identities before
reusing any object; do not borrow another checkout's Core. Generate a config:

```text
python tests/smart_slice_core/configure_bridge_reference.py --reference-build <own-frozen-source-Slicing-build> --output .tmp/recommendation-core/bridge-reference.cmake
cmake -S tests/smart_slice_core -B .tmp/recommendation-core -DORCA_BRIDGE_REFERENCE_CONFIG=<absolute-config-path>
cmake --build .tmp/recommendation-core --config Release --target smart_slice_bridge_tests --parallel 1
```

Run the executable with the reference build's Release directory and dependency DLL
directories on the process-local PATH. This checks the real three-plan JSON,
quality dispatch, transformed geometry and error propagation. These are development
source regressions; they do not qualify a new SDK bundle, C ABI release, Android
runtime, or APK. No old build directory is written by this target.

The default build also creates `smart_slice_old_new`. It reads five exact blobs
from e8d8dea3b2cb5705c62a41ac7a1a2273cec6539a, changes only their namespace and corresponding local include paths,
and compiles the old and current implementations into the same test process.
CTest `SmartSliceOldNew` compares the same profile and bed/floating cubes,
actual closed sloped geometry, a 0.42 slope metric input, a narrow layer interval,
open-mesh baseline support and an empty layer intersection. Its stdout records
actual old/new layer, support, support type, angle and adaptive request values.
A source repository containing the pinned commit is required at configure time.

The SDK producer also builds `orca_plate_process_test` for the shared strict
11-key native parser. `mobile-slicing/tests/plate_process_public_test.py DLL
NEW_OUTPUT` tests the real public export/import, Native reader and scanner,
source-profile precedence, multi-plate selection, invalid unselected plate,
explicit overrides, validate/slice, unknown sessions/curves and source SHA.
`color_bridge_native_test.py SLICING_BUILD MODEL_DLL NEW_OUTPUT` retains its
existing public Model/Slicing regressions and adds exact T4/T5+supportT3 and
T0..T4+supportT5 routes. The five-part source is the immutable original 76-face
native 3MF (SHA256 afe4e16c3d3c1e409871dffed0c15c3853cf09621c4bca05351de4a8bb843a6f),
now stored under mobile-slicing/tests/data. Each route saves native material
configuration, physical tool/thermal/first extrusion receipts, G-code and preview
hashes, positive support volume and preview support paths. Positive E alone is
not treated as net material consumption or support proof.

These changes keep the C headers, exports, ABI version and three-plan wire
contract. A horizontal cube no longer requests adaptive layering from its flat
cap; real sloped geometry gets finer balanced/speed layers, and severe overhangs
use normal support. Adaptive is a request requiring Native capability, not an
executed layer curve. Only the exact 11 plate process keys are consumed, in
source-global < selected-plate < explicit-request order. Malformed source on any
plate remains blocked before overrides. Workbench sessions and curves remain
unsupported. New normal producers and independent review/QA are required;
these source tests do not restore any stopped GLB/resource/safety workflow.

Plate process physical validation uses the Native `DynamicPrintConfig::validate`
result against the source profile, before request overrides. Only errors for
options actually carried by that plate are mapped to the new source guard;
missing source nozzle data is not fabricated from defaults as a physical profile.
All source plates are checked, including unselected plates. Native layer positivity
and the known source common layer interval remain required.

Reserved process/session keys are allowed only as direct plate metadata in
ModelSettings. They cannot alter process config through SliceInfo statistics or
model-instance metadata. Ordinary statistics remain accepted. Sessions and curves
remain unsupported in the SDK even in their formal container. The public regression
keeps wrong-container negatives, ordinary statistics, same-handle recovery, and
source-versus-effective override and export/reimport checks. The optional source-
matched bridge target also checks that real source support=true reaches all three
plans on an open mesh that has no independent geometric overhang evidence.
