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
