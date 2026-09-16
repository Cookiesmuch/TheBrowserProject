<#
.SYNOPSIS
    Dev workflow helper: regenerate patches/*.patch from the diff between a
    local working Chromium checkout and its pinned base commit.

.DESCRIPTION
    Use this after hand-editing files in ./chromium/src to author or update
    a patch. It does NOT run in CI — it's for local patch authoring only.

    apply-patches.ps1 commits overlay files and each applied patch locally
    as it goes, so HEAD is always "the last applied patch." This diffs your
    uncommitted edits against HEAD — i.e. only what's new since the last
    patch — instead of the whole cumulative series. Run apply-patches.ps1
    first (on a fresh bootstrap) so HEAD reflects the existing series
    before you start hand-editing.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PatchName,
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path,
    [string]$ChromiumSrc = "$((Resolve-Path "$PSScriptRoot\..").Path)\chromium\src"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $ChromiumSrc)) {
    throw "Chromium checkout not found at $ChromiumSrc — run bootstrap.ps1 first"
}

$patchesDir = Join-Path $RepoRoot "patches"
New-Item -ItemType Directory -Path $patchesDir -Force | Out-Null

if ($PatchName -notmatch '^\d{4}-') {
    $existing = Get-ChildItem $patchesDir -Filter "*.patch" | Sort-Object Name
    $next = if ($existing.Count -eq 0) { 1 } else {
        [int](($existing[-1].Name -split '-')[0]) + 1
    }
    $PatchName = "{0:D4}-{1}" -f $next, $PatchName
}
if (-not $PatchName.EndsWith(".patch")) {
    $PatchName = "$PatchName.patch"
}

$outFile = Join-Path $patchesDir $PatchName

Push-Location $ChromiumSrc
try {
    Write-Host "==> Writing $outFile from current working-tree diff" -ForegroundColor Cyan
    git diff --no-color --binary > $outFile
} finally {
    Pop-Location
}

if ((Get-Item $outFile).Length -eq 0) {
    Remove-Item $outFile
    Write-Warning "No changes detected in $ChromiumSrc — nothing written."
} else {
    Write-Host "==> Wrote $outFile" -ForegroundColor Green
    Write-Host "    Remember to also copy any wholesale NEW files into overlay/ instead of patches/."
}
