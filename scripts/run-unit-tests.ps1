<#
.SYNOPSIS
    Builds and runs TheBrowserProject's own unit test binaries.

.DESCRIPTION
    Covers only our code (components/tbp_download and friends) — not Chromium's
    own test suites, which are upstream's business and take hours.

    These deliberately live in small, standalone test binaries rather than being
    folded into components_unittests: that target is a ~2000-step build pulling
    in most of //components and much of //content, and there is no reason for
    our tests to wait on it. Most of what our binaries link (//base, gtest) is
    already built by the main chrome build, so in an incremental CI run this
    step costs well under two minutes.

    Assumes bootstrap.ps1 + apply-patches.ps1 + build.ps1 have already run (it
    reuses the same out/Release directory and its gn configuration).
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path,
    [string]$ChromiumSrc = "$((Resolve-Path "$PSScriptRoot\..").Path)\chromium\src",
    [string]$DepotToolsDir = "$((Resolve-Path "$PSScriptRoot\..").Path)\depot_tools",
    [string]$OutDir = "out/Release",

    # Full GN labels of the test binaries to build and run. Add new ones here as
    # the engine grows; the executable name is taken from after the colon.
    [string[]]$Targets = @("components/tbp_download:tbp_download_unittests")
)

$ErrorActionPreference = "Stop"

# See bootstrap.ps1 — process-local, doesn't carry over between CI steps.
$env:GIT_TERMINAL_PROMPT = "0"
$env:GCM_INTERACTIVE = "Never"

. (Join-Path $PSScriptRoot "lib\Resolve-RealPath.ps1")

function Write-Step($msg) {
    Write-Host "==> $msg" -ForegroundColor Cyan
}

if (-not (Test-Path $ChromiumSrc)) {
    throw "Chromium checkout not found at $ChromiumSrc — run bootstrap.ps1 first"
}

$env:PATH = "$DepotToolsDir;$env:PATH"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = "0"
$env:DEPOT_TOOLS_UPDATE = "0"

$outPath = Join-Path $ChromiumSrc $OutDir
if (-not (Test-Path $outPath)) {
    throw "Build directory not found at $outPath — run build.ps1 first"
}

# siso needs both its own binary path and its working directory dereferenced —
# passing resolved arguments alone is not enough, because Windows does not
# resolve reparse points when setting a process's current directory.
$sisoPath = Resolve-RealPath (Join-Path $ChromiumSrc "third_party\siso\cipd\siso.exe")
if (-not (Test-Path $sisoPath)) { throw "siso.exe not found at $sisoPath" }
$realOutPath = Resolve-RealPath $outPath

$failed = @()

foreach ($label in $Targets) {
    $name = $label.Split(":")[-1]

    Write-Step "Building $label"
    Push-Location $realOutPath
    try {
        & $sisoPath ninja -C $realOutPath $label
        if ($LASTEXITCODE -ne 0) { throw "Build of $label failed (exit $LASTEXITCODE)" }
    } finally {
        Pop-Location
    }

    $exe = Join-Path $outPath "$name.exe"
    if (-not (Test-Path $exe)) { throw "$name.exe not found at $exe after a successful build" }

    Write-Step "Running $name"
    & $exe
    if ($LASTEXITCODE -ne 0) {
        # Keep going so one failing suite doesn't hide results from the others.
        Write-Warning "$name FAILED (exit $LASTEXITCODE)"
        $failed += $name
    } else {
        Write-Host "$name passed" -ForegroundColor Green
    }
}

if ($failed.Count -gt 0) {
    throw "Unit test failures in: $($failed -join ', ')"
}

Write-Step "All unit tests passed"
