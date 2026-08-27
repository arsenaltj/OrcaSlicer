# Internal package localization and configuration assistant design

## Problem

The fast internal NSIS path packaged an existing verified Release build without
running `scripts/run_gettext.bat`. As a result, `resources/i18n` contained no
compiled message catalogs and a configured `zh_CN` UI fell back to English.

The configuration assistant itself was present. OrcaSlicer intentionally skips
the startup assistant when the existing user data already contains a completed
guide and a selected printer. The installer preserves that data to avoid losing
printer, filament, and process presets.

## Chosen design

1. Add one reproducible PowerShell entry point for fast internal packaging. It
   compiles localization catalogs before invoking CPack, checks the Simplified
   and Traditional Chinese catalogs, uses a short temporary CPack name to avoid
   Windows/NSIS path-length failures, and writes a SHA-256 sidecar file.
2. Add an install-time guard, enabled only for the AI Windows installer, that
   fails packaging if either Chinese catalog is absent from the staged package.
3. Add a thin top-menu item that calls the existing
   `GUI_App::run_wizard(ConfigWizard::RR_USER)` flow. It does not mutate or
   delete user data and works on all desktop platforms.

## Rejected alternatives

- Deleting `%APPDATA%\OrcaSlicer` during install would make the guide reappear,
  but would also risk deleting accepted printer and material configuration.
- Giving the AI build a different default data directory would duplicate
  presets and make upgrades from upstream OrcaSlicer confusing.
- Forcing the guide on every application update would interrupt normal users
  and change upstream startup behavior.

## Verification

- Build the changed GUI target in Release mode.
- Run localization generation and assert both Chinese `.mo` files exist.
- Package through the new fast-package entry point and inspect the installer.
- Launch the extracted package with an existing data directory and with a fresh
  isolated data directory; verify Chinese UI loading and the fresh-data guide.
- Run targeted C++ tests and the existing Python AI suite. No paid API calls are
  required.
