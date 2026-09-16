<#
.SYNOPSIS
    Installs/updates depot_tools and syncs the pinned Chromium revision
    (from chromium.version) into ./chromium via gclient.

.DESCRIPTION
    Idempotent: safe to re-run. First run does a full ~100GB fetch+sync and
    will take hours; subsequent runs are incremental.
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path,
    [string]$DepotToolsDir = "$((Resolve-Path "$PSScriptRoot\..").Path)\depot_tools",
    [string]$ChromiumDir = "$((Resolve-Path "$PSScriptRoot\..").Path)\chromium"
)

$ErrorActionPreference = "Stop"

function Write-Step($msg) {
    Write-Host "==> $msg" -ForegroundColor Cyan
}

$versionFile = Join-Path $RepoRoot "chromium.version"
if (-not (Test-Path $versionFile)) {
    throw "chromium.version not found at $versionFile"
}
$pinnedTag = (Get-Content $versionFile -Raw).Trim()
Write-Step "Pinned Chromium version: $pinnedTag"

# --- depot_tools ---
if (-not (Test-Path $DepotToolsDir)) {
    Write-Step "Cloning depot_tools into $DepotToolsDir"
    git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git $DepotToolsDir
} else {
    Write-Step "Updating existing depot_tools in $DepotToolsDir"
    git -C $DepotToolsDir pull --ff-only
}

$env:PATH = "$DepotToolsDir;$env:PATH"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = "0"

# --- resolve pinned tag to exact commit (never trust a hash hardcoded in docs) ---
Write-Step "Resolving $pinnedTag to an exact commit"
$lsRemote = git ls-remote https://chromium.googlesource.com/chromium/src.git "refs/tags/$pinnedTag"
if (-not $lsRemote) {
    throw "Could not resolve tag '$pinnedTag' against chromium/src — check chromium.version"
}
$pinnedCommit = ($lsRemote -split "\s+")[0]
Write-Step "Resolved $pinnedTag -> $pinnedCommit"

# --- fetch/sync ---
if (-not (Test-Path $ChromiumDir)) {
    New-Item -ItemType Directory -Path $ChromiumDir -Force | Out-Null
    Write-Step "Running 'fetch chromium' into $ChromiumDir (first run — this is the big one)"
    Push-Location $ChromiumDir
    try {
        & fetch --nohooks chromium
    } finally {
        Pop-Location
    }
}

$srcDir = Join-Path $ChromiumDir "src"
if (-not (Test-Path $srcDir)) {
    throw "Expected checkout at $srcDir after fetch, but it's missing"
}

Write-Step "Checking out pinned commit $pinnedCommit"
Push-Location $srcDir
try {
    git fetch origin $pinnedCommit
    git checkout $pinnedCommit
    Write-Step "Running gclient sync (incremental after first run)"
    & gclient sync --with_branch_heads --with_tags -D
    Write-Step "Running gclient runhooks"
    & gclient runhooks
} finally {
    Pop-Location
}

Write-Step "Bootstrap complete. Checkout at $srcDir"
