# Phase 61: AI Recommended Printable Palette Review

## Scope and boundary

This phase adds an AI-assisted design-palette workflow to model generation without adding smart-slicing behavior to OrcaSlicer core. The recommended colors are design targets only. Users remain responsible for matching those targets to the physical filaments available on their printer.

The implementation stays inside the model-generation GUI, its loopback Sidecar contract, provider adapters, the local mock, and their tests. It does not modify printer profiles, slicing defaults, 3MF compatibility, or shared OrcaSlicer slicing interfaces.

## User workflow

1. The user supplies text, an optional reference image, and a style.
2. The user explicitly chooses `AI 推荐目标色（用户匹配耗材）` and confirms the potentially billable AI call.
3. AI recommends exactly four distinct target colors with stable semantic roles: primary, structure, light, and accent.
4. The GUI explains the intended use and reason for each color. The user can replace, delete, or add colors, up to the existing four-color limit.
5. The user explicitly confirms the edited palette before generating the printable image preview.
6. The confirmed palette is carried through the existing preview and model-generation job. It is never written into printer filament slots automatically.

The same flow supports text-only, image-assisted, and all existing style modes. Input changes are detected so users can either request a fresh recommendation or deliberately continue with the current palette.

## Contract separation

- `AIModelGenerationClient` exposes provider-neutral recommendation and confirmation DTOs to the wxWidgets panel.
- The loopback Sidecar owns job state, persistence, recovery, provider calls, and validation.
- Provider adapters only translate the recommendation request and response for their upstream model API.
- The local mock implements the same public recovery fields and preview transition as production, allowing paid-provider-free GUI verification.

The Sidecar validates exactly four unique `#RRGGBB` values, the required role set, bounded explanatory text, and sufficient color contrast before exposing a recommendation to the native client. Invalid provider output fails closed.

## Recovery and failure behavior

Recommendation and confirmation state are persisted with the job. Restarting the application restores the original prompt, style, custom style, recommended colors, semantic roles, user confirmation state, and the correct next action.

Network or provider failures leave printer and slicer state unchanged. The user can retry the recommendation or switch back to current/custom colors. The GUI displays recommendation cost consent separately from final preview/model-generation consent.

## Verification

- Provider unit tests cover text and image recommendations, schema validation, uniqueness, semantic roles, contrast, and error mapping.
- Sidecar contract tests cover all three new routes, legal state transitions, persistence, confirmation, invalid palettes, and recovery.
- Mock tests cover public recovery inputs and the text-plus-palette printable preview transition.
- The Release OrcaSlicer target builds successfully on Windows.
- GUI verification used only `D:\Workspace\06_3DDY_claude\build\src\Release\orca-slicer.exe` and the repository-local mock. It covered recommendation, four cards, replace/delete/add behavior, cost warnings, stale-input handling, restart recovery, palette confirmation, preview download/decoding, and enabling the final image-confirmation action.

## Remaining external validation

Real-provider visual quality, upstream billing behavior, exact physical filament matching, and physical print quality require controlled paid-provider and printer runs. Those checks are intentionally not simulated as production evidence here.
