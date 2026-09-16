<#
.SYNOPSIS
    Runs gn gen + autoninja against the patched Chromium checkout.
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path,
    [string]$ChromiumSrc = "$((Resolve-Path "$PSScriptRoot\..").Path)\chromium\src",
    [string]$DepotToolsDir = "$((Resolve-Path "$PSScriptRoot\..").Path)\depot_tools",
    [string]$OutDir = "out/Release",
    [string]$Target = "chrome"
)

$ErrorActionPreference = "Stop"

function Write-Step($msg) {
    Write-Host "==> $msg" -ForegroundColor Cyan
}

if (-not (Test-Path $ChromiumSrc)) {
    throw "Chromium checkout not found at $ChromiumSrc — run bootstrap.ps1 first"
}

$env:PATH = "$DepotToolsDir;$env:PATH"
# bootstrap.ps1 sets this too, but it's process-local — CI runs each script as a
# separate step/process, so it doesn't carry over. Without it, gn/ninja may try to
# fetch depot_tools' bundled Windows toolchain instead of using the installed VS one.
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = "0"

$argsTemplate = Join-Path $RepoRoot "args.gn.template"
if (-not (Test-Path $argsTemplate)) {
    throw "args.gn.template not found at $argsTemplate"
}

Push-Location $ChromiumSrc
try {
    $outPath = Join-Path $ChromiumSrc $OutDir
    New-Item -ItemType Directory -Path $outPath -Force | Out-Null
    Copy-Item $argsTemplate (Join-Path $outPath "args.gn") -Force

    Write-Step "gn gen $OutDir"
    & gn gen $OutDir
    if ($LASTEXITCODE -ne 0) { throw "gn gen failed" }

    Write-Step "autoninja -C $OutDir $Target"
    & autoninja -C $OutDir $Target
    if ($LASTEXITCODE -ne 0) { throw "autoninja build failed" }

    Write-Step "Build complete: $outPath\$Target.exe"
} finally {
    Pop-Location
}
