# 3D Workbench Review

Updated 2026-09-29, acceptance R138. The runnable application is R138 on
`codex/feat/post-generation-3d-workbench`, with preserved uncommitted Beauty work.
R135 adds a fresh full `slic3rutils` run, R138 adds the geometry-control backend
wiring and a clean full-suite rerun; R136
records the validation ownership and GUI-channel retry; overall
visual acceptance is still pending. The repository handoff rules assign the
final visual and main-window experience check to the user, so this report does
not convert agent-run screenshots into user acceptance.

## Run

Executable: `D:/TEST/OrcaSlicer-figma-3d-workbench/build-figma-3d/runtime-r138/orca-slicer.exe`

Validated isolated profile: `D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/validation-profile-r138`

```powershell
& 'D:/TEST/OrcaSlicer-figma-3d-workbench/build-figma-3d/runtime-r138/orca-slicer.exe' '--datadir=D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/validation-profile-r138'
```

Route: 3D Generation -> History -> first Load -> 3D Beauty Workbench.
The first asset is R128's genuine accepted version with a pink recolored patch.
The previous blue R89 version remains in history.
The left Beauty entry opens the editing workspace. Loading existing history
does not submit a paid generation request.

## R135 Verification

The current Release test binary completed with 592 test cases: 559 passed and
33 skipped because the bundled Python or NumPy runtime was unavailable in the
test process. All 25,197 executed assertions passed. The skipped cases are
environment-gated plugin/binding checks, not Beauty or workbench failures.

`python scripts/verify_ai_integration.py --json` returned `ok: true` with no
errors, and `git diff --check` returned success with only the repository's
existing LF/CRLF conversion warnings.

The Windows Computer Use channel was retried after a session reset in R136 but
still returned `Codex auth token is unavailable`. R134's geometry control
screenshot and the earlier multi-size GUI evidence therefore remain the latest
visual evidence; no new user visual acceptance is claimed.

## Current Screens

Default, 1544 x 981 / 200%, final R133 history check:

![Current default workspace](D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/thumbnail-delivery-r133/r133-final-default.jpg)

Beauty, same runtime and size:

![Current Beauty workspace](D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/thumbnail-delivery-r133/r133-beauty-selection.jpg)

References: [Figma default](D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/figma-primary-r88.png),
[Figma Beauty](D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/figma-beauty-r128-reference.png).
The Figma frames are 1920 x 1200; design pixels map to 0.75 DIP.

R134 adds the four geometry controls visible in the Figma Beauty floating
surface: `减面强度`, `平滑迭代`, `自动补洞` and `保留硬边`. They are visibly
disabled with a backend-support tooltip; no geometry or metadata behavior is
claimed. The actual R134 screen is saved at
`D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/geometry-controls-r134/r134-geometry-controls.jpg`.
Screenshot SHA256: `378A2183D157BA2ECFD7D33293F5D63552FE8134C6F0F9BC8D24F124C4981AC4`.
Compare workspace controls and proportions with that mapping, not the different
sample model, camera angle or full-window pixel dimensions.

## Reference Differences

| Area | Current implementation and comparison limit |
| --- | --- |
| Application chrome | Orca tabs and native title bar retained. Only the post-generation workspace uses the new theme. |
| Rails and palette | Reference colors, icon assets, 280 DIP rails, palette controls and yellow print action implemented. R124 maps rounded inspector clipping and title/header spacing. Native text metrics and opaque surfaces still differ from Figma. |
| Viewport | Existing live ModelPreview3D. Ground grid is visible in the inclined view and edge-on in front view. The sample model and camera are not fixed to the Figma illustration. |
| Floating Beauty parameters | Native continuous dark surface; real operation-dependent strength/softness and original/lighting controls. Figma reduction, fill and hard-edge controls have no corresponding implemented backend. Existing operations keep their actual names and meanings under the approved first-version scope. |
| Right Beauty inspector | Contains existing operation, selection, color and semantic controls instead of the reference's mostly empty inspector. The expanded inspector is necessary to retain existing functionality. |
| History | Existing reference images or placeholders, real timestamps and candidate/accepted/current marks. Figma's repeated 3D sample thumbnails are replaced by the approved 2D asset thumbnails. Search, paging, Load and Download use existing assets. |
| Bottom actions | Existing candidate preview/before/accept/discard/undo/redo remain visible and fixed as required by the implementation plan. These actions add content to the reference composition. |
| Check and slicing | Real model/service availability and project values. No Figma sample face counts, repair verdicts, output estimates or print time are fabricated. Import does not implicitly slice or change presets. |
| Base | Disabled entry under the approved first-version scope. No post-generation base algorithm. |

