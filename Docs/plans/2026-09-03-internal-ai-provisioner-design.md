# Internal AI Provisioner Design

## Goal

Give an internal tester one small ZIP that configures the verified Image2 PRO
provider for the current Windows user without requiring administrator rights or
manual environment-variable editing.

## Design

The public repository contains only a bundle generator and reviewable install
and removal scripts. The generator reads `OPENAI_PRO_API` from the build
operator's effective Windows environment, validates the configured HTTPS base
URL, and writes a credential-bearing payload only under an ignored build output
directory. It then creates a ZIP, a SHA-256 checksum, and a credential-free
manifest tied to the current source commit.

The recipient extracts the ZIP and double-clicks `Install-OrcaAIConfig.cmd`.
The installer validates the payload, writes `OPENAI_PRO_API` and
`OPENAI_PRO_URL` to the current user's persistent environment, verifies the
write without printing either value, broadcasts the Windows environment-change
notification, and performs a non-billable DNS/TCP reachability check. It writes
only status, provider host, and timestamps to a local diagnostic log. A matching
removal command deletes only the two user-level values installed by this bundle.

## Boundaries

- The API key is deliberately extractable from the generated internal bundle;
  this is distribution control, not cryptographic secret protection.
- Generated payloads and ZIPs are ignored by Git and must never be uploaded to
  the public download site.
- The provisioner does not modify OrcaSlicer binaries, installation files,
  ports, 3MF/profile formats, printer defaults, or provider retry behavior.
- Installation never submits a provider request and therefore cannot create a
  paid image or 3D generation job.
- User-level configuration avoids UAC. A running OrcaSlicer process must be
  fully closed and reopened before it sees the new variables.

## Verification

- Generate a bundle from a synthetic key and assert that command output,
  manifest, checksum, and README do not contain it.
- Extract the ZIP and validate the payload through the real installer in
  `-ValidateOnly` mode without writing the registry.
- Reject missing keys, malformed payloads, unexpected fields, non-HTTPS URLs,
  and control characters.
- Parse every PowerShell file and run the focused Python tests without making
  network or provider calls.
