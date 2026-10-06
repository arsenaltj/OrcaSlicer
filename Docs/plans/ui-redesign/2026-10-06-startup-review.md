# Startup UX Review

## Current Candidate

- Branch: `codex/ui-redesign-3d-model-ux-20260930`.
- Base: `047af2bf4d5350e79aabaa18bcd1335cd9f2dce6` plus the local startup patch.
- Checkpoint 1: startup splash, Figma `17:2081`, accepted by the user.
- Checkpoint 2: startup buffer, Figma `17:2063`, accepted by the user.
- Checkpoint 1 status: runnable candidate delivered and user review accepted.
- Windows x64 Release build and eight UiRedesign tests passed for checkpoint 2.
  Actual Chinese buffer at 200% DPI, window actions, Escape and initialization
  handoff to the existing workspace/wizard were checked.
- Checkpoints 3-8: implemented, built and checked as one complete startup
  candidate, pending the user's unified acceptance. The user removed individual
  page approvals on 2026-10-06. The accepted splash and buffer evidence is retained.
- Current build: `.tmp/dev/logs/20261006-174634-178/result.json`, Windows x64 Release.
- Local checkpoint requested by the user on 2026-10-06; this report is included
  in that commit. No push, merge, publication or production-default startup activation.

## Review Entry

Double-click `tools/review_startup_setup.cmd` for the complete journey:
splash, initialization buffer, welcome, region, printer, filaments and home.
The complete runtime is installed at `.tmp/dev/run/orca-slicer.exe`.
The launcher enables the three startup surfaces in its own process and uses
`.tmp/dev/startup-setup-review/user-data`, which has not been used for developer
checks. It does not require PowerShell execution-policy changes.
Successful setup persists this review configuration; subsequent launches skip
the first-use pages. Closing unfinished setup exits and discards its draft.
The normal default data directory is not used by this review launcher.

For the accepted buffer checkpoint alone, run `tools/review_startup_buffer.cmd`.
The runtime is installed at
`.tmp/dev/run/orca-slicer.exe`; the launcher uses the separate data directory
`.tmp/dev/startup-buffer-review/data` and pauses a real startup stage.
Escape or the top-right close icon exits. Ctrl+Enter resumes initialization.
The default image-home route pauses at "Opening the workspace..."; the existing
Prepare route pauses at "Preparing fonts and graphics...". The comparison
screenshot uses `ORCASLICER_UI_REDESIGN_IMAGE_HOME=0` to reach that real Fonts
stage. The preview does not open a new setup page or apply any setup selections.

The accepted splash remains available through `tools/review_startup_splash.cmd`
and its original evidence remains under `.tmp/dev/startup-review`.

For the normal startup journey, set `ORCASLICER_UI_REDESIGN_STARTUP_SPLASH=1`
without `ORCASLICER_UI_REDESIGN_STARTUP_REVIEW`. The existing splash preference,
startup stages and main-window handoff remain active. Buffer and first-use setup
have separate `STARTUP_BUFFER` and `STARTUP_SETUP` opt-ins. Default startup will
be enabled after unified user acceptance.

## Complete Setup Implementation

| Checkpoint | Figma | Implemented behavior |
| --- | --- | --- |
| 3 | `17:938` | Welcome, placeholder brand and start action |
| 4 | `17:1298` | Four existing regions, local draft selection |
| 5 | `17:1110` | Three real product images; next disabled until selection |
| 6 | `17:1476` | Single selection, highlight, back preserves the draft |
| 7 | `17:1664` | Compatible filaments and colour per physical head; complete and home |
| 8 | `17:1883` | Actual profile errors, disabled completion, back and reload |

- `StartupSetupService` provides the catalog, draft, state and commands.
  WonderMaker ZR, ZR Ultra and ZR Ultra S use local 0.4 mm profiles with 1/4/4
  physical heads. Compatible PLA Basic is the default.
- `StartupSetupDialog` uses native wxWidgets, local Figma assets, HONOR Sans
  Design, 692 x 500 DIP panels, 200 x 48 DIP actions, and a scrollable backdrop
  for smaller windows. The dark lists retain wheel, keyboard and scrollbar input.
