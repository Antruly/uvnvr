# Update the HTTP and RTSP ports in an existing uvnvr.json while preserving
# every other setting. The installer calls this on upgrades so the ports page
# is authoritative; writing the file by hand avoided reopening it.
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][int]$HttpPort,
    [Parameter(Mandatory = $true)][int]$RtspPort
)

$ErrorActionPreference = 'Stop'
$json = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
if ($null -eq $json.http) {
    $json | Add-Member -NotePropertyName http -NotePropertyValue ([pscustomobject]@{}) -Force
}
if ($null -eq $json.rtsp) {
    $json | Add-Member -NotePropertyName rtsp -NotePropertyValue ([pscustomobject]@{}) -Force
}
$json.http | Add-Member -NotePropertyName port -NotePropertyValue $HttpPort -Force
$json.rtsp | Add-Member -NotePropertyName port -NotePropertyValue $RtspPort -Force

$text = $json | ConvertTo-Json -Depth 20
# UTF8 without BOM: the C++ JSON parser does not skip a byte-order mark.
[System.IO.File]::WriteAllText($Path, $text + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
