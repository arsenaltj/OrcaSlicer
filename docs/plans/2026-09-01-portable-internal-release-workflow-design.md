# Portable Internal Release Workflow Design

## Goal

Make the existing Windows internal-release process usable from a different developer computer without committing machine-specific paths, SSH connection details, provider credentials, or internal defaults to the public repository.

## Structure

The repository gains a top-level `release/` operator kit. `build_internal.ps1` resolves the repository from its own location, accepts the build directory and external defaults file as parameters, locks a clean `codex/orca-integration-v2` HEAD, reconfigures the existing CMake build with an exact dated revision, invokes the authoritative `scripts/package_internal_fast.ps1`, and optionally runs the focused Python and Catch2 release checks. `upload_installer.ps1` accepts a manifest and a locally supplied SSH target, independently checks the installer size and SHA-256, uploads to a unique temporary path, repeats both checks remotely, and only then installs the file into the configured download directory. `verify_public_release.ps1` verifies the anonymous page, Range response, PE magic, total size, checksum header, and health endpoint.

`config.example.ps1` documents every operator-supplied value while leaving sensitive connection data blank. `.gitignore` excludes `release/config.local.ps1` and local JSON variants. `README.md` covers prerequisites, the two-command happy path, website metadata changes, candidate tests, deployment, rollback, concurrent-branch safety, and known PowerShell/symlink pitfalls.

## Safety and testing

Scripts fail closed on a dirty or moving source tree, mismatched manifest identity, unsafe filenames or remote paths, missing tools, and remote checksum mismatches. Validation-only modes exercise local discovery and manifest checks without compiling or connecting to a server. Tests include PowerShell parser checks, validation-only runs against the latest manifest, the public endpoint verifier, `git diff --check`, and staged secret-pattern scanning. Pushes are ordinary non-force pushes after fetching and confirming the shared branch has not diverged.
