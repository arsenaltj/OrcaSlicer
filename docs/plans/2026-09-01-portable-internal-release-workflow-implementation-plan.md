# Portable Internal Release Workflow Implementation Plan

> **For the release-toolkit implementer:** Complete and verify each task in order; keep machine-specific and secret values outside the public repository.

**Goal:** Add a safe, portable operator kit for building, uploading, deploying, and verifying internal Windows releases from different developer computers.

**Architecture:** Keep the existing package script authoritative and add thin PowerShell wrappers under `release/`. Machine and credential values remain local; committed files contain only parameters, examples, public URLs, and non-secret server directory contracts.

**Tech Stack:** PowerShell 7/Windows PowerShell, CMake/CPack, Git, SSH/SCP, Python/pytest, Catch2, Flask website deployment.

---

### Task 1: Local configuration boundary

**Files:**
- Create: `release/config.example.ps1`
- Modify: `.gitignore`

1. Define build directory, defaults file, optional NSIS/CMake tools, SSH target, website worktree, public URL, and remote paths as operator variables.
2. Leave SSH target and sensitive local paths blank or illustrative.
3. Ignore `release/config.local.ps1` and `release/*.local.json`.
4. Verify with `git check-ignore release/config.local.ps1 release/test.local.json`.

### Task 2: Portable build wrapper

**Files:**
- Create: `release/build_internal.ps1`

1. Resolve the repository root from `$PSScriptRoot` and normalize relative parameters against it.
2. Require the expected branch, a clean worktree, a stable full HEAD, an existing matching CMake cache, and an external defaults file.
3. Resolve CMake from an explicit parameter, the cache, PATH, or Visual Studio installations.
4. Reconfigure with explicit single-string `-D` arguments and invoke `scripts/package_internal_fast.ps1`.
5. Run Python guardrails and exact Catch2 tags unless skipped.
6. Add `-ValidateOnly` to exercise discovery without building.

### Task 3: Verified artifact upload

**Files:**
- Create: `release/upload_installer.ps1`

1. Load the package manifest and require the installer beside it.
2. Recompute local size/SHA-256 and require source identity to match the current clean HEAD unless explicitly validating an older artifact.
3. Validate the SSH target, owner/group, filename, and absolute remote directory as shell-safe tokens.
4. Upload to a unique `/tmp` name, verify size and SHA-256 remotely, then use `install` for the final file.
5. Add `-ValidateOnly` so artifact validation performs no network action.

### Task 4: Public verification and operator runbook

**Files:**
- Create: `release/verify_public_release.ps1`
- Create: `release/README.md`

1. Verify homepage metadata, absence of a login prompt, anonymous 16-byte Range response, `MZ`, full Content-Range size, checksum header, and `/healthz`.
2. Document build prerequisites, configuration, commands, website metadata files, safe candidate copying with `current/.`, tests, deploy command, rollback, and final report fields.
3. State that the repository is public and real connection/default/provider data must remain local.

### Task 5: Verification, commit, and push

**Files:**
- Test: all `release/*.ps1`, documentation, `.gitignore`

1. Parse every PowerShell script with the PowerShell parser and require zero errors.
2. Run build/upload validation-only modes against the current configuration and latest manifest.
3. Run the public verifier against the currently published release.
4. Run `git diff --check` and secret-pattern scans on the exact staged files.
5. Fetch `origin`, ensure no unexpected divergence, commit only the release kit/design/plan/ignore rule, and push without force.
