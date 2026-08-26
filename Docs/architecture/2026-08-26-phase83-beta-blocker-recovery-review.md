# Phase 83: Model Generation Beta Blocker Recovery Review

## Outcome

The first internal-Beta blocker set is fixed. Ready image jobs no longer stall because input, preview, and artifact downloads cancel one another; restored image input is now a supported Sidecar route; local model loading no longer sends a misleading remote stop; AI visual review requires explicit paid-use confirmation; and an unavailable Sidecar can be detected again without restarting OrcaSlicer.

## Behavior changes

- `AIModelGenerationClient` keeps control requests serialized while retaining file downloads independently. A reset or user cancellation still cancels both categories.
- `GET /v1/orcaslicer/model-jobs/{id}/input` is accepted by the same validated job router as the other task assets.
- A ready artifact download is presented as “取消加载”. Cancelling preserves the ready job and exposes “重新加载 3D 模型”; no `/stop` request is sent for this local-only action.
- “AI 视觉复核” displays a Yes/No dialog explaining that five views are sent to the AI service and may consume API credit.
- When model generation is unavailable, the primary footer action is “重新检测服务”. It resets and reuses `MainFrame` service discovery.

## Verification evidence

- `python -m unittest discover -s tools/ai -p "test_*.py"`: 280 passed.
- `ctest --test-dir build-ai-tests/tests/slic3rutils -C Release --output-on-failure`: 23 passed.
- MSVC Release target `OrcaSlicer`: passed; only the pre-existing `LNK4098` warning was emitted.
- `git diff --check`: passed; only line-ending conversion notices were printed.
- GUI cold-start recovery against an older Sidecar was intentionally exercised. `/input` returned 404, while the independent artifact and preview downloads completed and the model became importable instead of remaining at 94/95%.
- GUI service recovery was exercised with an unavailable endpoint, followed by a valid local Sidecar. Clicking “重新检测服务” restored generation controls without restarting the app.
- GUI visual review confirmation was opened and declined; no paid Provider task was created.

## Package

- Archive: `output/packages/OrcaSlicer-AI-Windows-x64-20260826-beta-blockers.zip`
- Archive SHA256: `CEB368184E927DEE77FD2BEE205357F0BB983E9A9998CE6B9EBBA72861B88979`
- Packaged `OrcaSlicer.dll` SHA256: `DD5C5039F008A1474E44F9AA9844C3ED2C04EB346CA60D4DE08D8C33DCB6E13C`
- Packaged Sidecar SHA256: `9282A5BBECB212062F21EEAE0B3A7B7EFAE353942C154EDEE0576BA4D0EA9466`

The packaged DLL matches the final Release staging DLL, and the packaged Sidecar matches the reviewed source file.

## Internal-Beta recommendation

This build is suitable for a small, observed internal Beta. Keep real paid generation to designated test accounts, collect job IDs and Sidecar logs for failures, and explicitly record very large OBJ load times. OBJ parsing remains synchronous after download begins, so unusually large models may still make the window temporarily unresponsive even though download cancellation and recovery semantics are now correct.
