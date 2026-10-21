$ErrorActionPreference = 'Stop'
$Version = if ($env:UVNVR_VERSION) { $env:UVNVR_VERSION } else { '0.10.0' }
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$Uvcpp = "$env:RUNNER_TEMP\uvcpp\libuvcpp-1.8.1-msvc-arm64"
$Ffmpeg = "$env:RUNNER_TEMP\ffmpeg"
$Out = "$Root\dist-packages\uvnvr-$Version-windows-arm64"
New-Item -ItemType Directory -Force -Path "$Out\bin", "$Out\share\uvnvr\frontend\dist" | Out-Null
# CMakeLists pins RUNTIME_OUTPUT_DIRECTORY to the build root for every config;
# Visual Studio also accepts the older Release/ layout as a fallback.
$Exe = if (Test-Path "$Root\build\uvnvr.exe") { "$Root\build\uvnvr.exe" } else { "$Root\build\Release\uvnvr.exe" }
if (!(Test-Path $Exe)) { throw "cannot find the built uvnvr.exe under $Root\build" }
Copy-Item $Exe "$Out\bin\uvnvr.exe"
$ffmpegExe = Get-ChildItem -Path $Ffmpeg -Recurse -Filter ffmpeg.exe | Select-Object -First 1
Copy-Item $ffmpegExe.FullName "$Out\bin\ffmpeg.exe"
Copy-Item "$Root\frontend\dist\*" "$Out\share\uvnvr\frontend\dist\" -Recurse
Copy-Item "$Root\packaging\windows\uvnvr.portable.json" "$Out\uvnvr.json"
Get-ChildItem "$Uvcpp\bin\*.dll" | Where-Object { $_.Name -ne 'uvcppd.dll' } | Copy-Item -Destination "$Out\bin\" -Force
$vcpkgBin = "$Root\build\vcpkg_installed\arm64-windows\bin"
if (Test-Path $vcpkgBin) { Copy-Item "$vcpkgBin\*.dll" "$Out\bin\" -Force }
Copy-Item "$Root\LICENSE", "$Root\THIRD_PARTY_NOTICES.md" "$Out\"
Compress-Archive -Path "$Out\*" -DestinationPath "$Root\dist-packages\uvnvr-$Version-windows-arm64.zip" -Force
$env:Path += ';C:\Program Files (x86)\NSIS'
$makensis = Get-Command makensis -ErrorAction SilentlyContinue
if ($makensis) {
  & $makensis.Source -V2 "-DVERSION=$Version" "-DARCH=arm64" "-DOUTDIR=$Root\dist-packages" "-DSOURCE=$Out" "-DROOT=$Root" "$Root\packaging\windows\uvnvr.nsi"
}
