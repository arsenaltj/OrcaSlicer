# AI integration baseline manifest

## Official source

| Field | Value |
|---|---|
| Remote | `https://github.com/OrcaSlicer/OrcaSlicer.git` |
| Branch | `upstream/main` |
| Pinned commit | `6ef02a67dbb22ae1a019d9f485f46bfc3e1b44aa` |
| Commit date | `2026-08-20T22:35:28-03:00` |
| Commit subject | `Revert "Fix unstable contours from triangulated planar faces" (#15315)` |
| Integration branch | `codex/orca-integration-v2` |

The integration branch was created directly from the pinned commit and tracks `upstream/main`. No feature code is included in this baseline.

## Accepted feature inputs

| Feature | Live development branch | Accepted commit | Acceptance status |
|---|---|---|---|
| Model generation | `codex/model-generation` | `db81edc2b028c37822c997e99eb88c0465089f0c` | Accepted for integration on 2026-08-27 |
| Smart slicing | `codex/smart-slicing` | `1c163d68906e287b946b40a975feb3bfd9aab68d` | Accepted for integration on 2026-08-22 |

An observed branch head is not an accepted input. The accepted commit is written here only after the user confirms the corresponding GUI behavior.

## Ownership boundaries

### Model generation owns

- model-generation Domain/GUI code;
- printable image and model-quality pipelines;
- provider adapters and model-generation Sidecar modules;
- its focused tests and documentation.

### Smart slicing owns

- SmartSlicing Domain/Application/Ports;
- preflight, candidates, trial-slice orchestration and transaction contracts;
- SmartSlicing presenter/view model/panel and Orca adapter;
- its focused tests and documentation.

### Integration owns

- shared Orca GUI composition and navigation;
- build registration that includes both modules;
- cross-feature DTO/Port adaptation;
- runtime isolation defaults and combined verification;
- upstream merges and conflict resolution.

### Shared runtime (integration approval required)

- `AIServiceManager` and `AISidecarClient` lifecycle/protocol/authentication;
- loopback-only native HTTP transport and shared Python network policy;
- diagnostics, installed bootstrap and package/build identity;
- release/build workflows and the machine-readable integration lock.

## Forbidden dependency directions

- Model generation must not include or invoke SmartSlicing GUI/Application implementation.
- Smart slicing must not include provider task objects or ModelGenerationPanel implementation.
- Sidecar code must not mutate Orca Model, Config, 3MF or formal slicing state.
- Domain/Application code must not include wxWidgets, `Plater` or concrete provider SDKs.
- Feature code must not introduce persistent 3MF/profile fields without an explicit migration decision and compatibility tests.

## Baseline verification

| Gate | Status | Evidence |
|---|---|---|
| Official history and tracking | Passed | HEAD and merge base equal the pinned SHA |
| Clean worktree | Passed | No feature or local source changes before this manifest |
| CMake configure | Passed | Ninja, MSVC 19.44.35227, Windows SDK 10.0.26100.0, `BUILD_TESTS=ON`, `ORCA_TOOLS=ON` |
| Windows Release build | Passed | Full build completed; incremental confirmation returned `ninja: no work to do` with exit code 0 |
| Baseline tests | Passed with documented exclusions | 493/493 offline/code tests passed; three upstream `[NotWorking]` HTTP tests excluded; two Orca Cloud session tests independently reproduce a 30-second timeout |
| Isolated startup smoke | Passed | No other Orca process was running; GUI remained alive for 20 seconds with a build-local `--datadir` and empty stderr |

`--no-single-instance` is scanned by `InstanceCheck` but is not registered by the current command-line parser, so the official baseline rejects it as an invalid option. Integration smoke tests therefore first assert that no other Orca process is running and then use an isolated `--datadir`.

## Integration cycle 1

- Official source: `6ef02a67dbb22ae1a019d9f485f46bfc3e1b44aa`
- Baseline manifest commit: `ac87750d2c87efd2a1123d3c51ef881aa276fa5c`
- Model-generation source: `61f7b13e3e2f0acfbffcc7388911b302cd0f16ba`
- Smart-slicing source: `1c163d68906e287b946b40a975feb3bfd9aab68d`

The feature repositories have no merge base with the official history. Cycle 1 therefore ports the final accepted module snapshots, adapts shared Orca touchpoints manually, and records the source SHAs in every port commit. Later cycles consume only newly accepted SHAs and compare them with the previous accepted snapshot.

## Integration cycle 2

- Previous accepted model-generation source: `61f7b13e3e2f0acfbffcc7388911b302cd0f16ba`
- Reviewed model-generation checkpoint: `4b68ec1e12180afd18fc9dd60891e71425dea3c7`
- Accepted model-generation source: `ef9a99491be558cd688d5cc799ea258b507a80fc`
- Smart-slicing source remains: `1c163d68906e287b946b40a975feb3bfd9aab68d`

Cycle 2 ports the complete model-generation delta from the previously integrated source to the accepted target. It retains integration-owned diagnostics, artifact-download recovery, packaged internal configuration and smart-slicing composition. The moving branch head is not consulted after the accepted target is resolved.

## Integration cycle 3

- Fixed upstream source: `6fdd4945c19348cc5fc9ed9ae2f26f22a778786b`
- Previous accepted model-generation source: `ef9a99491be558cd688d5cc799ea258b507a80fc`
- Accepted model-generation source: `db81edc2b028c37822c997e99eb88c0465089f0c`
- Smart-slicing source remains: `1c163d68906e287b946b40a975feb3bfd9aab68d`
- Integration merge commit: `fc467b007fbfbe967aeaad99006b18ee0b589467`
- Accepted-snapshot integration receipt commit: `c1cdfece8752d24637ec9d62edc70f2101455b1f`

Cycle 3 records an explicit upstream intake and the next fixed model-generation snapshot. Smart slicing is unchanged. These identities describe source lineage only; build, test, GUI, installer and commercial-readiness evidence must be recorded separately against the final candidate and must not be inferred from this manifest.

The v2 machine-readable lock adds a historical receipt for snapshot-ported feature history. At `c1cdfece8752d24637ec9d62edc70f2101455b1f`, the model-generation Domain tree and three unchanged native interface objects exactly match `db81edc2b028c37822c997e99eb88c0465089f0c`; the SmartSlicing Domain and GUI adapter trees exactly match `1c163d68906e287b946b40a975feb3bfd9aab68d`. The validator resolves both sides of every recorded path and compares their Git object IDs. Shared/composed files are intentionally not claimed as byte-identical receipts; they remain subject to integration review and combined tests.

## Future integration checklist

1. Fetch and pin an upstream SHA; never integrate against a moving reference.
2. Build the pure official baseline before feature porting.
3. Record both user-accepted feature SHAs.
4. Port module-owned files first and shared Orca touchpoints last.
5. Keep upstream sync, each feature port and composition cleanup in separate commits.
6. Run focused tests after each port and the full combined release gates at the end.
7. Present the local integration candidate before any push or publication.
