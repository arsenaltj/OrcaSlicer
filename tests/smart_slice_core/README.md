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
`smart_slice_bridge.cpp` and `project_support.cpp`. A read-only source-matched
273fbf5 Core build supplies native dependency headers/libraries. Generate a config:

```text
python tests/smart_slice_core/configure_bridge_reference.py --reference-build <273-build> --output .tmp/recommendation-core/bridge-reference.cmake
cmake -S tests/smart_slice_core -B .tmp/recommendation-core -DORCA_BRIDGE_REFERENCE_CONFIG=<absolute-config-path>
cmake --build .tmp/recommendation-core --config Release --target smart_slice_bridge_tests --parallel 1
```

Run the executable with the reference build's Release directory and dependency DLL
directories on the process-local PATH. This checks the real three-plan JSON,
quality dispatch, transformed geometry and error propagation. These are development
source regressions; they do not qualify a new SDK bundle, C ABI release, Android
runtime, or APK. No old build directory is written by this target.
