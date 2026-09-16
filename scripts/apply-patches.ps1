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

# Refuse to start if a TRACKED file is already modified going in. This is a
# sanity check that bootstrap.ps1's reset actually left a clean tree — if it
# didn't, something unexpected happened and a human needs to look, rather than
# silently proceeding. Deliberately ignores untracked entries ("??"): a real
# Chromium checkout normally has untracked nested-repo directories from
# third_party/ (not registered as formal git submodules), and flagging those
# would refuse to run on every single checkout. The actual risk Copilot's
# review flagged — `git add -A` sweeping up unrelated state — is now handled
# by staging exact paths below instead, so this check doesn't need to be the
# only line of defense.
Push-Location $ChromiumSrc
try {
    $trackedDirty = git status --porcelain | Where-Object { -not $_.StartsWith("??") }
    if ($trackedDirty) {
        throw "Chromium checkout at $ChromiumSrc has modified tracked files already — refusing to start (would risk committing unrelated changes). Run bootstrap.ps1 to reset it, or investigate manually.`n$trackedDirty"
    }
} finally {
    Pop-Location
}

function Invoke-GitCommit($message, [string[]]$Paths) {
    Push-Location $ChromiumSrc
    try {
        & git add -- @Paths
        if ($LASTEXITCODE -ne 0) { throw "git add failed for $($Paths -join ', ') (exit $LASTEXITCODE)" }
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
    $copiedRelPaths = @()
    foreach ($file in $overlayFiles) {
        $relPath = $file.FullName.Substring($overlayDir.Length).TrimStart('\', '/')
        $dest = Join-Path $ChromiumSrc $relPath
        New-Item -ItemType Directory -Path (Split-Path $dest -Parent) -Force | Out-Null
        Copy-Item $file.FullName $dest -Force
        Write-Host "    $relPath"
        $copiedRelPaths += $relPath
    }
    Invoke-GitCommit "tbp: overlay files" $copiedRelPaths
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

            # Get exactly which files this patch touches before applying, so the
            # commit afterward stages only those — not `-A`, which could sweep up
            # unrelated dirty state.
            $numstat = & git apply --numstat $patch.FullName
            $patchPaths = @($numstat | ForEach-Object { ($_ -split "`t")[2] } | Where-Object { $_ })
            if ($patchPaths.Count -eq 0) {
                throw "Could not determine which files $($patch.Name) touches (git apply --numstat returned nothing) — refusing to apply."
            }

            # --3way falls back to a real three-way merge (using the blobs the patch was
            # generated against) when a plain context-match fails — this matters most
            # after a chromium.version bump, where upstream may have touched nearby but
            # non-conflicting lines in the same file. It still fails (with conflict
            # markers left in the file) on genuine overlapping changes, which is exactly
            # when a human needs to re-derive the patch by hand.
            & git apply --whitespace=nowarn --3way $patch.FullName
            if ($LASTEXITCODE -ne 0) {
                throw "Patch failed to apply cleanly: $($patch.Name). Fix the patch (see scripts/export-patches.ps1) before continuing."
            }
            Invoke-GitCommit "patch: $($patch.Name)" $patchPaths
        }
    } finally {
        Pop-Location
    }
}

Write-Step "Overlay + patches applied cleanly"
