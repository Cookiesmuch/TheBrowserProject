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

# See bootstrap.ps1 — process-local, doesn't carry over between CI steps.
$env:GIT_TERMINAL_PROMPT = "0"
$env:GCM_INTERACTIVE = "Never"

function Write-Step($msg) {
    Write-Host "==> $msg" -ForegroundColor Cyan
}

if (-not (Test-Path $ChromiumSrc)) {
    throw "Chromium checkout not found at $ChromiumSrc — run bootstrap.ps1 first"
}

# Refuse to start if a TRACKED file is already modified going in. This is a
# sanity check that the tree is in a known-good state — either pristine (fresh
# from bootstrap.ps1's reset) or exactly the fully-patched-and-committed state a
# prior successful run left behind. Deliberately ignores untracked entries
# ("??"): a real Chromium checkout normally has untracked nested-repo
# directories from third_party/ (not registered as formal git submodules), and
# flagging those would refuse to run on every single checkout. The actual risk
# Copilot's review flagged — `git add -A` sweeping up unrelated state — is now
# handled by staging exact paths below instead, so this check doesn't need to
# be the only line of defense.
#
# Checked BEFORE the skip check below (issue #12), not after: a matching stamp
# only proves this exact hash was successfully applied at some point in the
# past, not that nothing has touched the tree since. Honoring the stamp without
# this check first would let an unexpectedly dirty tree slip through silently
# whenever the stamp happened to still match.
Push-Location $ChromiumSrc
try {
    $trackedDirty = git status --porcelain | Where-Object { -not $_.StartsWith("??") }
    if ($trackedDirty) {
        throw "Chromium checkout at $ChromiumSrc has modified tracked files already — refusing to start (would risk committing unrelated changes). Run bootstrap.ps1 to reset it, or investigate manually.`n$trackedDirty"
    }
} finally {
    Pop-Location
}

# --- skip check (issue #12) ---
# Mirrors bootstrap.ps1's skip check against the same stamp file: if
# chromium.version + patches/ + overlay/ hash the same as what's already
# recorded there, and the tracked-dirty check above just confirmed the tree
# hasn't been touched since, this tree is already fully patched and committed
# (or bootstrap.ps1 already decided as much and left it alone) — copying
# overlay files and reapplying + recommitting every patch again would be a
# no-op that still rewrites file mtimes across the tree, defeating ninja's
# incremental cache for nothing. Skip straight to done.
. (Join-Path $PSScriptRoot "lib\Get-PatchStateHash.ps1")
$versionFile = Join-Path $RepoRoot "chromium.version"
$overlayDir = Join-Path $RepoRoot "overlay"
$patchesDir = Join-Path $RepoRoot "patches"
$desiredHash = Get-PatchStateHash -VersionFile $versionFile -PatchesDir $patchesDir -OverlayDir $overlayDir
$stampFile = Join-Path (Split-Path $ChromiumSrc -Parent) ".tbp_stamp"

if ((Test-Path $stampFile) -and ((Get-Content $stampFile -Raw).Trim() -eq $desiredHash)) {
    Write-Step "patches/ + overlay/ + chromium.version unchanged since the last successful apply on this runner — skipping reapply (tree already correctly patched, mtimes preserved)"
    exit 0
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
$patchFiles = Get-ChildItem -Path $patchesDir -Filter "*.patch" | Sort-Object Name
if ($patchFiles.Count -eq 0) {
    Write-Step "patches/ is empty (no-op) — nothing to apply"
} else {
    Write-Step "Applying $($patchFiles.Count) patch(es)"
    Push-Location $ChromiumSrc
    try {
        foreach ($patch in $patchFiles) {
            Write-Host "    applying $($patch.Name)"

            # Apply from a CRLF-normalized copy, never the file as it sits on disk.
            #
            # `git apply` matches context lines byte-for-byte against the target.
            # The Chromium tree is LF, so a patch whose own line terminators got
            # converted to CRLF fails on every hunk with a bare "patch does not
            # apply". That conversion is the Windows git default (core.autocrlf),
            # and .gitattributes only protects files git actually rewrites — a
            # checkout that reuses an existing working tree can leave an older,
            # already-converted copy in place. Normalizing here makes patch
            # application independent of how any given machine checked the repo
            # out, which is what CI failing on exactly this taught us.
            #
            # Safe because every patch here targets the Chromium tree, which is
            # uniformly LF; a patch that legitimately needed to add CR-terminated
            # content would need different handling.
            $normalizedPatch = Join-Path ([System.IO.Path]::GetTempPath()) "tbp-$($patch.BaseName)-$PID.patch"
            $patchBytes = [System.IO.File]::ReadAllBytes($patch.FullName)
            $patchText = [System.Text.Encoding]::UTF8.GetString($patchBytes) -replace "`r`n", "`n"
            # Write without a BOM; git apply will not parse a patch that starts with one.
            [System.IO.File]::WriteAllText($normalizedPatch, $patchText,
                                           (New-Object System.Text.UTF8Encoding($false)))

            # Get exactly which files this patch touches before applying, so the
            # commit afterward stages only those — not `-A`, which could sweep up
            # unrelated dirty state.
            $numstat = & git apply --numstat $normalizedPatch
            $patchPaths = @($numstat | ForEach-Object { ($_ -split "`t")[2] } | Where-Object { $_ })
            if ($patchPaths.Count -eq 0) {
                Remove-Item $normalizedPatch -Force -ErrorAction SilentlyContinue
                throw "Could not determine which files $($patch.Name) touches (git apply --numstat returned nothing) — refusing to apply."
            }

            # --3way falls back to a real three-way merge (using the blobs the patch was
            # generated against) when a plain context-match fails — this matters most
            # after a chromium.version bump, where upstream may have touched nearby but
            # non-conflicting lines in the same file. It still fails (with conflict
            # markers left in the file) on genuine overlapping changes, which is exactly
            # when a human needs to re-derive the patch by hand.
            & git apply --whitespace=nowarn --3way $normalizedPatch
            $applyExit = $LASTEXITCODE
            Remove-Item $normalizedPatch -Force -ErrorAction SilentlyContinue
            if ($applyExit -ne 0) {
                throw "Patch failed to apply cleanly: $($patch.Name). Fix the patch (see scripts/export-patches.ps1) before continuing."
            }
            Invoke-GitCommit "patch: $($patch.Name)" $patchPaths
        }
    } finally {
        Pop-Location
    }
}

# Record what's now applied so bootstrap.ps1 and a future run of this script can
# skip redoing this work (see issue #12) as long as chromium.version/patches/
# overlay stay exactly this way. Lives outside the git checkout ($ChromiumDir,
# not $ChromiumSrc) so it's untouched by git clean/reset and by design gets
# invalidated (stamp goes stale, triggering a real reapply) whenever
# bootstrap.ps1 actually resets the tree for a version bump.
Set-Content -Path $stampFile -Value $desiredHash -NoNewline
Write-Step "Overlay + patches applied cleanly"
