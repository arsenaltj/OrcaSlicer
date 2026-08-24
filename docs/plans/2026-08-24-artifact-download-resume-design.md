# Tripo Artifact Download Resume Design

## Problem

The installed AI runtime completed a paid Tripo generation and conversion, but the CDN connection ended after 4,403,811 of the declared 6,083,847 bytes. The response ended cleanly enough for `urllib` to return EOF, so the downloader renamed the partial file as complete. ZIP validation later reported the misleading error `Tripo returned an invalid OBJ package`.

## Design

Keep task creation and conversion unchanged. The artifact downloader now treats `Content-Length` and `Content-Range` as integrity boundaries, retains a partial file after a transient interruption, and makes at most three download attempts. A later attempt sends `Range: bytes=<downloaded>-`; safe redirects retain only the existing `Accept` header and this byte range. A valid `206` response must begin at the requested offset and declare a consistent total size. If a server ignores the range and returns `200`, the local partial file is replaced rather than appended.

Size limits, HTTPS/host allowlisting, redirect validation and final archive/model validation remain unchanged. Exhausted retries delete the partial file and return a specific incomplete-download error. No retry creates a Tripo generation or conversion task, so recovery does not duplicate paid work.

## Verification

- Unit tests cover interrupted-then-resumed download, repeated truncation cleanup, and range preservation across safe redirects.
- The real failed conversion ID is reused without creating a new task. A seeded 4,403,811-byte partial file must resume to 6,083,847 bytes and pass ZIP integrity.
- The recovered OBJ must pass existing palette and structural gates and render in all five offline review views.
- The patched module is deployed to the user-selected installation and loaded by a newly auto-started Sidecar.
