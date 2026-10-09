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

For an explicitly requested internal test package with already documented architecture
diff budget findings, `-KnownIntegrationReport <prior-full-check.json>` records a
narrow exception. It requires `-SourceManifest`, the same source HEAD, complete Git
checks, and exactly the same finding codes and messages. Clean committed source
and complete uncommitted snapshots may both use this exception. The internal
packager explicitly supplies `--channel internal`; the standalone check defaults
to `release`, which cannot use budget exceptions. Every check still runs; new
findings, changed budgets, non-budget failures and release packages remain blocked.
`integration-check.json` and the package manifest preserve the actual failed
integration result, source cleanliness and channel. An exception is labelled
`internal-validation`, including when built on the integration branch. This option
does not approve remote integration or public release and does not change the
architecture lock.

The wrapper also supplies the verified internal report to the guardrail tests.
Their repository-budget and JSON CLI assertions then require exactly the recorded
findings, including failure status; all other tests remain unchanged. Running the
test suite normally still requires zero findings. The recorded package source
identity is checked before using these expectations.

## Package contents and tester configuration

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

## Offline beauty runtime for the Windows internal candidate

For the UX branch, follow the [Windows portrait dependency and build guide](../Docs/coordination/portrait-dependencies-windows.md).
Run `python scripts/portrait_dependencies.py prepare`, then configure with its
generated CMake initial cache. This supplies `ORCA_AI_WEIGHTS_DIR`,
`ORCA_AI_PORTRAIT_SITE_PACKAGES` and the native semantic runtime from a fixed,
file-hashed dependency attachment. The package includes four weights and the
verified CPython 3.12 x64 CPU stack. Application Python modules and the raster
DLL continue to come from the current source build, not the dependency archive.

The older `scripts/stage_beauty_runtime.py` / `ORCA_BEAUTY_RUNTIME_ROOT` path does
not provision these newer installer inputs. Do not use another developer's
absolute cache paths. Runtime libraries, weights and licenses are installed under
`resources/beauty-runtime`; no user configuration or provider credentials belong
in the dependency attachment. A user `local_semantic_runtime.json` still overrides
automatic bundled runtime discovery, including explicit opt-out.

Content inspection supports bounded ZIP/TAR/gzip/bzip2/xz containers and the
646 MB pinned parsing model. Public examples in audited dependency files are
classified only for exact file hashes, paths and literals; other assignments,
changed files and token patterns remain subject to the normal checks.

Pass `-SevenZipExecutable <absolute-path-to-7z.exe>` when 7-Zip is not installed
in a standard location. Both preflights verify this path without executing it;
the content scanner receives it explicitly. Internal NSIS packages use zlib
compression to keep large offline-model builds practical, while the final
artifact names and manifests retain the full snapshot revision.