- Default colours share Orca's existing palette. The native custom colour dialog
  edits the draft; each head retains its material and colour independently.
- The legacy guide and new setup share `apply_startup_vendor_selection` and the
  existing `PresetBundle::apply_vendor_config` installation boundary.
- Completion validates and saves a staged bundle before publishing live
  selections. `firstguide.finish=1` is written only with successful setup.
  Invalid profiles, save failure, duplicate completion and cancelled/late
  catalog callbacks have targeted coverage.
- Preset copies rebind their edited and saved vendor pointers. Shell listeners
  disconnect before feature-host destruction, fixing the observed exit crash.
- Layout updates from the backdrop's actual resized dimensions, fixing the
  observed maximize/restore offset. List tooltips use virtual-row hit testing,
  fixing the constant purple colour tooltip. The back-button font matches the
  scaled Figma instance's rendered text size.

## Complete Setup Evidence

- Final source identity:
  `06ea60d8c9910df57f575f4290bffd5dbceb7f76aeedb8ad243dd497352000af`.
- EXE SHA256:
  `10DD6B305B259A123A330E0B5C66500EE2EDCF3B1584E24405C4F3D9DA87D0F0`.
- DLL SHA256:
  `44B1CE2B4D30F991713010E48EB0D0E340179574EF5A657D1B3C2249A130D1C0`.
- Automated tests: `.tmp/dev/logs/20261006-172114-926/result.json`, 19 UiRedesign
  tests passed, including 11 StartupSetup cases; test execution 9.19 seconds.
  This tests source identity `dd8c48175aca37da050bb65a164fbfc09fcb83b4ef874560e3b90fd3731f4d3a`.
  Later program edits only correct list tooltips and the back-button font in
  `StartupSetupDialog.cpp`; tested service, persistence and state inputs match.
- Final build/install, resource identity and bundled Python/Pillow checks all
  passed in `.tmp/dev/logs/20261006-174634-178/result.json`.
- Complete candidate evidence is under `.tmp/dev/startup-setup-review/`:
  `source-snapshot.json`, `tracked-changes.patch`, `untracked-source/` and
  `verification.json`. The delivered binary was built from a dirty local snapshot
  based on the stated HEAD, before the local checkpoint. It is not a build of
  a clean integrated commit.
- `comparison-all.png` and six individual `comparison-*.png` files compare
  692 x 500 panels without resizing. The first five use the delivered build's
  screenshots; the error comparison explicitly labels the earlier verified
  recovery build, before the back-button font correction.
- `welcome-delivered.png`, `region-delivered.png`,
  `printer-unselected-delivered.png`, `printer-selected-delivered.png`,
  `filaments-delivered.png` and `home-delivered.png` show the final complete
  setup run in `qa-delivery-20261006-1755`, PID 18700, normal exit code 0.
  Saved configuration has China, Ultra S 0.4, four heads, head two's compatible
  ABS and `#FFF144`, the other heads' PLA Basic and white, and finish=1.
- `qa-verified/OrcaSlicer.conf` retains the independent North America trial,
  Ultra S, head two ABS/custom `#FF80FF`, other heads PLA Basic/white, and
  finish=1. Back/forward and custom-colour persistence were checked there.
- `restart-home-delivered.png` shows the final build skipping setup with an
  existing valid Ultra configuration in `qa-final`, PID 32912, exit code 0.
- `welcome-after-cancel.png`, `colour-tooltip-green-final.png` and
  `colour-tooltip-scrolled-final.png` show cancel/restart and correct green/red
  tooltips in `qa-small`, PID 30580, exit code 0. Its configuration still has
  Default Printer, no finish flag and no submitted filament-colour draft.
- Maximize/restore and small-window scrolling passed at current 200% DPI.
  Screenshots include `filaments-maximized-verified.png`,
  `filaments-small-candidate.png` and `filaments-small-scrolled-candidate.png`.
  Actual restored, maximized and small windows were approximately 1190 x 795,
  1560 x 1040 and 750 x 535 logical pixels. Small windows allow both-axis scrolling.
