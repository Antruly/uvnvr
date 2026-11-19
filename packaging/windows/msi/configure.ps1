param(
    [int]$HttpPort = 8080,
    [int]$RtspPort = 554,
    [string]$InstallDir = "",
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
if ($InstallDir) {
    # The MSI passes [INSTALLFOLDER]. so the trailing backslash cannot escape
    # the closing quote; normalize it back to the real directory.
    $InstallDir = [System.IO.Path]::GetFullPath($InstallDir).TrimEnd('\')
}
$dataRoot = Join-Path $env:ProgramData 'UvNVR'
$configPath = Join-Path $dataRoot 'uvnvr.json'
$desktopUrl = Join-Path $env:PUBLIC 'Desktop\UvNVR Web UI.url'
$startDir = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\UvNVR'
$startUrl = Join-Path $startDir 'UvNVR Web UI.url'

if ($Uninstall) {
    $svc = Get-Service -Name 'UvNVR' -ErrorAction SilentlyContinue
    if ($svc) {
        Stop-Service -Name 'UvNVR' -Force -ErrorAction SilentlyContinue
        try { $svc.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30)) } catch {}
    }
    Start-Sleep -Milliseconds 500
    & sc.exe delete UvNVR | Out-Null
    Stop-ScheduledTask -TaskName 'UvNVR' -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName 'UvNVR' -Confirm:$false -ErrorAction SilentlyContinue
    Stop-Process -Name 'uvnvr' -Force -ErrorAction SilentlyContinue
    Remove-NetFirewallRule -DisplayName 'UvNVR' -ErrorAction SilentlyContinue
    Remove-Item $desktopUrl -Force -ErrorAction SilentlyContinue
    Remove-Item $startUrl -Force -ErrorAction SilentlyContinue
    Remove-Item $startDir -Force -ErrorAction SilentlyContinue
    exit 0
}

New-Item -ItemType Directory -Force $dataRoot | Out-Null
if (Test-Path $configPath) {
    # On upgrade, apply only properties that were given a non-default value;
    # otherwise keep the ports already in the file. Every other key is
    # preserved, including TLS and storage settings.
    try {
        $existing = Get-Content $configPath -Raw | ConvertFrom-Json
        if ($HttpPort -eq 8080 -and $existing.http.port) {
            $HttpPort = [int]$existing.http.port
        }
        if ($RtspPort -eq 554 -and $existing.rtsp.port) {
            $RtspPort = [int]$existing.rtsp.port
        }
        if ($null -eq $existing.http) {
            $existing | Add-Member -NotePropertyName http -NotePropertyValue ([pscustomobject]@{}) -Force
        }
        if ($null -eq $existing.rtsp) {
            $existing | Add-Member -NotePropertyName rtsp -NotePropertyValue ([pscustomobject]@{}) -Force
        }
        $existing.http | Add-Member -NotePropertyName port -NotePropertyValue $HttpPort -Force
        $existing.rtsp | Add-Member -NotePropertyName port -NotePropertyValue $RtspPort -Force
        $text = $existing | ConvertTo-Json -Depth 20
        [System.IO.File]::WriteAllText(
            $configPath,
            $text + "`r`n",
            (New-Object System.Text.UTF8Encoding($false))
        )
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

# The MSI is a machine-wide installation, so it registers the Windows service
# (recommended for production). The Inno Setup wizard lets the operator opt out.
Stop-ScheduledTask -TaskName 'UvNVR' -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName 'UvNVR' -Confirm:$false -ErrorAction SilentlyContinue
Stop-Process -Name 'uvnvr' -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
$binPath = '"{0}" --service --config "{1}"' -f $exe, $configPath
$existing = Get-Service -Name 'UvNVR' -ErrorAction SilentlyContinue
if ($existing) {
    Stop-Service -Name 'UvNVR' -Force -ErrorAction SilentlyContinue
    try { $existing.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30)) } catch {}
    & sc.exe delete UvNVR | Out-Null
    for ($i = 0; $i -lt 40; $i++) {
        if (-not (Get-Service -Name 'UvNVR' -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 250
    }
}
Start-Sleep -Milliseconds 500
# New-Service passes BinaryPathName through unchanged, so quotes inside the
# command line survive (native argument passing in PowerShell 5.1 does not).
New-Service -Name 'UvNVR' -BinaryPathName $binPath -StartupType Automatic `
    -DisplayName 'UvNVR' -ErrorAction Stop | Out-Null
& sc.exe description UvNVR 'UvNVR software network video recorder' | Out-Null
Start-Service -Name 'UvNVR' -ErrorAction SilentlyContinue
Remove-NetFirewallRule -DisplayName 'UvNVR' -ErrorAction SilentlyContinue
New-NetFirewallRule -DisplayName 'UvNVR' -Direction Inbound -Program $exe -Action Allow `
    -Profile Any | Out-Null
