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

After a release is published, verify the complete asset set and checksums with:

```bash
scripts/verify-release.sh 0.1.0
```

It downloads the attached assets, checks every expected filename, validates
`SHA256SUMS` and parses the SPDX SBOM.
