<#
.SYNOPSIS
    Copies overlay/ into the Chromium checkout, then applies patches/*.patch
    in lexical order.

.DESCRIPTION
    Fails fast on the first patch that doesn't apply cleanly, naming it, so
    CI catches a bad patch in seconds rather than after a multi-hour build.

    Overlay files and each applied patch are committed locally in the
    Chromium checkout (this history is never pushed anywhere — chromium/src
    is gitignored). This keeps re-runs idempotent (bootstrap.ps1 resets to
    the pinned commit + cleans untracked files before this runs) and gives
    scripts/export-patches.ps1 a correct baseline: a new patch's diff is
    just what changed since the last patch commit, not the whole series.
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path,
    [string]$ChromiumSrc = "$((Resolve-Path "$PSScriptRoot\..").Path)\chromium\src"
)

$ErrorActionPreference = "Stop"

function Write-Step($msg) {
    Write-Host "==> $msg" -ForegroundColor Cyan
}

if (-not (Test-Path $ChromiumSrc)) {
    throw "Chromium checkout not found at $ChromiumSrc — run bootstrap.ps1 first"
}

function Invoke-GitCommit($message) {
    Push-Location $ChromiumSrc
    try {
        & git add -A
        if ($LASTEXITCODE -ne 0) { throw "git add failed (exit $LASTEXITCODE)" }
        & git -c user.name="tbp-fork-bot" -c user.email="tbp-fork-bot@localhost" commit -m $message --quiet
        if ($LASTEXITCODE -ne 0) { throw "git commit failed for '$message' (exit $LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
}

# --- overlay: copy new files verbatim, preserving relative paths ---
$overlayDir = Join-Path $RepoRoot "overlay"
$overlayFiles = Get-ChildItem -Path $overlayDir -Recurse -File | Where-Object { $_.Name -ne ".gitkeep" }
if ($overlayFiles.Count -eq 0) {
    Write-Step "overlay/ is empty (no-op) — nothing to copy"
} else {
    Write-Step "Copying $($overlayFiles.Count) overlay file(s) into checkout"
    foreach ($file in $overlayFiles) {
        $relPath = $file.FullName.Substring($overlayDir.Length).TrimStart('\', '/')
        $dest = Join-Path $ChromiumSrc $relPath
        New-Item -ItemType Directory -Path (Split-Path $dest -Parent) -Force | Out-Null
        Copy-Item $file.FullName $dest -Force
        Write-Host "    $relPath"
    }
    Invoke-GitCommit "tbp: overlay files"
}

# --- patches: apply in lexical order ---
$patchesDir = Join-Path $RepoRoot "patches"
$patchFiles = Get-ChildItem -Path $patchesDir -Filter "*.patch" | Sort-Object Name
if ($patchFiles.Count -eq 0) {
    Write-Step "patches/ is empty (no-op) — nothing to apply"
} else {
    Write-Step "Applying $($patchFiles.Count) patch(es)"
    Push-Location $ChromiumSrc
    try {
        foreach ($patch in $patchFiles) {
            Write-Host "    applying $($patch.Name)"
            & git apply --whitespace=nowarn $patch.FullName
            if ($LASTEXITCODE -ne 0) {
                throw "Patch failed to apply cleanly: $($patch.Name). Fix the patch (see scripts/export-patches.ps1) before continuing."
            }
            Invoke-GitCommit "patch: $($patch.Name)"
        }
    } finally {
        Pop-Location
    }
}

Write-Step "Overlay + patches applied cleanly"
