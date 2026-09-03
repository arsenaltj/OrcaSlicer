# Internal Locked Installer Implementation Plan

**Goal:** Restore a single Windows internal installer that contains the verified Image2, text/vision, and Tripo provider configuration and starts the packaged Sidecar without coworker setup.

**Architecture:** Keep credentials in a package-only JSON payload generated from the release computer's persistent environment. CMake accepts and installs that payload only for the `internal` distribution channel; the installed bootstrap validates it, makes it authoritative inside the Sidecar process, and rejects it in `commercial` builds. The release wrapper and fast packager require the payload, validate the packaged runtime offline, and emit only credential-free metadata and logs.

**Tech Stack:** CMake/CPack/NSIS, PowerShell, Python 3.12 unittest, OrcaSlicer C++17/wxWidgets.

---

### Task 1: Restore the package-only internal configuration contract

**Files:**
- Modify: `tools/ai/create_internal_defaults.py`
- Modify: `tools/ai/orca_ai_installed_bootstrap.py`
- Modify: `tools/ai/test_create_internal_defaults.py`
- Modify: `tools/ai/test_installed_bootstrap.py`

**Steps:**

1. Add failing tests requiring complete PRO Image2, legacy text/vision, and Tripo configuration.
2. Run the two focused Python test modules and confirm the new expectations fail.
3. Extend the allow-list and required setting groups without logging values.
4. Run the focused tests and confirm they pass.

### Task 2: Re-enable internal-only CMake packaging

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify: `tools/ai/test_integration_guardrails.py`
- Modify: `scripts/verify_ai_integration.py`

**Steps:**

1. Change the guardrail test to require credentials for `internal` while continuing to reject them for `commercial`.
2. Run the guardrail test and confirm it fails against the current blanket prohibition.
3. Validate and install `ORCA_AI_INTERNAL_DEFAULTS_FILE` only for `internal` packages.
4. Run integration guardrails and the repository verifier.

### Task 3: Make the internal release path one-command and fail closed

**Files:**
- Modify: `release/build_internal.ps1`
- Modify: `scripts/package_internal_fast.ps1`
- Modify: `release/README.md`

**Steps:**

1. Resolve the allowed provider variables from process/user/machine scope without printing them.
2. Generate the locked payload under the ignored build directory before CMake configuration.
3. Require that payload in the fast packager and mark the manifest as `package_internal_locked`.
4. Add an offline staged-runtime validation that checks Python, Pillow, build identity, and defaults loading without contacting a provider.

### Task 4: Build and validate the coworker installer

**Files:**
- Generated only: `build/windows-installer/*`

**Steps:**

1. Run focused Python tests, all AI tests, C++ AI tests, and repository secret scanning.
2. Commit source changes so the release identity is exact and the worktree is clean.
3. Run `release/build_internal.ps1` to create the single EXE and portable diagnostic ZIP.
4. Inspect the package for runtime files and the locked payload without displaying credential values.
5. Launch the portable build against an isolated data directory, verify authenticated `/health`, Chinese UI, and Sidecar logs, and make no paid generation request.
6. Return the exact installer path, SHA-256, source SHA, and any remaining limitation.
