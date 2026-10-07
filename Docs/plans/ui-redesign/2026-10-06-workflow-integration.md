# Startup, Model Workbench And Slicing Integration

This record captures the pre-submission development candidate and its acceptance evidence on 2026-10-07. Later submission or integration commits do not retroactively identify this runtime as a build of the merged source.

## Frozen Sources

- Destination: `codex/ui-redesign-3d-model-ux-20260930`, HEAD `dfa4f8ee642be24023a1510e906e54b9c2d85e65`; clean worktree at implementation start.
- Complete Beauty workbench: `cadeca384f33c3cbcfe6b51473ee1b934c7a269c`.
- PR20 loading, texture preservation, asynchronous save and cancellation: `78f3166f36`.
- Optional portrait protection: COL-016 archive `COL-016-20261006-153603-425699-r6-gui-ready`, based on `cadeca384f`. Its archived sources are checked against the manifest. Live R7 changes are excluded.
- Figma: `TafDZMN6dVlTTT5DX1LwCF`, model/result `15:6355`, overview `15:6079` / `15:2853`, color `15:914`, slicing `15:1251` / `15:1462` / `15:1685` / `15:2020`.

## Ownership And Scope

Keep the current startup, Shell, generation FeatureHost and submission chain. Transplant Beauty controls, preview/texture persistence and version transactions into the current ModelGeneration ownership boundary. Keep all model checks and edits detached until explicit color confirmation and project import. R6 is optional and requires compatible identity-bound protection data; R7 remains excluded.

Expose read-only workbench and slicing snapshots through feature hosts. The Shell owns navigation and embeds the existing native renderer and Orca business controls. Model import uses one exact rollback-capable transaction for the new objects and colors. Slicing uses the existing three-goal recommendation coordinator and versioned Apply gateway; Preview and export remain within the Model navigation.

## Progress

- Phase 0: frozen sources and module dependencies identified. Destination HEAD remains `dfa4f8ee64`; implementation is an uncommitted working-tree snapshot.
- Phase 1: independent overview, FeatureHost commands, detached model checks and history selection implemented. Real accepted assets load, expose actual topology and dimensions, and select their current history card. Local history and checks also work with the sidecar stopped.
- Phase 2: complete Beauty controls, selection/protection/detail controls, local recoloring, texture persistence, async candidate publication, draft restore and version transactions transplanted. Actual local recoloring, candidate discard/accept, undo/redo, save, recheck, return and history reopen exercised. Selection and protection persist through the accepted-version undo/redo sequence.
- Phase 3: archived COL-016 R6 identity checks, optional entry, protection and explicit unlock transplanted. The actual protection entry and saved result were exercised. R6 starts disabled; incompatible assets keep the entry disabled. Portrait appearance is still awaiting user visual acceptance.
- Phase 4: dark native color review and detached preparation implemented. Model, colors and new-object placement commit through one rollback-capable transaction. Actual color review, import and single Undo exercised. Six placement/import tests passed after fixing the initial bed index; color cancellation and rollback have targeted coverage.
- Phase 5: three-goal state, actual candidate parameters, revision checks, versioned Apply, official slicing and local export implemented. Real native slicing completes, opens Preview under the Model navigation and exports local G-code. Returning restores the native parameter view. Real AI Apply remains blocked by missing machine validation evidence.
- Phase 6: matching Windows x64 Release candidate built, installed and exercised. Unified first-use setup, restart, history/Beauty/color/import, native slicing/Preview/export and several failure/regression paths have evidence below. Full acceptance remains incomplete.

## Candidate And Entry

- Checkout: `D:\TEST\OrcaSlicer-ui-redesign-3d-model-ux-20260930`.
- Build: `.tmp/dev/build`; installed runtime: `.tmp/dev/run`.
- Double-click review entry: `tools/run_model_workflow_review.cmd` (PowerShell wrapper uses process-local execution-policy bypass).
- The review entry enables startup splash, initialization buffer, first-use setup and the model workflow. Per-page review pauses are disabled. It restores the calling process environment after launching Orca and the installed local sidecar.
- Default isolated data: `.tmp/dev/workflow-review`, including the real asset copies used for workbench checks. Unified first-use/restart data: `.tmp/dev/workflow-review-unified`.
- Local review service: `127.0.0.1:18767`, with a generated session token and parent-process lifetime. No provider generation was submitted during acceptance.
- Latest native build/install: `.tmp/dev/logs/20261007-093755-396/result.json`, `PREPARED`, build 139.41 seconds, install 5.87 seconds, runtime identity check 22.69 seconds; bundled Python checks passed.
- This candidate retains the shared plate-icon theme correction, exposes failed historical-model replacement without losing the current valid model, and clears that error after successful retry. Result summary and statistics use the existing auto-wrapping Label at their actual column width; the rebuilt window was checked for complete text and correctly placed controls.

