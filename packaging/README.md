# Packaging

Six target artifacts are planned for M5:

| Target | Artifacts |
|---|---|
| Windows x64 / arm64 (MinGW) | Inno Setup `setup.exe`, portable `.zip` |
| Linux x64 / arm64 | `.deb`, `.rpm`, `.tar.gz` |
| macOS x64 / arm64 | `.pkg`, `.dmg`, `.zip` |

The brand icon source is `resources/branding/uvnvr.svg`; regenerate all formats with
`scripts/generate-icons.sh`. Packaging scripts live under `packaging/windows`,
`packaging/linux` and `packaging/macos`.

Startup configuration is installed by each package:

- Windows: the Inno Setup **Service ports** page writes
  `%ProgramData%\UvNVR\uvnvr.json` and the boot task runs with `--config`.
  The installer provides English and Simplified Chinese, creates desktop
  shortcuts for the web UI and the application, and adds/removes the firewall
  rule and boot task. The x64 MSI (`packaging/windows/msi`) uses
  `HTTPPORT`/`RTSPPORT` properties and the same config/shortcut/task behavior.
  Build it with:
  ```
  ISCC.exe /DMyAppVersion=<version> /DMyArch=x64 /DSourceDir=<stage> packaging\windows\uvnvr.iss
  ```
  The script is `packaging/windows/uvnvr.iss`; the Chinese language file is
  `packaging/windows/ChineseSimplified.isl`.
- Linux: the post-install script creates `/etc/uvnvr/uvnvr.json`; the systemd unit
  runs with `--config`.
- macOS: the pkg post-install script creates
  `/Library/Application Support/UvNVR/uvnvr.json`; launchd passes `--config`.
- The portable Windows zip contains `uvnvr.json` next to `bin\`.

Existing configuration files are preserved on upgrade. See
[`docs/configuration.md`](../docs/configuration.md).

After a release is published, verify the complete asset set and checksums with:

```bash
scripts/verify-release.sh 0.1.0
```

It downloads the attached assets, checks every expected filename, validates
`SHA256SUMS` and parses the SPDX SBOM.