- Real profile-change rejection and recovery passed in `qa-error`, PID 44368,
  exit code 0: change the loaded Ultra S nozzle values to 0.6, reject completion
  without finish/selections, reload to expose the missing 0.4 catalog error,
  restore the profile and invalidate the isolated stale cache, reload and finish.
  `error-save-candidate.png`, `error-catalog-candidate.png`,
  `filaments-recovered-candidate.png` and `home-after-recovery-candidate.png`
  retain these states. The restored profile matches the resource SHA256
  `32A2D25A673C1F54FB27E85BA925354E31AD0593C83E62269A89091C9FA6CB81`.
  Faulty profile/cache and the unsuccessful pre-start injection attempt are
  retained; `filaments-catalog-error-verified.png` is a normal page from that
  unsuccessful attempt and is not error evidence.

## Unified User Review

- Check the splash/buffer transitions, welcome and region menu against Figma.
- Check all three printer cards, single selection, disabled next and back navigation.
- Check 1/4-head pages, independent material/colour selection, custom colours,
  scrolling, long-name tooltips, completed setup and home transition.
- Check cancellation/restart and restart after successful completion.
- Check overall typography, icon sizes, rounding, centering and window actions
  at the user's normal desktop size and DPI.

The implementation and developer checks are complete for this candidate.
User visual acceptance remains pending. The full resolution/DPI matrix below
and normal data-directory history/AI discovery checks remain unverified.
Default startup activation is intentionally pending unified acceptance.

## Checkpoint 2 Review Checklist

- Full-window `#313136` background and 40 DIP dark top bar.
- Centered 180 x 180 DIP placeholder with 24 DIP corners, white 44 px Name,
  and 20 px status at the design's spacing; no progress bar on this page.
- Three locally installed Figma SVGs, 20 DIP icons and 40 DIP action targets.
- Minimize, maximize/restore, close, hover feedback and tooltips.
- Actual startup-stage text and clean close while initialization is paused.
- Fixed DIP proportions and centering in restored and maximized windows.
- Review the native font rendering differences shown in the 1:1 comparison.

## Checkpoint 2 Evidence

- Final build: `.tmp/dev/logs/20261006-154305-442/result.json`.
- Final tests: `.tmp/dev/logs/20261006-154522-616/result.json`
  (8 passed, 0 failed, Release).
- Final source identity:
  `aa004a037a489b90a6feb8db72d6266eb4d8f8fdd64998ad4f130228a8c3f2ea`.
- Runtime executable SHA256:
  `10DD6B305B259A123A330E0B5C66500EE2EDCF3B1584E24405C4F3D9DA87D0F0`.
- Runtime `OrcaSlicer.dll` SHA256:
  `1FE874C558AEB4CA50D54D7E7614BCE0A78A4B6B7BDB3D92CB962D5C8DFE7BCF`.
- `.tmp/dev/startup-buffer-review/source-snapshot.json`: final build input hashes.
- `.tmp/dev/startup-buffer-review/tracked-changes.patch`: tracked patch including
  the preserved checkpoint 1 changes; untracked program files are hash-listed
  in the source snapshot and remain in this checkout.
- `.tmp/dev/startup-buffer-review/buffer-final-clean.png`: final native window.
- `.tmp/dev/startup-buffer-review/comparison-final.png`: 1:1 central crops,
  Figma left and final native candidate right, without resizing either crop.
- `.tmp/dev/startup-buffer-review/visual-metrics-final.json`: measured glyph bounds.
- `.tmp/dev/startup-buffer-review/verification.json`: operations, provenance and limits.
- `.tmp/dev/startup-buffer-review/prepare-handoff.png`: existing workspace/wizard
  after resuming Fonts; initialization reached `workspace_revealed`.

The actual restored window was 1190 x 795 logical pixels and the maximized
window was 1560 x 992, at 200% DPI. Name starts at the same relative glyph Y
(241 DIP) as Figma. Its measured native glyph bounds are 122 x 32 versus the
reference's 120 x 33. The captured background is RGB 50/49/54 versus Figma's
49/49/54; the paint code uses the specified `#313136`. These small capture/font
differences are recorded for visual review rather than called pixel-identical.