| Identity | SHA256 / value |
| --- | --- |
| HEAD | `dfa4f8ee642be24023a1510e906e54b9c2d85e65` |
| Program source snapshot | `98e958291832d0986c0e5a8a16e9100330d05dfe32eb59d502497f8a0c322180` |
| Native input snapshot | `4aa4518a2a4a8d241be4c2b7886abf2e4eaf03df7aa5b51831cb1fae96dabb88` |
| `orca-slicer.exe` | `10dd6b305b259a123a330e0b5c66500ee2edcf3b1584e24405c4f3d9da87d0f0` |
| `OrcaSlicer.dll` | `080ecbd2813058033956ce2a64182fb53d043fa74b1fafc95d7644c8c7af5fd5` |
| Installed `orca_ai_sidecar.py` | `7707ac84a90010447a62bd61852992f1df673ae58fe2ea89a227ea652d03077f` |
| Installed bootstrap | `635413ae9c6e7b0535ca23f8770ca112a257df84132fae93f8f24aecda7e233c` |
| Beauty runtime manifest | `3c9b92502af4144774834474f1c84bf17b6302fa9d2e2cd86c33c2d200fa779f` |

`.tmp/dev/runtime-state.json` records 17,285 installed files, including 16,097 resource files, with individual hashes and the 226 program input hashes. This is a development identity record, not a clean commit or a packaged source archive. Documentation and evidence are recorded separately from its program-input identity.

## Actual Main-Window Results

Evidence directory: `.tmp/dev/workflow-evidence/`. Screenshots record the candidate actually exercised: `17`-`34` use the earlier candidate, `35`-`53` use build `20261007-084410-596`, `54`-`61` use `20261007-091930-507`, and `62`-`63` use the latest build above. Relevant earlier Beauty, import and native slicing inputs are unchanged by the final replacement-error and result-label corrections.

