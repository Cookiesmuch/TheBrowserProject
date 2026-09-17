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
# reads. GIT_TERMINAL_PROMPT=0 only suppresses GIT'S OWN built-in prompt; it does
# NOT stop GCM, which is a separate external program with its own interactive/UI
# prompting logic. The runner service runs as NT AUTHORITY\NETWORK SERVICE — no
# desktop, no Session-0 UI, no cached credential store of its own — so when GCM
# tries its interactive fallback there's nothing for it to fall back to, and it
# hangs indefinitely instead of failing fast (confirmed: the exact same `git
# ls-remote` command completed in ~25s run manually as the interactive user, but
# sat for 12+ minutes under the actual runner service). GCM_INTERACTIVE=Never is
# the variable GCM itself reads to disable all of its own interactive/UI prompting
# — this is the actual fix; GIT_TERMINAL_PROMPT=0 alone was not sufficient.
$env:GIT_TERMINAL_PROMPT = "0"
$env:GCM_INTERACTIVE = "Never"

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

# --- skip check (issue #12) ---
# git reset --hard + git clean -fd rewrite file mtimes broadly across the tree,
# which defeats ninja's (mtime-based) staleness detection even when nothing
# actually changed — turning every CI run into a near-full rebuild regardless of
# how small the diff was. apply-patches.ps1 stamps $stampFile with a hash of
# chromium.version + patches/ + overlay/ after it successfully finishes; if that
# hash still matches what we'd apply now AND the tree has no uncommitted tracked
# changes (i.e. it's exactly the fully-patched, fully-committed state
# apply-patches.ps1 leaves behind), the tree is already correct and none of
# fetch/checkout/reset/clean/sync/runhooks need to run at all. Falls back to the
# full flow below on any mismatch, missing stamp, or unexpected dirty tree — this
# only ever skips work, never skips verification.
. (Join-Path $PSScriptRoot "lib\Get-PatchStateHash.ps1")
$patchesDirForHash = Join-Path $RepoRoot "patches"
$overlayDirForHash = Join-Path $RepoRoot "overlay"
$desiredHash = Get-PatchStateHash -VersionFile $versionFile -PatchesDir $patchesDirForHash -OverlayDir $overlayDirForHash
$stampFile = Join-Path $ChromiumDir ".tbp_stamp"

$canSkip = $false
if (Test-Path $stampFile) {
    $stampedHash = (Get-Content $stampFile -Raw).Trim()
    if ($stampedHash -eq $desiredHash) {
        Push-Location $srcDir
        try {
            $trackedDirty = git status --porcelain | Where-Object { -not $_.StartsWith("??") }
            if (-not $trackedDirty) { $canSkip = $true }
        } finally {
            Pop-Location
        }
    }
}

if ($canSkip) {
    Write-Step "chromium.version + patches/ + overlay/ unchanged since the last successful run on this runner — skipping fetch/checkout/reset/resync (tree is already correctly patched; mtimes preserved for ninja's incremental cache)"
} else {
    # Invalidate the stamp before doing anything destructive below. Otherwise a
    # fallback into this branch for a reason OTHER than a hash mismatch (e.g. the
    # tree was unexpectedly dirty even though the hash still matched) resets the
    # checkout back to the pristine, unpatched pinned commit while leaving the old
    # stamp in place — apply-patches.ps1 would then see that still-matching stamp
    # and skip reapplying entirely, leaving CI building an unpatched tree.
    if (Test-Path $stampFile) {
        Remove-Item $stampFile -Force
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
}

Write-Step "Bootstrap complete. Checkout at $srcDir"
