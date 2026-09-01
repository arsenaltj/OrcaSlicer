# Restricted Release Publisher Design

## Goal

Allow an enrolled employee to upload a verified internal Windows installer
without receiving an administrator credential, interactive shell, website source
access, or generic command execution on the production server.

## Identity and authorization

Employee input is normalized only when the complete value is a numeric ID with
optional leading zeroes and at most one `s`/`S` prefix. The canonical ID is an
authorization label, not an authentication secret. Each employee has an
individual locked Linux account and an individual ED25519 public key stored only
on the server. The authorized key uses OpenSSH `restrict` plus a forced command
bound to the canonical employee ID.

## Upload protocol

The PowerShell client keeps the existing manifest/source/clean-worktree gates. In
restricted mode it first requests `status`, compares the server-returned employee
ID and protocol, and then streams the installer to the forced command over SSH.
Both the unprivileged dispatcher and root-owned promotion helper independently
validate the employee, safe filename, 1 GiB limit, exact size, SHA-256, source
commit, revision, and Windows `MZ` prefix.

The publisher can write only a mode-0700 incoming directory. The only sudo rule
targets the immutable root-owned promotion helper. That helper also checks
`SUDO_USER`, the resolved incoming path, ownership, link count, and destination
state. Existing files are accepted only when their size and hash already match;
new files use a same-directory hard-link commit so another publication cannot be
overwritten by a race. A syslog receipt records employee ID and artifact identity.

## Explicit boundary

This role uploads installers but does not deploy website code or metadata. The
website source worktree currently contains unrelated uncommitted changes, so a
forced site deployment could publish work outside the release request. Website
deployment stays with the administrator until it has a clean commit-addressed
pipeline. The existing administrator SSH configuration remains unchanged.