| Journey / condition | Result | Evidence |
| --- | --- | --- |
| New isolated first-use setup | Welcome, China, ZR Ultra, four filaments, completion and home worked | `17-unified-first-use.png`, `18-unified-filaments.png` |
| Configuration persistence | `firstguide.finish=1`, region `China`, `WonderMaker ZR Ultra 0.4 nozzle`; restart skipped the guide and restored yellow `#FFF144`, green `#0ACC38`, white, white | `25-restart-home.png`, `26-restart-restored-filaments.png` |
| Empty project / native view | Empty geometry keeps slicing and export disabled; first GL initialization completes | `19-empty-native-disabled.png` |
| Historical assets / selection | Real textured model loads; selected version, face/vertex counts and dimensions displayed | `03-textured-model-overview.png`, `06-history-dialog-dark.png` |
| Beauty and saved texture | Region selection, protection, detail controls and local eye recoloring exercised; saved version reopened | `04-selection-protection-refinement.png`, `05-reopened-texture-with-local-color.png` |
| Optional R6 | Protection preserved and saved result reopened; appearance not accepted | `07-r6-protection-preserved.jpg`, `08-r6-saved-overview.jpg` |
| Color and project handoff | Native review exercised; imported model/colors/placement undone together | `09-color-review-original.jpg`, `11-color-review-centered.jpg`; walkthrough log |
| AI preflight rejection | Actual unsupported machine/material identity and open edges shown; three cards do not invent usable candidates | `20-final-ai-reasons.png` |
| Long messages / narrow window | Complete reason remains visible; scrolling does not move the bottom command bar | `21-final-small-window-scroll.png` |
| Native slicing / Preview / return | Real 500-layer slicing completes and keeps Model navigation; return restores native parameters | `22-final-native-preview.png` |
| Local export | Native unsupported-region warning displayed and confirmed; G-code written successfully | `23-final-export-warning.png`, `24-final-export-success.png` |
| Sidecar offline | Stopped only the review sidecar, confirmed port 18767 had no listener; accepted local asset loads and offline checks report five open and five non-manifold edges | `27-offline-history-model.png`, `28-offline-model-check.png` |
| Invalid local STL | Native import rejects an intentionally invalid mesh; current model remains at 976,825 faces and 519,975 vertices | `29-invalid-model-error.png`, `30-invalid-model-retains-project.png` |
| Multi-plate behavior | Added a second empty plate; native slicing/export disabled on that plate | `31-native-empty-second-plate.png` |
| Offline native slicing | First plate slices and opens Preview while the sidecar remains stopped | `32-offline-native-preview.png` |
| Preview plate invalidation | Selecting the empty second plate invalidates the export action and reports the need to re-slice | `33-preview-empty-plate-export-disabled.png` |
| Native project save/reopen | Saved two plates to a separate 3MF; reopened with model, colors and both plates preserved | `34-native-multi-plate-project-reopened.png` |
| Native edit undo/redo | Added a 30 mm cube; Undo restored the clean original project, Redo restored the cube, a final Undo returned to the original two-plate project | `35-native-primitive-added.png`, `36-native-undo-clean-project.png`, `37-native-redo-restores-object.png` |
| Beauty draft and candidate lock | Reopened the real asset with 1,197 selected faces and 163 protected faces; created a yellow `#FFF144` local recolor candidate; formal import remained disabled until resolution | `38-beauty-candidate-import-locked.png` |
| Beauty accepted-version undo/redo | Accepted the candidate; Undo restored white and Redo restored yellow with selection/protection counts preserved; checked actual colors with selection highlighting off | `39-beauty-version-accepted.png`, `40-beauty-redo-color-restored.png`, `41-beauty-undo-color-restored.png` |
| Save, recheck and history selection | Save/return increased history from eight to nine models and selected the new version; checks again reported five open/five non-manifold edges, while unsupported wall/overhang checks remained unexecuted | `42-beauty-save-recheck-history-selected.png` |
| Color review cancel | Preview retained the local yellow edit; Escape returned without changing the clean formal project, its original model or its two plates | `43-color-review-preserves-local-edit.png`, `44-color-cancel-retains-project.png` |
| Automatic project recovery | Added an unsaved cube, verified the backup contained two objects, preserved that backup, then terminated only the isolated review process. Restart offered recovery and restored the original model, cube, two plates, printer and four filament colors | `45-recovery-unsaved-cube.png`, `46-recovery-prompt-before-shell.png`, `47-recovery-two-plates-and-cube.png`, `48-recovery-presets-and-colors.png` |
| Recovery save and close | Recovered project initially remained dirty; explicit Save As to a separate 3MF cleared dirty state; review process then closed normally | `49-recovery-project-saved.png` |
| Damaged historical metadata | Deliberately damaged metadata in an isolated asset copy; history remained available and another valid real asset loaded | `50-corrupt-metadata-history-available.png`, `51-corrupt-metadata-valid-model-loaded.png` |
| Damaged historical model and retry | Invalid GLB replacement retained the valid current model. A hidden-error defect was fixed; the corrected workbench exposes the failure. Restoring the isolated asset and retrying cleared the error and loaded the requested version | `52-corrupt-model-retained-with-hidden-error.png`, `53-corrupt-model-error-on-result-page.png`, `54-corrupt-model-error-visible-current-retained.png`, `55-restored-history-model-error-cleared.png` |
| Image/Assets navigation | Switched through Image and Assets and returned to the retained model. Selected a copied real local PNG without submitting generation; returning again retained the model | `56-cross-module-model-retained.png`, `58-local-image-selection-no-generation.png` |
| Native process preset and persistence | Selected 0.16 mm Optimal; native layer height synchronized. Save As produced an independent 3MF. Reopening the original restored 0.20 mm, four colors and two plates | `59-native-process-preset-changed.png`, `60-native-process-preset-saved.png`, `61-native-project-restored-process-and-colors.png` |
| Result details at actual column width | Reproduced clipped summary/statistics, replaced fixed-width wrapping with existing auto-wrapping labels, rebuilt and checked full text and control placement. Returned to the ready model and selected history | `57-result-details-clipped-before-fix.png`, `62-result-details-wrap-at-actual-column-width.png`, `63-result-return-current-history-retained.png` |

The invalid STL test used the ordinary native `Ctrl+I` path. It preserved geometry but marked the project dirty and showed two native error dialogs. Its relevant `Plater.cpp` import implementation is unchanged from destination HEAD: it takes a snapshot before parsing and emits the empty-geometry error after the parse error. This is a retained baseline defect; the test does not establish zero Undo-history consumption for that native path. The new detached workbench import has separate transaction/rollback tests and a successful single-Undo GUI check.