These are recorded differences, not user-approved visual deviations. Exact
overall reproduction is not marked complete.

## Acceptance

| Gate | Evidence |
| --- | --- |
| Current layout | R125/R126/R127 on R124: all three plan sizes at 100%, 125% and 150% passed layout/function checks, including menus, expanded selection and narrow scrolling. R124: wide and exact 1280 x 800 at 200%. Glyph fidelity at 125%/150% is unresolved. |
| Semantic cancel/retry | R123 on R121: active cached request canceled at visible 0%; roles stayed unavailable, retry succeeded, original colors and accepted model restored. Uncached recognizer subprocess interruption not exercised. |
| Model edits and versions | R128 on R124: genuine pink recolor candidate/blue before view, history/import locks, accept and automatic current-card selection, blue undo/pink redo, white-candidate discard restoring pink, normal restart and ordinary History restoration. R19/R20 deformation; R24 cleanup/repair; R113/R115 softness and discard retain their provenance. |
| Prepare | R129 on R124: latest pink accepted model imported through Go print and existing color matching; printer, six physical filament presets, process identity and 27 common visible quality fields unchanged. Slice/G-code still waiting for manual action. Non-manifold mesh and out-of-area prime tower prevent print-readiness acceptance. R98 explicit slicing retains separate provenance. |
| Load failure recovery | R131: malformed upload/recolor/before/discard recovery. R132: retained pink model and 10,367 selected faces; recolor processing/candidate/discard succeeds, and ordinary result/image navigation no longer shows the stale invalid-vertex error. Reentry retains selection; default restores accepted/current and import/history. |
| Thumbnail delivery | R133 uses the tested receiver on the actual UI path: delayed obsolete images and obsolete failures cannot overwrite replacement cards; removed targets are ignored; RGB/alpha ownership and current placeholder fallback are tested. Main window: ordinary History, all three rail pages and Beauty return passed. This does not exhaustively test all worker schedules. |
| Code tests | R84 full suite: 583 passed, 5 Python-binding skips, zero failures. R102 focused: 72 cases / 7667 assertions. R130 transaction name filters: 9 cases / 63 assertions. R133 presentation: 28 cases / 402 assertions in random order. R135 current Release run: 592 cases / 559 passed / 33 environment skips / 25,197 assertions passed. Integration and diff checks passed. |
| Outstanding | User visual acceptance; 125%/150% glyph distortion and the R125 startup/display size discrepancy (not reproduced in R127); controlled service-outage recovery after the recorded tool rejection; independent validation receipt while its registry is absent. The four Figma geometry controls remain intentionally disabled because no texture-preserving reduction/fill/hard-edge backend is available. |

Detailed provenance: [implementation and acceptance report](D:/TEST/OrcaSlicer-figma-3d-workbench/Docs/plans/2026-09-27-figma-3d-workbench.md).
R124 evidence: `D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/inspector-surface-r124`.
R125 size evidence: `D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/inspector-sizes-r125`.
R126 DPI evidence: `D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/current-dpi-r126`.
R127 narrow DPI evidence: `D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/narrow-dpi-r127`.
R128 transactions/restart evidence: `D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/current-transactions-r128`.
R129 Prepare evidence: `D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/current-prepare-r129`.
R130/R131 recovery evidence: `D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/current-load-error-r130`.
R131 complete source/runtime/evidence snapshot: `D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/current-load-error-r131-snapshot/manifest.json`.
R132 navigation/recovery evidence: `D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/image-navigation-r132`.
R132 complete source/runtime/evidence snapshot: `D:/TEST/OrcaSlicer-figma-3d-workbench/.tmp/figma-workbench/acceptance/image-navigation-r132-snapshot/manifest.json`.
R133 history evidence and complete snapshot: `acceptance/thumbnail-delivery-r133/` and `acceptance/thumbnail-delivery-r133-snapshot/manifest.json` under the same acceptance root.
R134 geometry-control evidence: `acceptance/geometry-controls-r134/`.

The first R125 1440 attempt and two homepage retries unexpectedly shrank.
Their screenshots are retained as failed-size evidence. After restoring 200%
and selecting 100% again in Windows Settings, a fresh instance completed the
entire 1440 route at stable exact dimensions; the 1280 route also stayed exact.
The cause is unresolved. Windows is restored to 200% and the isolated profile
to its original wide geometry.

R126 confirms functional operation at exact 1920 x 1080 / 125% and 150%.
Text distortion is also visible in Windows Settings and the ordinary Orca
page. Its cause is unknown; this does not establish visual fidelity. Windows
and the isolated profile are restored again after four normal process exits.

