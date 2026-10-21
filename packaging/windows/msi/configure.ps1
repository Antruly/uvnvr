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
    Unregister-ScheduledTask -TaskName 'UvNVR' -Confirm:$false -ErrorAction SilentlyContinue
    Remove-NetFirewallRule -DisplayName 'UvNVR' -ErrorAction SilentlyContinue
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

# The MSI is a machine-wide installation, so it registers the Windows service
# (recommended for production). The Inno Setup wizard lets the operator opt out.
Unregister-ScheduledTask -TaskName 'UvNVR' -Confirm:$false -ErrorAction SilentlyContinue
$binPath = '"{0}" --service --config "{1}"' -f $exe, $configPath
$existing = Get-Service -Name 'UvNVR' -ErrorAction SilentlyContinue
if ($existing) {
    Stop-Service -Name 'UvNVR' -Force -ErrorAction SilentlyContinue
    try { $existing.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30)) } catch {}
}
Start-Sleep -Milliseconds 500
& sc.exe create UvNVR binPath= $binPath start= auto obj= LocalSystem DisplayName= 'UvNVR' | Out-Null
if ($LASTEXITCODE -eq 1073) {
    & sc.exe config UvNVR binPath= $binPath start= auto | Out-Null
}
& sc.exe description UvNVR 'UvNVR software network video recorder' | Out-Null
& sc.exe start UvNVR | Out-Null
Remove-NetFirewallRule -DisplayName 'UvNVR' -ErrorAction SilentlyContinue
New-NetFirewallRule -DisplayName 'UvNVR' -Direction Inbound -Program $exe -Action Allow `
    -Profile Any | Out-Null
