# Internal Release Toolkit

This directory is the portable runbook and script entry point for building and
publishing the Windows internal release from `codex/orca-integration-v2`.
All paths that vary by computer are supplied through a local configuration file.

## Public repository boundary

This GitHub repository is public. Commit only scripts, examples, and the
non-secret server directory contract. Never commit:

- a real SSH target, account, port, private key, or password;
- provider/API keys or the internal defaults payload;
- `release/config.local.ps1` or any `release/*.local.json` file;
- generated installers, manifests, build directories, or deployment logs.

The internal defaults file must stay outside this Git worktree. The build script
checks that requirement and records only its SHA-256 hash in validation output.

## Files

| File | Purpose |
| --- | --- |
| `config.example.ps1` | Machine-specific configuration template |
| `build_internal.ps1` | Locks the source identity, builds, tests, and validates the installer |
| `upload_installer.ps1` | Verifies the manifest and uploads the exact installer atomically |
| `verify_public_release.ps1` | Checks the public page, range download, checksum header, and health endpoint |
| `server/` | Root-owned forced-command protocol and publisher enrollment template |

## One-time setup on each Windows computer

Prerequisites are the normal OrcaSlicer Windows build dependencies, NSIS, Git,
OpenSSH (`ssh`/`scp`), and access to the separately managed website repository
and server. Configure the dependency tree and a CMake build directory by following
the Windows build documentation linked from the repository root `README.md`.
The wrapper intentionally does not download toolchains or private configuration.

From the repository root:

```powershell
Copy-Item .\release\config.example.ps1 .\release\config.local.ps1
notepad .\release\config.local.ps1
```

Set `InternalDefaultsFile` to an absolute path outside the repository. Set the
local SSH alias or `user@host` in `SshTarget`; SSH host, port, and key details
belong in the operator's local OpenSSH config. Set `WebsiteWorktree` to the
website checkout on that computer. The local config is ignored by Git.

For an enrolled restricted publisher, set `EmployeeId` to the assigned employee
number and keep `RestrictedPublisher = $true`. Accepted aliases are the exact
numeric ID, a zero-padded ID, or one `s`/`S` prefix; the client sends only the
canonical numeric ID. The ID selects authorization but is not a credential—the
matching private key remains required.

## Build and upload

Always start from the official integration branch, pull the newest remote commit,
and make sure the worktree is clean. Do not build while another process is
changing the same checkout.

```powershell
git switch codex/orca-integration-v2
git pull --ff-only origin codex/orca-integration-v2
git status --short

. .\release\config.local.ps1
& .\release\build_internal.ps1 @BuildRelease -ValidateOnly
$buildResult = & .\release\build_internal.ps1 @BuildRelease
$manifestPath = $buildResult.Manifest

& .\release\upload_installer.ps1 -ManifestPath $manifestPath @UploadRelease -ValidateOnly
& .\release\upload_installer.ps1 -ManifestPath $manifestPath @UploadRelease
```

The build script rejects the wrong branch, a dirty worktree, a build cache from
another checkout, and source changes made during packaging. It runs the existing
authoritative `scripts/package_internal_fast.ps1`, Python integration guardrails,
the AI integration verifier, and focused model-generation/smart-slicing tests.
It then checks the manifest identity, SHA-256, optional 7-Zip integrity, and
Authenticode status.

The preferred restricted mode verifies the server-bound employee identity, then
streams the installer through a forced SSH command. The unprivileged server
account checks filename, size, SHA-256, source identity, revision, and the `MZ`
header; a root-owned helper repeats those checks before an atomic no-overwrite
installation. The legacy administrator mode still uses SCP and explicitly
configured owner/group values. `-AllowSourceMismatch` exists only for
`-ValidateOnly` inspection of an older artifact; never use it for upload.

## Restricted publisher enrollment

The server administrator follows `release/server/README.md`. The employee sends
only one ED25519 `.pub` line. The key is stored only in that employee's server-side
`authorized_keys`, with OpenSSH `restrict` and a forced command; neither the key
nor the real server address belongs in this public repository.

The resulting role is intentionally `installer-upload` only. It cannot open a
shell, forward ports or agents, edit the website checkout, run the deployment
script, or change homepage metadata. Those website operations remain an
administrator step while the separate website worktree contains uncommitted work.

## Update the website metadata

The website is a separate repository. Do not copy it into this repository and do
not reset unrelated changes in its worktree. Update only these website files:

- `app/public.py`
- `app/templates/home.html`
- `tests/test_public.py`

Use the new manifest as the source of truth for filename, version/revision,
display label, byte size, SHA-256, CST build time, and full source commit. Derive
the “本次更新” list from the actual Git range between the previous published
source commit and the new manifest source commit; list four concise, user-visible
changes. Keep anonymous download enabled and retain the unsigned-build warning.

Before changing the website, make a targeted backup of those three files. Run its
tests in the local website checkout, then prepare an isolated server candidate.
On the server, create the candidate from the resolved release directory:

```bash
current=$(readlink -f /srv/3dprint-beer/current)
candidate=/tmp/3dprint-site-candidate-REVISION
test "${current#/srv/3dprint-beer/releases/}" != "$current"
install -d -m 0755 "$candidate"
cp -a "$current/." "$candidate/"
test -d "$candidate/app"
test ! -L "$candidate"
```

Never run `cp -a /srv/3dprint-beer/current "$candidate"`: `current` is a symlink,
and copying it as the candidate can make a supposedly isolated edit touch the live
release. Replace `REVISION` with the manifest revision and update only the targeted
files inside the candidate. Then run:

```bash
/home/web/3dprint-web/.venv/bin/python -m pytest -q
```

## Server layout and deployment

The non-secret production layout is:

- website source: `/home/web/3dprint-web`
- downloads: `/srv/3dprint-beer/data/downloads`
- current symlink: `/srv/3dprint-beer/current`
- immutable releases: `/srv/3dprint-beer/releases`
- deploy script: `/home/web/bin/deploy-3dprint-beer`
- service user: `web`

After candidate tests pass and the targeted website source files are updated,
deploy through the maintained script under the service user's systemd session:

```bash
web_uid=$(id -u web)
runuser -u web -- env \
  XDG_RUNTIME_DIR=/run/user/$web_uid \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$web_uid/bus \
  bash /home/web/bin/deploy-3dprint-beer
```

Do not manually change the `current` symlink. If deployment verification fails,
restore only the three targeted website files from the backup, run the tests, and
redeploy through the same script.

## Public verification

From the OrcaSlicer repository root:

```powershell
. .\release\config.local.ps1
& .\release\verify_public_release.ps1 -ManifestPath $manifestPath @PublicRelease
```

The verifier requires the page to show the exact filename, source identity, CST
build time, update section, and anonymous download button. It also verifies a
16-byte HTTP range response with an `MZ` prefix, total file size, checksum header,
and `/healthz`. Finally inspect the page once at desktop width and once around
390 px to catch layout regressions.

Record the final source commit, revision, installer filename, byte size, SHA-256,
upload destination, public URL, test results, and website deployment result in the
release handoff. Report any skipped gate explicitly.