R127 closes the remaining 1440 x 900 and 1280 x 800 layout/function checks
at 125% and 150%. Complete size coverage does not resolve text fidelity or
establish user visual acceptance. Its four instances also exited normally;
Windows and the isolated profile are restored to the same original settings.

R128 made no application change and verified 53 source/asset hashes and six
original model hashes against R124. The isolated profile gained one genuine
accepted model, SHA256 `84CA348A3F6FA5AB3914D8BD5B3C34BFF44475F9B17D2F6C744F83BB48FD19AC`.
The top detailed acceptance table now reflects current size/DPI coverage.

R129 imported the latest accepted pink GLB through the actual main window.
Prepare reports 963,940 triangles, 55,114 non-manifold edges and a partially
out-of-area prime tower. This verifies the handoff, not printable geometry.
The visible configuration comparison is limited to captured fields and preset
identities; it is not an exhaustive in-memory configuration comparison.
Returning to Beauty retained 10,367 selected faces and zero protected faces.
An optional Save As attempt was canceled because the automation focus report
did not identify the filename field reliably; no R129 project file was saved at that point. Before replacing the runtime for R130, actual Save As preserved it as `acceptance/current-load-error-r130/r129-prepare-preserved.3mf` (15,763,072 bytes).
Current application files and all seven retained model hashes match their
recorded baselines. No application edit, rebuild or domain-test rerun occurred.

R130 fixes a stale top-level load-error flag after valid processing or successful version display. R131 fixes GLB recolor being reported as zero softened vertices by carrying the selected operation through the worker and completion callback. Backend algorithms, metadata and file lifetime are unchanged. R131 GUI replay passed at 1544 x 981 / 200%; the earlier size matrix was not repeated for these two bounded changes.

Current EXE SHA256: `136FA9437963E0A12CD996D3C9CCFC91FF88DC655859C6D11AD407CA7E910689`.
Current DLL SHA256: `33FA0C05F7F0CF9BA124E07AABB18E73347AC76C71F2459D695EEA888C6388FE`.
All seven retained models and 29 installed workbench assets still match their prior hashes. Build retains the existing LNK4098 warning. No full-suite rerun or new user visual acceptance is claimed.

R132 fixes a further stale-message defect reproduced on R131: after recovery,
ordinary result navigation still showed the old invalid-vertex-index error.
Valid processing or successful version display now clears the known load-error
labels before clearing the flag. Actual R132 replay confirmed recovery, clean
result/image navigation and preserved selection at 1544 x 981 / 200%.
Startup also restored an existing saved design confirmation; image expansion,
125% zoom and Fit passed without a provider call. No image generation rerun or
complete size-matrix replay is claimed.

R133 extracts the existing plain-pixel delivery into ModelLibraryThumbnail.hpp
and validates dimensions and channel lengths before copying pixels. Obsolete
deliveries are ignored; current unavailable images finish with placeholders.
The production timer calls the same receiver exercised by the tests. The actual
main-window pass loads the pink accepted asset, visits all three history-rail
pages, returns to the first page and opens/returns from Beauty. Selection remains
10,367 faces / zero protected. No processing transaction or new acceptance.

R134 EXE SHA256: `136FA9437963E0A12CD996D3C9CCFC91FF88DC655859C6D11AD407CA7E910689`.
R134 DLL SHA256: `37214B182F8CA4545618F9D4E90167EAC66BFAB2D8CFA51846025E2079DBE547`.
The isolated R134 instance was launched with the validation profile and
opened the genuine historical model before entering Beauty. No paid generation,
candidate transaction or model file mutation was performed.

No commit, push, merge or package was made.

### Current backend control wiring and clean suite R138 (2026-09-29)

- `ModelFinishingOptions` now carries `smoothing_iterations` and
  `preserve_hard_edges`. The existing texture-preserving smoothing backend
  clamps iterations to 1..32 and uses the hard-edge preference for its existing
  55-degree crease protection. The legacy strength-derived iteration count is
  retained when the new value is zero.
- The Beauty workbench UI passes these two controls into the finishing request.
  `减面强度` and `自动补洞` remain visibly disabled because the current
  texture-preserving GLB path has no safe topology/UV/material correspondence
  implementation for them. No new geometry or metadata contract was added.
- Release build targets completed successfully:
  `cmake --build build-figma-3d --config Release --target slic3rutils_tests
  --parallel 4` and `cmake --build build-figma-3d --config Release --target
  OrcaSlicer_app_gui --parallel 4`. The runnable isolated directory is
  `build-figma-3d/runtime-r138/`.
