$ErrorActionPreference = 'Stop'
$Version = if ($env:UVNVR_VERSION) { $env:UVNVR_VERSION } else { '0.1.0' }
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$Uvcpp = "$env:RUNNER_TEMP\uvcpp\libuvcpp-1.8.1-msvc-arm64"
$Ffmpeg = "$env:RUNNER_TEMP\ffmpeg"
$Out = "$Root\dist-packages\uvnvr-$Version-windows-arm64"
New-Item -ItemType Directory -Force -Path "$Out\bin", "$Out\share\uvnvr\frontend\dist" | Out-Null
Copy-Item "$Root\build\Release\uvnvr.exe" "$Out\bin\uvnvr.exe"
$ffmpegExe = Get-ChildItem -Path $Ffmpeg -Recurse -Filter ffmpeg.exe | Select-Object -First 1
Copy-Item $ffmpegExe.FullName "$Out\bin\ffmpeg.exe"
Copy-Item "$Root\frontend\dist\*" "$Out\share\uvnvr\frontend\dist\" -Recurse
Copy-Item "$Uvcpp\bin\*.dll" "$Out\bin\" -Force
$vcpkgBin = "$Root\build\vcpkg_installed\arm64-windows\bin"
if (Test-Path $vcpkgBin) { Copy-Item "$vcpkgBin\*.dll" "$Out\bin\" -Force }
Copy-Item "$Root\LICENSE", "$Root\THIRD_PARTY_NOTICES.md" "$Out\"
Compress-Archive -Path "$Out\*" -DestinationPath "$Root\dist-packages\uvnvr-$Version-windows-arm64.zip" -Force
$makensis = Get-Command makensis -ErrorAction SilentlyContinue
if ($makensis) {
  & $makensis.Source -V2 "-DVERSION=$Version" "-DARCH=arm64" "-DOUTDIR=$Root\dist-packages" "-DSOURCE=$Out" "-DROOT=$Root" "$Root\packaging\windows\uvnvr.nsi"
}