Recovery deliberately enters native Prepare before the Shell is activated. This follows the documented startup compatibility exception in `Docs/plans/ui-redesign/README.md` (2026-09-23 single-direction migration section) and `GUI_App.cpp::should_start_with_redesign_shell`; it is not a transition from an active Shell back to the old Notebook.

Final one-plate project: `portrait-workflow-native-valid.3mf` (16,021,478 bytes). Its predecessor with an out-of-bounds wipe tower remains preserved as `portrait-workflow-native.3mf`; the tower was corrected through the native move tool before slicing. Bed-boundary validation was not bypassed.

Multi-plate regression project: `portrait-workflow-multi-plate-review.3mf`, 16,027,266 bytes, SHA256 `00e3ca4ecb4827ad7cfe656dbb22ec1f90b7d5575b7dd71052500df539de8fbb`. This file was explicitly saved through native Save As and reopened; it was not created by an automatic workflow save.

Recovered regression project: `portrait-workflow-recovery-review.3mf`, 15,988,575 bytes, SHA256 `B1EC04EA3EEFFDACF5B27711AB34AA707B8F7C4D1C2627845608FB05EB447C82`. Native Save As created this independent evidence file. Its ZIP/XML/JSON contents confirm two build objects and two plates, `WonderMaker ZR Ultra 0.4 nozzle`, four `WonderMaker PLA Basic` slots, four 0.4 mm nozzles and colors `#FFF144`, `#0ACC38`, `#FFFFFF`, `#FFFFFF`. The pre-recovery backup is preserved under `recovery-input-20261007-0900/`.

Preset regression project: `portrait-workflow-preset-review-20261007.3mf`, 15,988,641 bytes, SHA256 `C2BFA0F1BD4136CD49C25E903D0C991FA206F85407874A9F988B3118B804CDC2`. Actual native Save As produced this separate file; ZIP/JSON inspection confirmed 0.16 mm layer height, 0.2 mm initial layer, ZR Ultra 0.4, four PLA Basic slots and the same four colors. Native Ctrl+Z after process-preset selection did not restore that preset. Restoration used reopening the original project, whose hash stayed unchanged. Preset Undo is not recorded as passed; its baseline cause has not been established.

Final export: `portrait-workflow-final-20261007.gcode`, 22,554,471 bytes, SHA256 `62a84ee755455493087ccad63ed561cb0388c0a4441e2792c4b46d0003c6070b`. Actual statistics: 500 layers, 100 mm, estimated time 5h 22m 44s (GUI 5h23m), total filament 89.68 g, four 0.4 mm nozzles. Export retained an unsupported-region warning; no physical print quality is claimed.

Main walkthrough log: `.tmp/dev/workflow-review-unified/log/debug_Wed_Oct_07_08_09_36_30964.log.0`. Restart log: `debug_Wed_Oct_07_08_22_48_43696.log.0` in the same directory. Offline and multi-plate log: `.tmp/dev/workflow-review/log/debug_Wed_Oct_07_08_25_38_45220.log.0`.

Recovery log: `.tmp/dev/workflow-review/log/debug_Wed_Oct_07_09_00_26_32688.log.0`.

Historical failure/preset log: `.tmp/dev/workflow-failure-review-20261007/log/debug_Wed_Oct_07_09_21_17_32744.log.0`. Latest result-layout check: `debug_Wed_Oct_07_09_41_09_3516.log.0` in the same directory.

Review instances 30964, 43696, 45220 and 32688 exited normally; their owned sidecars are no longer running. Review instance 29340 alone was terminated to test recovery, and its owned sidecar 25300 exited. Sidecar 9928 was deliberately stopped for the offline check. Separate archived R6 process 12644 was still running at the final process check and was not operated or changed.

Subsequent review instances 32744 and 3516 and their owned sidecars 31224 and 40384 have exited. The final process check found only the separate archived R6 instance among those identities; it remains preserved.

## Automated Checks

