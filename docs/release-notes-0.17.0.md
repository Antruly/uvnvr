# UvNVR 0.17.0

Installer and UI refresh.

## Highlights

- **Modern Windows installer.** The NSIS setup is replaced by Inno Setup 6 for
  both x64 and arm64:
  - English and Simplified Chinese wizard languages, stored as UTF-8 with a
    bundled `ChineseSimplified.isl`, with a working language-selection dialog.
  - Service-ports page writes `%ProgramData%\UvNVR\uvnvr.json` only when no
    configuration exists; upgrades keep user settings.
  - Windows Firewall rule and boot scheduled task (`schtasks`) are created and
    removed on uninstall.
  - Start-menu entries plus **desktop shortcuts** for the web UI and the
    application; the desktop task is checked by default.
  - Uninstall preserves `%ProgramData%\UvNVR` (database, recordings, secrets).
- **ONVIF discovery bulk add.** Scan results have row checkboxes, a select-all
  toggle and **Add selected (n)**. Duplicate hosts are skipped, progress and
  errors are shown per row, and the add loop does not stop on one failure.
- **Fixed the scan-to-channel flow.** **Probe** now closes the scan dialog before
  opening the channel form (previously the form opened behind it), and errors no
  longer appear on the page hidden by the modal.
- **UI refresh.** New surface hierarchy, spacing, panels, buttons, inputs,
  tables, status pills, modal and login composition; redesigned icon set; the
  dashboard hero shows live counters instead of placeholder text.
- **Light theme completed.** Sidebar, top bar, tabs, token form, scope tree,
  status pills, notices, modals and authentication panels use theme tokens
  instead of dark hard-coded colors. The API-token scope list no longer shows raw
  `system.scopes.*` keys.
- Windows console output already switches to UTF-8 at the start of `main`
  (`SetConsoleOutputCP(CP_UTF8)` / `SetConsoleCP(CP_UTF8)`).

## Verification

- The Inno Setup script compiles under Inno Setup 6.7.3 for x64 and arm64.
- Scan bulk add was exercised in a headless browser with mocked discovery and
  probe responses: both devices were added, marked **Added**, and the success
  count was shown.
- The light and dark dashboards, channels, storage, system, alerts and login
  screens were captured and inspected.

## Upgrade notes

- The installer name is unchanged (`uvnvr-<version>-windows-<arch>-setup.exe`);
  it is now produced by Inno Setup.
- Upgrading over an NSIS installation is supported by installing into the same
  directory; the data directory is preserved.
- CI/release jobs now install Inno Setup and run `ISCC.exe` instead of
  `makensis`.

## Known limitations

- Inno Setup installs require administrator privileges for the firewall rule and
  boot task.
- Windows arm64 uses the same script with `/DMyArch=arm64`; code signing is not
  configured, so SmartScreen may warn.

## Packages

Windows x64/arm64, Linux x64/arm64 (deb/rpm/tar.gz), macOS x64/arm64
(pkg/dmg/zip), plus the x64 MSI, SHA256SUMS, SPDX SBOM and FFmpeg source/license
assets.
