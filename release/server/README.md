# Restricted publisher server components

These files implement the `release/upload_installer.ps1 -RestrictedPublisher`
protocol. They do not contain a server address, employee public key, or password.

An administrator copies this directory to a root-only temporary directory on the
server, then enrolls a publisher by sending exactly one ED25519 public-key line to
standard input:

```bash
./install-restricted-publisher 12345 < employee-12345.pub
```

The installer creates the padded account `s00012345`, locks its password, and
places the supplied key in a mode-0600 `authorized_keys` file with OpenSSH
`restrict` and a forced command. The forced command accepts only:

- `status CANONICAL_EMPLOYEE_ID`
- `begin` with the fixed artifact identity and chunk size
- `append` with a fixed offset, byte count, and per-chunk SHA-256
- `commit` with the same fixed artifact identity

It rejects an interactive shell, arbitrary commands, mismatched employee IDs,
unsafe filenames, files over 1 GiB, non-EXE content, and source/hash/size format
errors. Upload state is committed only after a complete 1-8 MiB chunk passes its
checks. `begin` truncates an interrupted partial append back to the last committed
offset, and the client can safely rerun the same upload to resume. A conflicting
artifact identity is rejected instead of overwriting partial state. Stable error
messages include an `error_stage` value for server-side diagnosis.

The unprivileged account can write only its private incoming directory. A
root-owned promotion helper revalidates the completed file before atomically
placing it under `/srv/3dprint-beer/data/downloads` as `web:web:0644` and logging
the employee ID, source commit, revision, size, and SHA-256 through syslog.

The sudo rule grants only the root-owned promotion helper. That helper also
requires `SUDO_USER` to match the employee-bound account and requires the source
file to be a single-link regular file owned by that account inside its incoming
directory.

This role intentionally cannot edit or deploy website source. Website metadata
deployment remains an administrator operation until the separate website
repository has a clean, commit-addressed deployment path. This prevents unrelated
worktree changes from being published accidentally.

To revoke the publisher, an administrator should lock the account, remove its
authorized key and sudoers entry, validate sudoers/sshd, and retain audit logs:

```bash
passwd -l s00012345
rm -f /var/lib/3dprint-publishers/12345/.ssh/authorized_keys
rm -f /etc/sudoers.d/3dprint-release-s00012345
visudo -c
sshd -t
```
