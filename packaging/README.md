# Packaging

Six target artifacts are planned for M5:

| Target | Artifacts |
|---|---|
| Windows x64 / arm64 (MinGW) | NSIS `setup.exe`, portable `.zip` |
| Linux x64 / arm64 | `.deb`, `.rpm`, `.tar.gz` |
| macOS x64 / arm64 | `.pkg`, `.dmg`, `.zip` |

The brand icon source is `resources/branding/uvnvr.svg`; regenerate all formats with
`scripts/generate-icons.sh`. Packaging scripts live under `packaging/windows`,
`packaging/linux` and `packaging/macos`.

Startup configuration is installed by each package:

- Windows: the NSIS **Service ports** page writes
  `%ProgramData%\UvNVR\uvnvr.json` and the boot task runs with `--config`.
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
