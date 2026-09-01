# Restricted Release Publisher Implementation Plan

1. Extend the upload client with strict employee-ID normalization and a binary
   streaming SSH protocol while retaining read-only validation and administrator
   compatibility.
2. Add a forced-command dispatcher, root-owned promotion helper, and idempotent
   enrollment script under `release/server/` without committing real hosts or
   employee public keys.
3. Parse PowerShell and Bash, exercise accepted/rejected employee forms, and scan
   the exact diff for secrets.
4. Audit the production server read-only, enroll the supplied public key, validate
   account/key/sudoers/sshd state, and run status plus command-rejection tests.
5. Exercise the full server protocol idempotently with the already-published
   installer, verify that its hash is unchanged and incoming storage is empty,
   then confirm the website service and health endpoint remain healthy.
6. Commit only the generic release toolkit changes, fetch the shared branch again,
   and push normally without force.
