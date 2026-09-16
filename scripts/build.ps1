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

# Also pin the Windows SDK version explicitly. Pinning the VS install alone isn't
# enough: vcvarsall.bat still picks its own "preferred" SDK version independent of
# which VS edition is running it, and on a machine with a VS preview/Insiders
# install alongside VS2022, that preferred version can be a preview SDK
# (e.g. 10.0.28000.0) that was never actually installed — vcvarsall then reports a
# nonexistent include path and gn gen fails. vcvarsall.bat honors WindowsSDKVersion
# if it's already set in the environment, skipping its own auto-detection, so pick
# the newest SDK version that's actually present under Windows Kits\10\Include.
$sdkRoot = "C:\Program Files (x86)\Windows Kits\10\Include"
if (Test-Path $sdkRoot) {
    $latestSdk = Get-ChildItem $sdkRoot -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if ($latestSdk) {
        $env:WindowsSDKVersion = "$($latestSdk.Name)\"
        Write-Step "Pinned Windows SDK version to $($env:WindowsSDKVersion)"
    } else {
        Write-Warning "No installed Windows SDK version found under $sdkRoot — gn gen may fail."
    }
} else {
    Write-Warning "Windows Kits Include dir not found at $sdkRoot — cannot pin SDK version explicitly."
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

    # siso (the ninja-compatible build tool GN generates for) can leave .siso_lock
    # behind even after a clean exit — seen twice now. A stale lock makes the next
    # invocation die instantly with an unhelpful "path specified" error. Clear it
    # if the PID it references isn't actually running, so a build never needs
    # manual intervention to recover from a prior run (clean or interrupted).
    $lockPidFile = Join-Path $outPath ".siso_lock.pid"
    if (Test-Path $lockPidFile) {
        $lockPid = (Get-Content $lockPidFile -Raw).Trim() -replace '^pid=', ''
        $stillRunning = $false
        if ($lockPid -match '^\d+$') {
            $stillRunning = [bool](Get-Process -Id ([int]$lockPid) -ErrorAction SilentlyContinue)
        }
        if (-not $stillRunning) {
            Write-Step "Clearing stale siso lock (pid $lockPid is not running)"
            Remove-Item (Join-Path $outPath ".siso_lock"), $lockPidFile -Force -ErrorAction SilentlyContinue
        }
    }

    Write-Step "autoninja -C $OutDir $Target"
    & autoninja -C $OutDir $Target
    if ($LASTEXITCODE -ne 0) { throw "autoninja build failed" }

    # The internal GN target/binary stays named "chrome" (chrome.exe) — renaming that
    # would ripple into installer/packaging/test scripts across the tree that reference
    # it by name. Branding (About page, window title, installer strings) is handled
    # properly via patches/0001-rebrand-to-thebrowserproject.patch instead. This copy
    # just gives us the product-facing binary name for what we actually ship.
    $brandedExe = Join-Path $outPath "TheBrowserProject.exe"
    Copy-Item (Join-Path $outPath "$Target.exe") $brandedExe -Force

    # chrome.exe can't run standalone — it needs chrome.dll, the .pak resource
    # files, icudtl.dat, locales/, etc. alongside it, so we link the whole output
    # directory into the repo instead of copying gigabytes of it on every build.
    # A junction needs no elevation (unlike a symlink) and costs zero extra disk.
    $repoLink = Join-Path $RepoRoot "out"
    $existingLink = Get-Item $repoLink -ErrorAction SilentlyContinue
    if ($existingLink -and $existingLink.LinkType -eq "Junction" -and $existingLink.Target -eq $outPath) {
        Write-Step "$repoLink already links to $outPath"
    } else {
        if (Test-Path $repoLink) { Remove-Item $repoLink -Force -Recurse }
        New-Item -ItemType Junction -Path $repoLink -Target $outPath | Out-Null
        Write-Step "Linked $repoLink -> $outPath"
    }

    Write-Step "Build complete: $outPath\$Target.exe"
    Write-Step "Also reachable at $repoLink\TheBrowserProject.exe (and $repoLink\$Target.exe)"
} finally {
    Pop-Location
}
