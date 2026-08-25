# Phase 80: Quality-guided regeneration design

## Goal

Turn existing structural and visual review evidence into a safe, explainable
input for the next model-generation attempt. The first Phase 80 slice must not
modify the mesh, change Orca slicing behavior, or start a paid provider request
without the existing explicit confirmations.

## Approaches considered

### 1. Automatically repair the final mesh

This could thicken walls, flatten the base, or reshape overhangs after Tripo.
It appears direct, but robust geometry edits need solid modeling semantics that
the current triangle OBJ does not provide. On the single real end-to-end sample,
automatic thickening could close the radio grille, merge knobs, or alter the
handle. This approach is rejected for Phase 80.

### 2. Append warning text directly inside the Sidecar retry loop

This is small, but it couples quality policy to Tripo orchestration and may
silently create another paid task. It also gives the user no opportunity to
understand or edit the proposed changes. This approach is rejected.

### 3. Provider-neutral, user-approved refinement advice

The selected approach derives bounded structured advice from persisted quality
reports. The public job contract exposes the advice. The GUI shows the reasons
and can append a concise instruction block to the next input, but applying it
does not call Image2, GPT, Tripo, or another provider. The existing preview and
3D confirmation dialogs remain the only paid-call entry points.

## Architecture

```text
model-quality.json ----+
                       +--> model_refinement.py --> public job refinement DTO
visual-quality.json ---+                                  |
                                                          v
                                                AIModelGenerationClient
                                                          |
                                                          v
                                                ModelGenerationPanel
                                                show / apply to prompt
                                                          |
                                             existing explicit preview consent
                                                          |
                                             Image2 first, then Tripo consent
```

`model_refinement.py` is a pure model-generation domain module. It knows report
warning codes and maps them to provider-independent outcomes such as wider
connections, larger bed contact, self-supporting forms, clearer silhouettes,
and broader material regions. It does not import Provider clients, HTTP code,
wxWidgets, Orca workspace classes, or slicer configuration.

The Sidecar reads the existing reports and computes advice when serializing the
public job. Because the function is deterministic, old jobs immediately gain
advice without a migration or a new persisted schema. Missing, malformed, or
passing reports produce unavailable/empty advice safely.

## Public contract

The optional `refinement` object contains:

```json
{
  "schema": 1,
  "available": true,
  "summary": "检测到 3 类可在下一次生成中改善的问题。",
  "prompt_suffix": "打印优化要求：加粗薄壁和连接处；使用更自支撑的斜面；减少零碎色块。",
  "issues": [
    {
      "code": "thin_local_wall_regions",
      "category": "geometry",
      "title": "局部薄壁或细连接",
      "instruction": "加粗薄壁、把手和连接颈，并使用圆滑过渡与主体可靠连接。"
    }
  ]
}
```

Rules:

- accept only known structural or visual codes;
- deduplicate multiple codes that map to the same action category;
- preserve a stable priority order: topology, detached parts, thickness, bed
  contact, overhang, silhouette/semantics, then color regions;
- expose at most six issues;
- cap every text field and the complete prompt suffix;
- never include file paths, Provider identifiers, secrets, or raw report text;
- visual-review-unavailable is not a design defect and produces no advice.

## GUI behavior

The model quality card gains a small "下一次生成优化" section. It shows a
short summary and an `应用到下一次生成` button only when advice is available.

Applying advice:

1. appends the exact bounded `prompt_suffix` to the current text input;
2. refuses duplicate insertion;
3. refuses insertion if the Sidecar 2,000-byte input limit would be exceeded;
4. leaves the current model, reports, and comparison view intact;
5. performs no network request;
6. tells the user to review the input and explicitly create a new Image2 preview.

Image-only jobs may gain a text instruction for the next image-assisted attempt.
Historical jobs without advice continue to behave as before.

## Failure and compatibility behavior

- Malformed optional advice is ignored by C++.
- Unknown issue codes are omitted rather than rendered as raw text.
- Sidecar unavailable behavior is unchanged.
- Old job JSON and old quality reports require no migration.
- No 3MF, profile, project, printer, material, or slicing default changes.
- No automatic support, placement, mesh modification, or slicing decision.
- `IModelArtifactConsumer`, `IPrintablePaletteProvider`, and
  `OrcaWorkspaceAdapter` are unchanged.

## Verification

- Unit tests cover warning-to-advice mapping, deduplication, stable priority,
  passing/invalid reports, visual advice, limits, and unknown codes.
- Sidecar contract tests prove the optional DTO appears for persisted reports
  and remains empty for a passing job.
- Existing offline model-generation tests must remain green.
- Windows Release build must pass.
- Repository-local Orca GUI verification must prove the advice is visible,
  applying it changes only the input, and no provider or Sidecar generation
  request occurs.

## Deferred work

- `ModelGenerationGateway` extraction is the next Phase 80 slice after this DTO
  is stable; this design keeps the DTO provider-neutral so the extraction does
  not require another UI contract change.
- The 8-12 case paid benchmark will use the advice only after local and mock GUI
  behavior is accepted.
- Multi-view reference strategies and physical print calibration remain later
  phases.
