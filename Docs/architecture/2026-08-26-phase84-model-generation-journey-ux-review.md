# Phase 84: Model Generation Journey UX Review

## Outcome

The model-generation flow is suitable for a small, observed internal Beta after the Phase 83 recovery fixes and this UX pass. The UI now distinguishes user decisions from technical evidence, describes paid 3D generation before submission, shows measured local OBJ load performance, exposes history actions, and records only lifecycle facts that OrcaSlicer can prove.

## Journey changes

- Image preview stages use task-oriented labels. Minimum-feature and changed-pixel metrics live in a collapsed processing-details section.
- Paid 3D confirmation identifies one paid task, Provider-owned pricing, an expected 3–10 minute duration, target triangle count, and the fact that stopping locally may not cancel remote work or billing.
- Generated and historical OBJ parsing is timed with `steady_clock`. The preview shows the measured local duration and warns at 300,000 triangles or above.
- History cards have visible Load and Delete Local actions. Deletion requires confirmation, validates that targets are descendants of `generated_models`, and does not imply remote cancellation.
- Compatible local metadata schema 3 records parse duration, import time, whether auto-slicing was requested, and an explicit tester-supplied print outcome.
- Import UI says “slice requested” and directs the tester to verify G-code elsewhere. It never infers slicing or print success from import alone.

## Verification evidence

- `python -m unittest discover -s tools/ai -p "test_*.py"`: 280 passed.
- `ctest --test-dir build-ai-tests/tests/slic3rutils -C Release --output-on-failure`: 23 passed.
- MSVC Release target `OrcaSlicer`: passed; only the pre-existing `LNK4098` warning was emitted.
- GUI at 1342 × 982: processing details expanded without clipping; history buttons remained visible; deletion confirmation was opened and declined without removing any model.
- Existing 293,028-triangle OBJ: displayed a measured local parse time of 0.73 seconds and did not trigger the 300,000-triangle warning.
- `git diff --check`: passed; only line-ending conversion notices were printed.

## Package

- Archive: `output/packages/OrcaSlicer-AI-Windows-x64-20260826-journey-beta.zip`
- Archive SHA256: `DF94912298BA86E609DA32860BFFBA882960587AD8B532679446E846660326B7`
- Packaged `OrcaSlicer.dll` SHA256: `075DC6AF04B0E0F531BB6331FFEF42596EE746FA21FA6D85C35CFA95BA34AC5D`
- Packaged Sidecar SHA256: `9282A5BBECB212062F21EEAE0B3A7B7EFAE353942C154EDEE0576BA4D0EA9466`
- Archive inventory: 15,658 entries; required launcher, readme, DLL, and Sidecar paths verified.

## Remaining Beta evidence to collect

- Run designated high-face fixtures at 300k, 500k, and 1m target quality on representative internal machines; the application now captures the measurements but this change does not fabricate benchmark results.
- Complete real prints and record “success” or “issue” in model history. Until a printer-status integration exists, the tester is the authoritative source for this outcome.
- Provider price remains intentionally dynamic. OrcaSlicer discloses billing ownership but cannot show an exact amount without a Provider quote API.

## Recommendation

Proceed with a small observed internal Beta. Use designated Provider accounts, keep paid task IDs for incident review, and require testers to verify G-code and record the physical print outcome before treating a model as closed-loop successful.