- R138 clean full suite: 593 test cases, 560 passed, 33 environment-gated
  skips, 0 failures; 25,203 assertions passed. The skipped cases report the
  bundled Python interpreter already being in use or NumPy being unavailable.
  The focused smoothing/hard-edge test passed with 1 case and 4 assertions.
- The R138 `[BeautyWorkbench]` targeted run passed 47 test cases and 7,276
  assertions in random order (seed 138). The combined history/presentation
  filter passed 28 test cases and 402 assertions.
- `python scripts/verify_ai_integration.py --json` returned `ok: true` with no
  errors. `git diff --check` returned success with the repository's existing
  LF/CRLF conversion warnings.
- R138 executable SHA256:
  `136FA9437963E0A12CD996D3C9CCFC91FF88DC655859C6D11AD407CA7E910689`.
  R138 DLL SHA256:
  `19BB1594709BFAFF53D60F318870F0B98558010A5B9293FCB6CE3955D3A8883E`.
  The source and `runtime-r138` copies match these hashes.
- The full-suite log is
  `.tmp/figma-workbench/acceptance/full-suite-r138/slic3rutils-full-r138-clean.log`.
  An earlier run saw one external GitHub SSL failure while isolated Orca
  processes were active; the clean rerun after stopping those processes passed
  and is the recorded result above.
- The R138 runtime launched successfully with the isolated validation profile.
  A fresh startup smoke check on 2026-09-29 used PID 4084, confirmed the
  executable path above and `Responding=True`, then stopped it normally.
  The Computer Use channel still returned `Codex auth token is unavailable`,
  so no new screenshot-backed interaction is claimed for this turn.
  User visual acceptance of the Figma match, physical printing fidelity,
  topology-changing controls, and final main-window walkthrough remain
  outstanding. No commit, push, merge or package was made.

### Requirement audit R139 (2026-09-29)

| Plan requirement | Current evidence | Audit result |
| --- | --- | --- |
| Dedicated feature branch with Beauty changes preserved | Current branch is `codex/feat/post-generation-3d-workbench`; worktree retains the Beauty source and asset changes | Pass |
| Dark Figma-style 3D workspace | R124-R127 screenshots and the `build_post_generation_workbench` route show the dark rail, viewport, inspector, history drawer and fixed footer | Pass for implementation; user visual acceptance pending |
| Beauty, selection, semantic color, cleanup and repair behavior | R102/R128/R130-R133 GUI evidence plus `[BeautyWorkbench]` and finishing tests | Pass within recorded scope |
| State locking, candidate lifecycle, history reload and undo/redo | `PostGenerationUiState`, presentation tests, R128/R129 transaction and restart evidence | Pass within recorded scope |
| History thumbnails and stale async delivery handling | R133 main-window route plus 28 presentation/history tests and thumbnail receiver tests | Pass |
| Disabled post-generation base entry | `m_workbench_base` is disabled on the result surface; no post-generation base algorithm or import contract was added | Pass |
| Static/build gates | R138 full suite, focused suite, integration verifier and diff check | Pass |
| Exact visual reproduction and final user acceptance | Computer Use still returns `Codex auth token is unavailable`; existing screenshots document adaptations but do not constitute user acceptance | Pending |
| Texture-safe reduction and automatic hole filling | Current GLB writer requires unchanged topology; no safe implementation exists in the approved first-version contract | Pending future scope |

R140 added a local startup observation for the R138 runtime. The EXE created a
valid process and window handle, but the automated capture remained on the
white initialization surface and did not reach the workbench. That image is
retained under `acceptance/startup-r140/` as startup evidence only; it is not
counted as a visual acceptance result. The earlier R124-R134 screenshots remain
the latest usable main-window workbench evidence.

The corresponding R140 debug log confirms bundled Python and WebView2 runtime
initialization. It records an AI sidecar challenge timeout (`curl: Timeout was
reached`) during startup; this is an environment/network observation and is not
counted as a workbench or provider-flow acceptance result.

R137 rebuilt `slic3rutils_tests` from the current worktree with the Release
configuration and ran the complete suite. The result was 592 test cases,
559 passed, 33 skipped because the bundled Python/NumPy runtime was already
in use or unavailable, and 0 failed; 25,195 assertions passed. The focused
AI/Beauty/ModelGeneration selection also passed: 210 cases, 208 passed and 2
fixture-dependent skips, with 17,674 assertions passed. The rebuilt test
binary SHA256 is
`D37CEBA18531BDD5AE2979BA429CE13830EC2D015D2EB4155EF6826B771A493A`.
`python scripts/verify_ai_integration.py --json` returned `ok: true`, and
`git diff --check` returned success with only existing LF/CRLF conversion
warnings. Logs are in
`.tmp/figma-workbench/acceptance/full-suite-r137/`.

