# Native semantic recognizers

The domain API consumes RGB8 images and returns region labels and confidence.
The MediaPipe adapter owns the six-class body mapping and the 478-landmark lip
contours; domain and GUI code do not depend on MediaPipe structs or landmark ids.
Body and face providers are registered and selected independently. The built-in
provider id for each is `mediapipe.cpu.v1`.

## Runtime preparation and integration

On Windows x64, prepare the pinned official artifacts explicitly:

```powershell
./scripts/prepare_semantic_runtime.ps1 -Destination ./semantic-runtime
./scripts/prepare_semantic_runtime.ps1 -Destination ./semantic-runtime -VerifyOnly
```

Use `-Offline` to require an already complete, verified download cache. Preparation
uses PowerShell and .NET ZIP support; the application uses the native C ABI and
does not start Python. The manifest contains the source commit, URLs, sizes,
SHA256 hashes and the wheel's complete LICENSE/NOTICE entries. Both model cards
and their Apache-2.0 license provenance are recorded in `MODEL_LICENSES.md`.

Include `cmake/OrcaSemanticRuntime.cmake`, call
`orca_configure_semantic_recognizers(target)` for the adapter's target, and call
`orca_stage_semantic_runtime(executable)` in the executable's CMake directory.
Set `ORCA_SEMANTIC_RUNTIME_DIR` to the prepared directory to verify and stage the
runtime under `<exe>/ai/portrait_semantics`. No build step downloads a model.
The directory is separate from the sidecar resources. An empty staging path is
supported; missing runtime files produce an unavailable prediction.

`ORCA_ENABLE_MEDIAPIPE_NATIVE` defaults on for Windows x64 and off elsewhere.
Setting it off compiles the same factories with unavailable provider objects.
Windows ARM64 cannot use the pinned x64 DLL. With native enabled, the adapter
verifies the DLL and selected model hash before creating its CPU task. Missing
files, wrong hashes, invalid inputs and ordinary C API errors return an error
with no accepted semantic labels, allowing the caller to retain ordinary color
matching. Cancellation is checked before/after native calls and during mask
processing; an in-progress native image call itself is synchronous.

The prepared `providers.json` contains `body_provider`, `face_provider`, and
`boundary_provider`. The boundary provider defaults to `none`; a missing,
damaged, or unknown optional boundary provider falls back to the body/face result.
Register a replacement factory under a new id, then select that id independently.
Changing a model whose output layout differs requires its own adapter: overwriting
a pinned model file is intentionally rejected by SHA256 verification. Provider
identity includes model hashes and interpretation-policy versions for cache keys.

## Offline boundary-model A/B

`tools/ai/semantic_boundary_ab.py` compares MobileSAM and EfficientSAM-Tiny with
the same RGB ROI and positive-skin/negative-hair prompts. It saves the unrounded
soft mask, overlay, changed-pixel count, optional contour error against a frozen
reference, model SHA256 values, CPU timings, and peak process memory. The tool is
evaluation-only and requires Python ONNX Runtime; Orca does not load Python.

EfficientSAM accepts either its combined model or the official split encoder and
decoder; split mode records encoding and decoding separately. MobileSAM's upstream
export script covers only the prompt encoder and mask decoder, so the evaluation
encoder is derived from the fixed TinyViT checkpoint with
`tools/ai/export_mobile_sam_encoder.py`. Run providers in separate processes when
comparing peak working-set values because the operating-system counter is a
process-lifetime peak.

The source/weight audit state is recorded in
`tools/ai/semantic_boundary_models.json`. Entries with a missing revision, URL,
size, digest, LICENSE, or NOTICE remain blocked from packaging. No ONNX boundary
provider is registered in the application until a candidate passes the four-model
visual and performance gates.

## Interpretation limits and network behavior

The body model is trained for people in 2D images. Rendered objects, animals,
occluded faces, backs of heads and clothing may be ambiguous. Its pixel output is
not by itself proof that a mesh region has a particular meaning. The pipeline
must retain the confidence, visibility and person gates when projecting labels.
Face lip confidence is a mask reliability policy after the detection/presence
gates, not a calibrated per-pixel neural probability. Boundary pixels abstain.

The official [v1.0.0 privacy notice](https://github.com/google-ai-edge/mediapipe/blob/6d31f1ebc3284db74d211d62bdc4f0a0c29ea120/README.md#privacy-notice)
states that input images are processed on the device and are not sent to Google,
and that Tasks sends performance/utilization metrics. The pinned Windows DLL
imports WinInet and contains `https://play.googleapis.com/log`. The public C API
does not expose a metrics-disable switch. Local inference therefore does not
mean the official binary makes no network attempts.

In the 2026-09-15 native ABI verification, two local 750x820 portrait renders both
produced six body masks and one 478-point face while a test-process-only hook made
`HttpSendRequestA` fail with `ERROR_INTERNET_CANNOT_CONNECT`. Two HTTP sends were
actually rejected per process. This shows those inference calls do not depend on
successful HTTP transmission; it is not a whole-machine offline or zero-network
certification. No production DLL or system network settings were changed.
Separately forcing local `InternetOpenA` session allocation to fail terminated
the official library with a fatal check; such native fatal errors are outside the
adapter's C++ exception fallback. A no-telemetry upstream/source build remains a
separate runtime-provider option, not a patch applied to this binary.