The first checkpoint 2 build and GUI journey used source identity
`01017ff70f5ef372d7a84d59b10445c503678cf43c6a6fcb7d340e3fdcf5ae42`.
The final change only recalculates the right spacer on DPI changes, with the
same initial 200% geometry. Final rendering and window actions were rechecked;
the unchanged initialization/keyboard checks retain their earlier evidence.
The final increment passed after correcting a wxSizerItem API spelling error;
the failed attempt remains in `.tmp/dev/logs/20261006-154129-102`.

Checkpoint 2 source is in `StartupBufferView.hpp` and the existing
`StartupLoadingPanel`/`StartupStage` host in `GUI_App.cpp`. No new first-use
configuration service, setup page or production default entry was enabled.

## Checkpoint 1 Evidence

- Build: `.tmp/dev/logs/20261006-125831-007/result.json`.
- Tests: `.tmp/dev/logs/20261006-130135-101/result.json` (7 passed, 0 failed).
- Built source identity:
  `c74a8866c65fd762d978cfc1b0ffe53da6a85b61703890b0b29577ef15e6c511`.
- Runtime executable SHA256:
  `10DD6B305B259A123A330E0B5C66500EE2EDCF3B1584E24405C4F3D9DA87D0F0`.
- Runtime `OrcaSlicer.dll` SHA256:
  `E57361AAE37A023D0EFA6444C5E26A40D732CE67336C861392CD784DDDF065D3`.
- `.tmp/dev/startup-review/figma-17-2091.png`: Figma splash reference.
- `.tmp/dev/startup-review/startup-final.png`: actual review window screenshot,
  captured at 200% DPI and represented in logical pixels.
- `.tmp/dev/startup-review/comparison.png`: Figma left, actual candidate right.
- `.tmp/dev/startup-review/normal-handoff.png`: existing main window and existing
  first-use wizard after ordinary startup; no new setup page is implemented.
- `.tmp/dev/startup-review/source-snapshot.json`: source file hashes from the build.
- `.tmp/dev/startup-review/verification.json`: operations, artifact identity and limits.

The review splash is 1180 x 1180 physical pixels at the current 200% DPI,
corresponding to 590 x 590 DIP. Version and status glyph positions match the
Figma reference; NAME differs by at most one logical pixel in glyph bounds.
Version is the real 2.5.0-dev, replacing Figma's example 1.5.0-dev. The progress
fill is the actual 30% stage, replacing the reference's 30.41% example.

## Remaining Review

- User visual acceptance of checkpoint 1 was received before starting checkpoint 2.
- User visual acceptance of checkpoint 2 was received. Checkpoints 3-8 are
  implemented and delivered together for final review.
- 1920 x 1200, 1920 x 1080, 1440 x 900, 1280 x 800, 100% and 125% DPI,
  English/long status text and switching monitors
  with different DPI are not yet checked in actual windows. The current native
  font is HONOR Sans Design. The Fonts and Finish Chinese statuses were checked.
- The checkpoint 1 trial encountered the existing welcome wizard's close
  limitation and was terminated after its handoff capture. The checkpoint 2
  handoff reached that unchanged wizard and the trial subsequently exited with
  code 0. Neither trial verifies future first-use cancel/apply behavior.
- Profile persistence, complete setup, cancellation and actual profile-error
  recovery were checked as recorded above. Historical assets and local AI
  service discovery with the normal data directory remain checks for activating
  the production-default entry after the user's unified acceptance.
- `verify_ai_integration.py --json` reports six architecture diff budgets:
  root CMake +213/-19, slic3r CMake +78/-2, MainFrame.cpp +579/-111,
  MainFrame.hpp +37/-1, Plater.cpp +429/-691, Plater.hpp +57/-5.
  The integration gate is not passed; its budgets were not relaxed.
- The user requested a local startup optimization commit on 2026-10-06.
  No push, merge or release was performed; unified visual acceptance remains pending.