### Continuation review R141 (2026-09-29)

- The current worktree remains on `codex/feat/post-generation-3d-workbench`
  with the previously recorded Beauty and workbench changes preserved. No
  commit, push, merge, reset, clean, or package was performed.
- `python scripts/verify_ai_integration.py --json` returned `ok: true` again.
- Fresh focused Release tests passed independently: `[BeautyWorkbench]`
  47 cases / 7,276 assertions, `[ModelGeneration]` 3 cases / 35 assertions,
  and `[ModelFinishing]` 29 cases / 5,007 assertions. An initial combined
  filter matched no tests; it was corrected and the three real runs are the
  recorded evidence.
- No `orca-slicer` validation process remained running after the checks.
- A Computer Use retry still failed before window discovery with
  `Codex auth token is unavailable`. No screenshot, interaction, or user visual
  acceptance is claimed from this attempt. The latest usable workbench evidence
  remains R124-R134; R140 is startup-only evidence.

### 3D entry crash fix R142 (2026-10-05)

- The user-reported crash was reproduced from the retained crash log
  `.tmp/figma-workbench/validation-profile-r138/log/crash_Mon_Oct_05_08_30_48_0.log`.
  It is an `ACCESS_VIOLATION` in `wxEvtHandler::DoBind`, with the first project
  frame at `ModelGenerationFinishingView.cpp:1298` while
  `build_model_finishing()` was constructing the 3D result page.
- The cause was a construction-order bug: `m_workbench_smoothing_iterations`
  and `m_workbench_preserve_hard_edges` are created by
  `build_post_generation_workbench()`, which runs after `build_preview_panel()`
  and `build_model_finishing()`. The earlier code dereferenced both null
  pointers to bind events during page construction.
- The fix removes those early bindings and installs each binding immediately
  after its control is created. The legacy panel is therefore safe before the
  separate workbench controls exist, while slider/toggle refresh behavior is
  unchanged after construction.
- Release `OrcaSlicer_app_gui` and `slic3rutils_tests` rebuilt successfully.
  Fresh focused tests passed: `[BeautyWorkbench]` 47 cases / 7,276 assertions,
  `[ModelGeneration]` 3 cases / 35 assertions, and `[ModelFinishing]` 29 cases
  / 5,007 assertions. The integration verifier returned `ok: true`; diff check
  passed with the repository's existing line-ending warnings.
- The repaired runtime is
  `build-figma-3d/runtime-r142/`. Its EXE SHA256 is
  `136FA9437963E0A12CD996D3C9CCFC91FF88DC655859C6D11AD407CA7E910689` and its
  DLL SHA256 is
  `8F64DAE5BC288E0994C545E9D1B1EC24C14E3AA539A43F8F42D3A2B85B0238B4`.
  A fresh isolated startup run reached the normal Orca main window and exited
  normally; no new crash file was created. The UI automation bridge remained
  unavailable (`Trusted RPC service is not configured: sky`), so a literal
  mouse click into the 3D tab is still not claimed. The crash stack itself
  proves the failing path was page construction, which the rebuilt DLL fixes.

### Shared user profile launch (2026-10-05)

The `--datadir` argument used by validation runs intentionally creates an
isolated Orca profile. It also isolates `generated_models`, the sidecar log and
the sidecar output directory, so that profile must not be used as the normal
user launch command when existing history is required.

`tools/run_figma_3d_shared.ps1` provides the normal launch path. With no
`-DataDir` argument it omits `--datadir`, allowing Orca to select the standard
Windows profile (`%APPDATA%/OrcaSlicer`) and keeping model history and local
sidecar output together. An explicit `-DataDir` remains available for isolated
validation.

The script was run with R142. It resolved the shared history to
`C:\Users\HONOR\AppData\Roaming\OrcaSlicer\generated_models`, where 40
existing entries were present. The sidecar started from R142, completed the
session challenge and health check, and logged the same shared output
directory. No model files were modified by this smoke check.

The shared launcher also handles PowerShell's empty `ArgumentList` case when
no `--datadir` is supplied. Rerunning it with
`powershell -ExecutionPolicy Bypass -File ...` starts R142 successfully and
keeps the sidecar output in the shared profile.

`start_figma_3d.cmd` is the normal Windows entry point for this version. It
directly starts `build-figma-3d/runtime-r142/orca-slicer.exe` without
`--datadir`, clears validation-only AI environment overrides and relies on
Orca's standard user profile selection. It is suitable for double-click
startup and does not depend on PowerShell execution policy.
