# Copies the six HyperLPR3 MNN models into <DataDir>\models\hyperlpr3.
#
# The model files are not stored in the repository. Point -Source or
# $env:HYPERLPR_MODELS_SOURCE at a directory that contains them.
#
# Usage:
#   powershell -File scripts/fetch-ai-models.ps1 [-DataDir <dir>] [-Source <dir>]
#
# Defaults:
#   -DataDir   $env:UVNVR_DATA_DIR, else .\data
#   -Source    $env:HYPERLPR_MODELS_SOURCE, then models\hyperlpr3 in this repo,
#              %USERPROFILE%\.cache\uvnvr\hyperlpr3
param(
    [string]$DataDir = $(if ($env:UVNVR_DATA_DIR) { $env:UVNVR_DATA_DIR } else { ".\data" }),
    [string]$Source = $env:HYPERLPR_MODELS_SOURCE
)

$ErrorActionPreference = "Stop"

$Models = @(
    "b320_backbone_h.mnn",
    "b320_header_h.mnn",
    "b640x_backbone_h.mnn",
    "b640x_head_h.mnn",
    "litemodel_cls_96xh.mnn",
    "rpv3_mdict_160h.mnn"
)

function Test-ModelSet {
    param([string]$Directory)
    if (-not $Directory) { return $false }
    foreach ($name in $Models) {
        if (-not (Test-Path -LiteralPath (Join-Path $Directory $name) -PathType Leaf)) {
            return $false
        }
    }
    return $true
}

$RepoDir = Split-Path -Parent $PSScriptRoot
$Candidates = @()
if ($Source) { $Candidates += $Source }
$Candidates += @(
    (Join-Path $RepoDir "models\hyperlpr3"),
    (Join-Path $HOME ".cache\uvnvr\hyperlpr3")
)

$Resolved = $null
foreach ($candidate in $Candidates) {
    if (Test-ModelSet $candidate) {
        $Resolved = (Resolve-Path -LiteralPath $candidate).Path
        break
    }
}

if (-not $Resolved) {
    Write-Error @"
HyperLPR3 model files were not found.

Download the HyperLPR3 MNN models (b320_backbone_h.mnn, b320_header_h.mnn,
b640x_backbone_h.mnn, b640x_head_h.mnn, litemodel_cls_96xh.mnn and
rpv3_mdict_160h.mnn), then run:

  powershell -File scripts/fetch-ai-models.ps1 -Source C:\path\to\models -DataDir C:\path\to\data

or set HYPERLPR_MODELS_SOURCE.
"@
    exit 1
}

$Destination = Join-Path $DataDir "models\hyperlpr3"
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

Write-Host "source: $Resolved"
Write-Host "destination: $Destination"

foreach ($name in $Models) {
    $target = Join-Path $Destination $name
    $sourceFile = Join-Path $Resolved $name
    if ((Resolve-Path -LiteralPath $Resolved).Path -eq (Resolve-Path -LiteralPath $Destination).Path) {
        Write-Host "  ${name}: already in place"
        continue
    }
    Copy-Item -LiteralPath $sourceFile -Destination $target -Force
    $size = (Get-Item -LiteralPath $target).Length
    Write-Host "  ${name}: $size bytes"
}

Write-Host "HyperLPR3 models are ready under $Destination"
