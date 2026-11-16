# Team internal validation toolkit

Current scope (2026-09-11): build Windows EXE/portable ZIP packages for internal
team testing and verify their actual Orca main-window journeys. Public release,
website upload, publisher enrollment and commercial promotion are not part of
this workflow. Historical release plans are provenance, not current instructions.
Public/WinGet/nightly publishing and macOS signing/notarization CI steps have
been removed; cross-platform compilation and unsigned test artifacts remain.

## Local validation comes first

A full local application build and GUI validation do not require a commit, push,
PR, remote CI or a clean worktree. Reuse the fixed validation task and its own
source/build/runtime/configuration directories; see the
[handoff workflow](../Docs/coordination/validation-handoff.md).

Internal packages accept a base HEAD plus a complete handoff manifest. This
supports uncommitted modifications and a detached validation HEAD. Each added or
modified file has a repository-relative path, action, byte length and SHA-256;
deletions use action `delete`. Unrecorded program files and changed bytes fail
verification. Place temporary output in Git-ignored directories. Explicitly
excluded untracked research documents may be listed in `excluded_untracked_docs`.
A dirty checkout without a complete manifest must be corrected by completing the
snapshot, not by automatically committing or cleaning the user's worktree.

## Internal build entry point

Use an independently configured Windows CMake build with the normal dependencies
and NSIS. The wrapper reads its Python/CMake paths from that build and reconfigures
it for the internal installer. It does not require Git remotes or website/SSH
configuration.

For an explicitly requested Windows release candidate, use
`-DistributionChannel commercial`. This selects the commercial runtime identity,
omits the Internal filename label, and requires a fresh strict release integration
check; `-KnownIntegrationReport` exceptions are rejected. The default remains
`internal`. Building a candidate does not sign, upload or publish it; installation
and release acceptance must be recorded separately.

```powershell
& .\release\build_internal.ps1 -BuildDir build-validation -SourceManifest <handoff/manifest.json> -OutputDir <new-empty-output-dir> -ValidateOnly
& .\release\build_internal.ps1 -BuildDir build-validation -SourceManifest <handoff/manifest.json> -OutputDir <new-empty-output-dir>
```

`-ValidateOnly` checks source and local prerequisites without building, staging,
packaging or starting the program. It is not functional acceptance. Clean source
can omit `-SourceManifest`; snapshots use a distinct revision label. The wrapper
calls `scripts/package_internal_fast.ps1`, which checks configured source/runtime
identity, builds the application and packages EXE and ZIP. Existing platform
refusals still apply; this workflow is not an alternate route around them.

For local snapshot/preflight regression checks, run
`python -m unittest discover -s scripts -p test_package_source_identity.py -v`.
These use temporary Git fixtures and never build or launch Orca. On Windows they
also execute both PowerShell entry points with `-ValidateOnly`.

The output contains the packages, SHA-256 files, content reports,
`source-snapshot.json` and a schema-4 package manifest. The manifest records the
actual dirty state, snapshot identity and optional integration baseline (null when
unavailable). A changed source identity invalidates the attempt. Use a new empty
output directory each time and retain failed attempts for diagnosis.

Applicable test results may be reused only for the same tested source and binary
identities. The wrapper's `-SkipTargetedTests` is for an already documented matching
test result, not an acceptance shortcut. Actual GUI evidence remains required:
startup/service health, history/state recovery, relevant editing undo/redo,
color-import cancel/confirm and preparation-page handoff, plus ordinary Orca
regressions. Report package creation and GUI acceptance separately.

All distribution channels require the complete integration check with zero errors.
`-KnownIntegrationReport`, `--known-report`, and guardrail-test
`--internal-package-report` are retired and fail explicitly, even for a passing
candidate. Historical reports remain evidence only; they cannot authorize a new
package or set expected test failures. Source snapshots, full Git checks, exact
artifact identity, and ordinary budget thresholds remain mandatory.
The guardrail test runner also rejects its former `--source-manifest` companion;
source manifests remain supported by the package snapshot and validation tools.

## Package contents and tester configuration

Platform login uses the built-in public API endpoint `https://api.cutelion.top`.
Fresh installations do not need a developer's environment or configuration file.
An explicit `ORCA_PLATFORM_API_URL` overrides the saved profile configuration,
which takes precedence over the shared application configuration and then the
built-in default. Invalid explicit overrides remain errors. This default contains
no account, token or provider credentials; user sessions stay in the existing
per-user secure store and must never be copied into a package.

Internal shared packages contain no provider credentials, user images or generated
assets. Tester configuration stays outside the package and source control. The
package content inspector remains mandatory; its recorded scope and limitations
must accompany each artifact. It does not submit a generation request.

```powershell
python -I release/verify_package_contents.py <artifact.exe-or.zip> --report <report.json>
python -m unittest discover -s release -p test_verify_package_contents.py
```

`create_ai_provisioner.ps1` is a separate, explicitly authorized private Image2 PRO
configuration tool. Its output is not a package component or a public download;
Tripo configuration is separate. Do not commit real configuration files or keys.

Remote source submission, if separately requested, follows the
[team SOP](../Docs/coordination/team-integration-sop.md). Building an internal
package does not authorize sending it, pushing code or contacting testers.

## Native portrait recognition runtime

Portrait recognition uses the native MediaPipe runtime configured by
`ORCA_AI_PORTRAIT_RECOGNITION`, `ORCA_ENABLE_MEDIAPIPE_NATIVE`, and
`ORCA_SEMANTIC_RUNTIME_DIR`; see [the current Windows runtime guide](../Docs/coordination/portrait-dependencies-windows.md).
The existing sealed files, models, licenses, and provider checks remain required.
The capability manifest keeps `legacy_farl_enabled: false` for compatibility.

FaRL/Torch staging, inference subprocesses, raster DLL, and their installation
rules have been retired. A CMake cache that enables `ORCA_AI_LEGACY_FARL_RUNTIME`
or supplies `ORCA_BEAUTY_RUNTIME_ROOT` / `ORCA_BEAUTY_RASTER_BINARY` fails during
configuration. Do not copy old research runtimes into an installation. Existing
user runtime, weights, evidence, and historical appearance data are not deleted.

Content inspection supports bounded ZIP/TAR/gzip/bzip2/xz containers and the
646 MB pinned parsing model. Public examples in audited dependency files are
classified only for exact file hashes, paths and literals; other assignments,
changed files and token patterns remain subject to the normal checks.

Pass `-SevenZipExecutable <absolute-path-to-7z.exe>` when 7-Zip is not installed
in a standard location. Both preflights verify this path without executing it;
the content scanner receives it explicitly. Internal NSIS packages use zlib
compression to keep large offline-model builds practical, while the final
artifact names and manifests retain the full snapshot revision.
