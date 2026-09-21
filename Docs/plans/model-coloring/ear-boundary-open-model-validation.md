# Ear/hair boundary open-model validation

## Identity and scope

- Baseline branch: `codex/team/maintenance`
- Baseline commit: `78c577a616170ef79d88dc2ecde838542cad9509`
- Experiment branch: `codex/exp/ear-boundary-refinement`
- Product boundary provider: `none` (the experiment does not change R4-C9l color assignments)
- Pipeline version: `orca.semantic-coloring/v18`
- Fixed inputs: the R4-C9l 750x820 source renders for Fang Fei, Liu Yifei, the couple, and the animal.

This round validates whether promptable open models provide useful local boundary evidence. It does not claim that an ONNX model is licensed or fast enough for product packaging, and it does not replace user review of the four six-view results.

## Implemented

1. `IBoundaryRefiner` is independent of body and face recognition. Requests contain RGB, coarse labels and confidence, positive/negative prompts, target labels, ROI, and cancellation. Results retain a soft mask, confidence, provider identity, and processing version.
2. Missing, unknown, invalid, canceled, or low-confidence boundary output keeps the R4-C9l result. Ear refinement is restricted to the requested ROI, the positive-seed-connected component, and existing Hair/FaceSkin/Unknown pixels. Lips, mouth, sclera, iris, and eyebrows are protected.
3. Boundary provider identity is included in cache signatures. A provider change invalidates cached recognition, and stale tasks keep the existing coordinator generation guard.
4. Sparse midpoint subfaces support depth 3 (64 leaves). Budget accounting charges every ancestor split and retains the original face id, 20 percent triangle limit, and 200,000 triangle limit.
5. Semantic trial preview uses the existing default 4x OpenGL MSAA surface while keeping dithering disabled. This affects display only; import and print boundaries still use the persisted subface tree.
6. The offline A/B tool supports MobileSAM encoder/decoder and EfficientSAM-Tiny combined or split ONNX. It writes float soft masks, PNG masks, overlays, hashes, timings, peak process memory, changed pixels, and optional reference-contour metrics.

## Fixed model identities

| Candidate | Source revision | Evaluation artifacts | Evaluation status |
|---|---|---|---|
| MobileSAM | `f706ad9c4eb7f219c00d9050e46328518ffb65d2` | Encoder `83398d...ace32`; decoder `43c765...241cf` | Internal A/B only; derived ONNX, product blocked pending full license/notice review |
| EfficientSAM-Tiny | `d525f622e6f640acf5a0fc37c7ca1f243da5bde0` | Encoder `84ed46...e0951`; decoder `a62f8f...c0b11` | Internal A/B only; product blocked pending full license/notice review |

Both repositories contain an Apache-2.0 root LICENSE. Exact sizes, full SHA256 values, derivation tools, and audit states are in `tools/ai/semantic_boundary_models.json`.

## Local A/B results

The red overlay is the existing coarse FaceSkin boundary; green is the prompted candidate boundary. Changed pixels compare masks inside the ROI and are not an accuracy score.

| Case | ROI pixels | MobileSAM candidate / changed | EfficientSAM candidate / changed | Current reading |
|---|---:|---:|---:|---|
| Liu Yifei left ear | 7,200 | 1,888 / 994 | 7,099 / 4,977 | MobileSAM closes around the ear and front hair strand; EfficientSAM accepts almost the entire ROI |
| Fang Fei right ear | 12,000 | 2,261 / 1,336 | 11,887 / 8,814 | MobileSAM follows the exposed outer ear; EfficientSAM accepts almost the entire ROI |
| Couple female ear | 11,500 | 2,200 / 1,508 | 11,479 / 8,391 | MobileSAM gives useful outer-ear evidence; EfficientSAM mostly follows the ROI exterior |
| Couple male ear | 13,750 | 377 / 5,920 | 13,660 / 7,399 | MobileSAM selects only a small inner-ear component; this view must abstain |
| Animal | no ROI | no inference / zero automatic refinement | no inference / zero automatic refinement | Person/face gate retains R4-C9l |

Artifacts are under `.tmp/semantic-boundary-ab/*-two-model-ab/`. Each case contains `result-card.json`, both overlays, soft-mask PNGs, and unrounded `.npy` masks.

No frozen manual ear contour is available in this experiment package. Consequently `contour_p95_px` and maximum contour error are intentionally null. Product integration cannot pass the P95 <= 1 pixel gate until R0 reference and uncertainty masks are added.

## CPU measurements

Liu Yifei's 90x80 ROI was measured in a fresh process seven times after warmup-equivalent repeated launches, four CPU threads:

| Candidate | Load median / max | Encode median / max | Decode median / max | Inference median / max | Peak working set median / max |
|---|---:|---:|---:|---:|---:|
| MobileSAM | 354.0 / 430.8 ms | 422.3 / 439.3 ms | 35.9 / 39.2 ms | 458.1 / 477.2 ms | 538.0 / 543.6 MiB |
| EfficientSAM-Tiny | 380.7 / 393.8 ms | 883.9 / 897.7 ms | 31.9 / 32.7 ms | 916.6 / 926.3 ms | 627.8 / 629.9 MiB |

These are model-process peaks, not incremental application memory. They are still too large to assume the final whole-model time and memory gates will pass. Encoding once per ROI prevents repeated decoder cost, but multiple face views remain expensive.

## Decision state

- MobileSAM is the only candidate worth user visual review. It improves three exposed-ear examples and correctly produces a rejectable candidate on the failed male view.
- EfficientSAM-Tiny fails the current visual shape test and is retained only as a recorded comparison.
- Neither model is enabled or packaged in the product. The product default remains `boundary_provider: none`.
- A native provider is deferred until user review of the overlays and a frozen reference contour. If MobileSAM is rejected or exceeds final performance limits, continue with the planned MobileNetV3-Small + LR-ASPP specialized model.

## Verification

- Release `OrcaSlicer.dll` and `orca-slicer.exe` compile and link successfully.
- Semantic/boundary/model-preview tests: 92 cases, 1,001 assertions passed.
- Triangle subface tests: 2 cases, 14 assertions passed.
- Offline A/B Python tests: 6 passed.
- AI integration lock verification: `ok: true`.
- The staged MediaPipe runtime under the Release directory passes manifest, size, and SHA256 verification.

The visual gate remains pending user review. Automated checks do not mark the ear/hair issue resolved.

## Packaged-preview user baseline (2026-09-17)

The user reviewed `OrcaSlicer_AI_EarBoundary_R4_Preview_20260917_x64.exe` and supplied a two-page report. Liu Yifei and Fang Fei each have five overall views plus local face/ear crops. The couple section contains only its heading, and the animal is absent.

The report confirms that the `boundary_provider: none` baseline still has Liu Yifei left-ear/hair mixing, missed ear-side hair strands, eyebrow expansion, Fang Fei ear mottling, and material-boundary jaggies around facial features and hair. The packaged 4x MSAA is insufficient for triangle-defined material boundaries. Depth-3 refinement and MobileSAM cannot be visually judged from this package because no boundary provider emits soft contour evidence.

The full Chinese test record and disposition are in `ear-boundary-preview-user-test-2026-09-17.md`. No visual issue is closed by this report; it freezes the baseline for the first product package that actually enables the MobileSAM provider.