- Latest targeted C++ run: 30 tests passed (CTest 10.70 seconds; command 10.84 seconds), `.tmp/dev/logs/20261007-091726-585/`. This includes retained-model load failure/recovery and affected workbench, workflow, slicing and startup conditions. The later change only corrects result-label sizing; relevant logic inputs are unchanged.
- Preceding retained C++ run: 256 tests passed in 36.75 seconds, `.tmp/dev/logs/20261007-071553-633/`. This covers Beauty/workbench state, import/rollback, undo/config state, smart slicing/preflight/Apply, workflow and Shell logic. These results are reused only for unchanged inputs.
- Earlier retained focused runs: 274 tests (`20261007-032930-335`), 44 (`20261007-031606-523`), 21 (`20261007-041933-206`), 33 (`20261007-061721-568`). Counts overlap; they are not a summed unique-test count.
- Broad run: 1,087 tests, four failures and seven skips, `.tmp/dev/logs/20261007-002359-687/`. Trial bounds, invalid outgoing trial-state serialization and processing-state locks were fixed; the following 160-test run passed (`20261007-003159-039`). `Check SSL certificates paths` remains unresolved: its real GitHub HTTP check expected 200 and received 0; the root cause is not proven. The complete broad suite has not been rerun to a clean pass.
- Earlier import/placement failure was repaired and all six `[LocalPrintModelImport]` tests passed in `.tmp/dev/logs/20261006-231641-967/`.
- Python: 77 relevant tests passed in the preceding implementation. Relevant Python inputs are unchanged. No real provider job was used as a test.
- After the latest logic suite, the only program edit was the result-label sizing correction. Release was rebuilt and the affected actual window states exercised. Counts from retained runs overlap and must not be added into a unique-test total. Unrelated tests were not repeated.
- `git diff --check` passed for the candidate.

## Integration Check

`scripts/verify_ai_integration.py --json` remains unsuccessful with six architecture-budget findings, recorded in `.tmp/workflow-integration-final.json`. The lock/budgets were not widened. All six categories already fail at destination HEAD, but several have increased in this implementation:

| Shared file | HEAD versus lock baseline | Current versus lock baseline |
| --- | --- | --- |
| Root `CMakeLists.txt` | +213/-19 | +213/-19 |
| `src/slic3r/CMakeLists.txt` | +78/-2 | +89/-2 |
| `MainFrame.cpp` | +579/-111 | +601/-111 |
| `MainFrame.hpp` | +37/-1 | +37/-1 |
| `Plater.cpp` | +429/-691 | +441/-691 |
| `Plater.hpp` | +57/-5 | +59/-5 |

These are recorded failures, not integration approval. The final UI-file and review-script fixes did not change the shared-file budget categories.

## Remaining Acceptance

- Real AI three-goal Apply and successful official slicing cannot be accepted with the supplied WonderMaker capability evidence. The registry requires calibrated nozzle offsets/reach, collision, thermal/tool-change, wipe/flush and specialized validation. Current four-nozzle profiles remain `PendingValidation`, as required by `test_smart_slicing_capabilities.cpp` and the UX requirements section 25.8. The reopened project also fails machine/material identity and open-edge preflight checks. Do not replace these checks with optimistic defaults.
- Full 1920x1200, 1920x1080, 1440x900 and 1280x800 visual checks at 100%/125% are still pending. Actual desktop scaling was 200%, with 1560x992 and 1190x795 logical window captures. Those captures are not the required complete matrix.
- Native plate-operation icon contrast after project reopen was corrected and rechecked. The existing image-home connection label still retained its last connected result after the sidecar was stopped; the offline check establishes local workflow availability, not continuous connection-status detection.
- User unified visual acceptance, including R6 portrait appearance, is pending. No printed color/quality result exists.
- Native object undo/redo, Beauty accepted-version undo/redo, automatic recovery, damaged historical metadata/model replacement and corrected retry now have actual GUI evidence. Native preset selection, save/reopen and local image selection/navigation were checked; the complete preset/image transition matrix remains pending. Native Ctrl+Z did not restore the selected process preset, so preset Undo remains an unresolved observation.
- Default-user-directory history/service discovery and normal double-click entry activation remain pending the user's unified acceptance. All current workflow activation is scoped to the isolated review entry.
- Architecture-budget and SSL-certificate-path failures remain recorded above.

## Verification Boundaries

Code tests, actual GUI operation, user visual acceptance and printed fidelity are distinct. The candidate is available for unified review; the complete plan is not marked accepted. This round does not authorize paid generation, printing, normal-entry enablement before unified acceptance, commits, pushes, merges or publication.

Use this checkout's `.tmp/dev/build` and isolated review data. Other checkouts, user projects and the separately running archived R6 instance are preserved. Do not overwrite this evidence with a later candidate without recording the new source/runtime identity.

Final source identity and primary installed hashes were rechecked against the candidate. The integration check still reports the same six failures; `git diff --check` passed. No commit, push, normal-entry enablement or package publication was performed.
