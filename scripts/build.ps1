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

# Pin the toolchain to VS2022 explicitly. Without this, Chromium's toolchain
# detection (build/toolchain/win/setup_toolchain.py) picks whichever installed VS
# version sorts highest, which breaks on a machine that also has a VS preview/
# Insiders build installed — that preview expects a matching preview Windows SDK
# that typically isn't installed, and gn gen fails with "Path ... does not exist"
# for the include dir. vswhere with an explicit [17.0,18.0) version range excludes
# anything newer than VS2022 (VS "18" = version 18.x) and only matches VS2022.
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vs2022Path = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -version "[17.0,18.0)" -property installationPath
    if ($vs2022Path) {
        $env:GYP_MSVS_OVERRIDE_PATH = $vs2022Path.Trim()
        Write-Step "Pinned toolchain to VS2022 at $($env:GYP_MSVS_OVERRIDE_PATH)"
    } else {
        Write-Warning "vswhere found no VS2022 (17.x) install with the C++ workload — gn gen may pick the wrong toolchain."
    }
} else {
    Write-Warning "vswhere.exe not found at expected path — cannot pin VS2022 toolchain explicitly."
}

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
