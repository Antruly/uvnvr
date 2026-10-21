param(
    [int]$HttpPort = 8080,
    [int]$RtspPort = 554,
    [string]$InstallDir = "",
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$dataRoot = Join-Path $env:ProgramData 'UvNVR'
$configPath = Join-Path $dataRoot 'uvnvr.json'
$desktopUrl = Join-Path $env:PUBLIC 'Desktop\UvNVR Web UI.url'
$startDir = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\UvNVR'
$startUrl = Join-Path $startDir 'UvNVR Web UI.url'

if ($Uninstall) {
    schtasks.exe /Delete /TN UvNVR /F 2>$null | Out-Null
    netsh advfirewall firewall delete rule name="UvNVR" 2>$null | Out-Null
    Remove-Item $desktopUrl -Force -ErrorAction SilentlyContinue
    Remove-Item $startUrl -Force -ErrorAction SilentlyContinue
    Remove-Item $startDir -Force -ErrorAction SilentlyContinue
    exit 0
}

New-Item -ItemType Directory -Force $dataRoot | Out-Null
if (Test-Path $configPath) {
    # Preserve an existing configuration on upgrade, including its ports.
    try {
        $existing = Get-Content $configPath -Raw | ConvertFrom-Json
        if ($existing.http.port) { $HttpPort = [int]$existing.http.port }
    } catch {
        # A malformed file is left untouched; the operator can fix it.
    }
} else {
    $drive = if ($InstallDir) { $InstallDir.Substring(0, 1).ToUpperInvariant() } else { 'C' }
    $dataDir = if ($drive -eq 'C') { $dataRoot } else { Join-Path $InstallDir 'data' }
    $document = [ordered]@{
        http = [ordered]@{ port = $HttpPort; host = '0.0.0.0' }
        rtsp = [ordered]@{ port = $RtspPort }
        dataDir = $dataDir
    } | ConvertTo-Json -Depth 5
    [System.IO.File]::WriteAllText(
        $configPath,
        $document,
        (New-Object System.Text.UTF8Encoding($false))
    )
}

$exe = Join-Path $InstallDir 'bin\uvnvr.exe'
$url = "http://127.0.0.1:$HttpPort/"
New-Item -ItemType Directory -Force $startDir | Out-Null
"[InternetShortcut]`r`nURL=$url`r`n" | Set-Content -Path $desktopUrl -Encoding ASCII
"[InternetShortcut]`r`nURL=$url`r`n" | Set-Content -Path $startUrl -Encoding ASCII

$taskCommand = "`"$exe`" --config `"$configPath`""
schtasks.exe /Create /TN UvNVR /TR $taskCommand /SC ONSTART /RU SYSTEM /RL HIGHEST /F | Out-Null
netsh advfirewall firewall delete rule name="UvNVR" | Out-Null
netsh advfirewall firewall add rule name="UvNVR" dir=in action=allow program="$exe" enable=yes | Out-Null
