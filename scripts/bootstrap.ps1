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

# This machine has credential.helper=manager set system-wide (Git Credential
# Manager, likely Git for Windows' own default), which applies to every https://
# remote — including chromium.googlesource.com, which needs no auth at all for
# reads. GCM tries to check/prompt for credentials anyway; in this non-interactive
# service context that prompt can never be shown or answered, so the git process
# just hangs forever. This is what was actually causing "bootstrap freezes for
# 10+ minutes" — not network throttling, despite how it looked from the outside
# (TCP connects fine; the hang is git waiting on a prompt, not on the network).
# GIT_TERMINAL_PROMPT=0 makes git fail fast with a clear error instead of hanging
# whenever it would otherwise try to prompt.
$env:GIT_TERMINAL_PROMPT = "0"

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
# depot_tools manages its own revision (its own tooling checks it out to a pinned
# commit, so it's often in detached HEAD — a plain `git pull` fails there with
# "You are not currently on a branch"). Every depot_tools command (gclient, fetch,
# etc.) already self-updates on invocation unless DEPOT_TOOLS_UPDATE=0, so we only
# need to clone it once and otherwise leave it alone.
if (-not (Test-Path $DepotToolsDir)) {
    Write-Step "Cloning depot_tools into $DepotToolsDir"
    git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git $DepotToolsDir
    if ($LASTEXITCODE -ne 0) { throw "git clone of depot_tools failed (exit $LASTEXITCODE)" }
} else {
    Write-Step "depot_tools already present at $DepotToolsDir (self-updates on use)"
}

$env:PATH = "$DepotToolsDir;$env:PATH"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = "0"
# Disable depot_tools' own self-update-on-every-invocation. It's not just slow —
# in CI's non-interactive context it appears to hang outright (a run sat with a
# live but idle git-remote-https process, zero active network connections, for
# 12+ minutes with no console output, on a step that normally takes under a
# minute once the cache is warm). It's also bad for reproducibility regardless:
# an uncontrolled auto-update could silently change the depot_tools version
# between runs. We pin by only ever cloning once and leaving it alone.
$env:DEPOT_TOOLS_UPDATE = "0"

# --- resolve pinned tag to exact commit (never trust a hash hardcoded in docs) ---
Write-Step "Resolving $pinnedTag to an exact commit"
$lsRemote = git ls-remote https://chromium.googlesource.com/chromium/src.git "refs/tags/$pinnedTag"
if (-not $lsRemote) {
    throw "Could not resolve tag '$pinnedTag' against chromium/src — check chromium.version"
}
$pinnedCommit = ($lsRemote -split "\s+")[0]
Write-Step "Resolved $pinnedTag -> $pinnedCommit"

# --- fetch/sync ---
# Gate on an actual checkout marker (.gclient), not just $ChromiumDir existing —
# if a first fetch is interrupted (or the directory was pre-created for any
# reason) before .gclient is written, checking the bare directory would skip
# `fetch` entirely and the script would throw later at the missing-srcDir check,
# unable to recover on rerun.
$gclientMarker = Join-Path $ChromiumDir ".gclient"
if (-not (Test-Path $gclientMarker)) {
    New-Item -ItemType Directory -Path $ChromiumDir -Force | Out-Null
    Write-Step "Running 'fetch chromium' into $ChromiumDir (first run — this is the big one)"
    Push-Location $ChromiumDir
    try {
        & fetch --nohooks chromium
        if ($LASTEXITCODE -ne 0) { throw "'fetch chromium' failed (exit $LASTEXITCODE)" }
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
    if ($LASTEXITCODE -ne 0) { throw "git fetch of pinned commit failed (exit $LASTEXITCODE)" }

    git checkout $pinnedCommit
    if ($LASTEXITCODE -ne 0) { throw "git checkout of pinned commit failed (exit $LASTEXITCODE)" }

    # A prior CI/local run may have left patches applied (tracked-file edits) and
    # overlay files copied in (untracked). Discard both so apply-patches.ps1 always
    # starts from a pristine pinned tree — otherwise reapplying an already-applied
    # patch fails, and deleted overlay files linger.
    git reset --hard $pinnedCommit
    if ($LASTEXITCODE -ne 0) { throw "git reset --hard failed (exit $LASTEXITCODE)" }
    # Single -f only: third_party/* dependencies are each their own nested git
    # checkout (gclient-managed, not tracked by src's own git index). A double -f
    # (-ffd) forces git clean to descend into and wipe those nested repos too,
    # which forces gclient sync to redownload every one of ~100 dependencies from
    # scratch on every single rerun — this is what was hammering
    # chromium.googlesource.com into HTTP 429 rate-limiting across our last few
    # runs. Single -f leaves nested repos alone and only removes stray untracked
    # files directly in src/ (like leftover overlay copies), which is all that's
    # actually needed here.
    git clean -fd -e out -e out/Release
    if ($LASTEXITCODE -ne 0) { throw "git clean failed (exit $LASTEXITCODE)" }

    Write-Step "Running gclient sync (incremental after first run)"
    & gclient sync --with_branch_heads --with_tags -D
    if ($LASTEXITCODE -ne 0) { throw "gclient sync failed (exit $LASTEXITCODE)" }

    Write-Step "Running gclient runhooks"
    & gclient runhooks
    if ($LASTEXITCODE -ne 0) { throw "gclient runhooks failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}

Write-Step "Bootstrap complete. Checkout at $srcDir"
