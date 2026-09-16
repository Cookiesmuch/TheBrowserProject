<#
.SYNOPSIS
    Interactive setup wizard for TheBrowserProject's development environment.

.DESCRIPTION
    Run this once on a new Windows machine to go from "nothing installed" to
    a working local build of the Chromium fork. It checks/installs every
    prerequisite this project actually needed the hard way during initial
    bring-up — Visual Studio 2022 + the C++ workload, the exact Windows SDK
    version the pinned Chromium revision requires, Windows long-path
    support, and a Defender exclusion for the checkout — then runs
    bootstrap.ps1 -> apply-patches.ps1 -> build.ps1 and smoke-tests the
    result.

    Safe to re-run: every step checks current state first and skips work
    that's already done.

.PARAMETER InstallRoot
    Where depot_tools/ and chromium/ will live. Defaults to this repo's own
    root. Chromium needs ~100GB free on whichever drive this points at —
    the wizard checks and warns before doing anything expensive.

.PARAMETER SkipBuild
    Run every setup/prerequisite step but stop before the actual
    (multi-hour) Chromium compile. Useful for staging a machine ahead of
    time without committing to the build yet.
#>
[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path,
    [string]$InstallRoot = (Resolve-Path "$PSScriptRoot\..").Path,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

function Write-Banner($msg) {
    Write-Host ""
    Write-Host ("=" * 70) -ForegroundColor Magenta
    Write-Host "  $msg" -ForegroundColor Magenta
    Write-Host ("=" * 70) -ForegroundColor Magenta
}
function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg) { Write-Host "    OK: $msg" -ForegroundColor Green }
function Write-Skip($msg) { Write-Host "    skip: $msg" -ForegroundColor DarkGray }

function Confirm-Step([string]$Prompt, [bool]$DefaultYes = $true) {
    $suffix = if ($DefaultYes) { "[Y/n]" } else { "[y/N]" }
    $answer = Read-Host "$Prompt $suffix"
    if ([string]::IsNullOrWhiteSpace($answer)) { return $DefaultYes }
    return $answer -match '^[Yy]'
}

# --- 0. Elevation: relaunch self elevated if needed -------------------------
$isElevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isElevated) {
    Write-Banner "TheBrowserProject Setup Wizard"
    Write-Host "This needs administrator rights to configure Git, Windows long-path"
    Write-Host "support, and a Defender exclusion. Relaunching elevated..."
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-RepoRoot", "`"$RepoRoot`"", "-InstallRoot", "`"$InstallRoot`"")
    if ($SkipBuild) { $argList += "-SkipBuild" }
    # Start-Process is a cmdlet, not a native command — it never sets $LASTEXITCODE,
    # so a caller checking it afterward would see a stale/zero value even when the
    # elevated child actually failed. Capture the process via -PassThru and use its
    # real ExitCode instead.
    $elevatedProc = Start-Process pwsh -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $elevatedProc.ExitCode
}

Write-Banner "TheBrowserProject Setup Wizard"
Write-Host "Repo:         $RepoRoot"
Write-Host "Install root: $InstallRoot  (depot_tools/, chromium/ go here)"
Write-Host ""

# --- 1. Disk space check -----------------------------------------------------
# InstallRoot may not exist yet on a fresh machine — Get-Item would fail before
# bootstrap ever gets a chance to create it. Create it first; it's the wizard's
# job to manage this directory anyway.
if (-not (Test-Path $InstallRoot)) {
    New-Item -ItemType Directory -Path $InstallRoot -Force | Out-Null
}
Write-Step "Checking free disk space on $((Get-Item $InstallRoot).PSDrive.Name):"
$drive = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$((Get-Item $InstallRoot).PSDrive.Name):'"
$freeGb = [math]::Round($drive.FreeSpace / 1GB, 1)
if ($freeGb -lt 100) {
    Write-Host "    Only $freeGb GB free — Chromium needs ~100GB (checkout + build output)." -ForegroundColor Yellow
    if (-not (Confirm-Step "Continue anyway?" $false)) {
        throw "Aborted: not enough free disk space at $InstallRoot"
    }
} else {
    Write-Ok "$freeGb GB free"
}

# --- 2. Visual Studio 2022 + C++ workload -----------------------------------
Write-Step "Checking for Visual Studio 2022 with the C++ workload"
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$vs2022Path = $null
if (Test-Path $vswhere) {
    $vs2022Path = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -version "[17.0,18.0)" -property installationPath
}
if ($vs2022Path) {
    Write-Ok "found at $vs2022Path"
} else {
    Write-Host "    Not found. This installs Visual Studio 2022 Build Tools + the C++"
    Write-Host "    workload via winget — a real, multi-GB download."
    if (Confirm-Step "Install VS2022 Build Tools now?") {
        Write-Step "Installing VS2022 Build Tools (this takes a while)..."
        winget install --id Microsoft.VisualStudio.2022.BuildTools --silent --accept-package-agreements --accept-source-agreements `
            --override "--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
        if ($LASTEXITCODE -ne 0) { throw "winget install of VS2022 Build Tools failed (exit $LASTEXITCODE)" }
        $vs2022Path = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -version "[17.0,18.0)" -property installationPath
        if (-not $vs2022Path) { throw "VS2022 install reported success but vswhere still can't find it — check manually." }
        Write-Ok "installed at $vs2022Path"
    } else {
        throw "VS2022 with the C++ workload is required. Install it manually, then re-run this script."
    }
}

# --- 3. Git: long-path support + safe.directory (system-wide) ---------------
Write-Step "Configuring Git and Windows for long paths"
$longPathsKey = "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem"
if ((Get-ItemProperty $longPathsKey -Name LongPathsEnabled -ErrorAction SilentlyContinue).LongPathsEnabled -eq 1) {
    Write-Skip "LongPathsEnabled already set"
} else {
    New-ItemProperty -Path $longPathsKey -Name "LongPathsEnabled" -Value 1 -PropertyType DWORD -Force | Out-Null
    Write-Ok "set LongPathsEnabled=1 (Chromium's source tree has paths that exceed the old 260-char limit)"
}
if ((git config --system --get core.longpaths) -eq "true") {
    Write-Skip "git core.longpaths already set"
} else {
    git config --system core.longpaths true
    Write-Ok "set git core.longpaths=true"
}
# Defensive: a moved/relocated checkout, or a build/service account running under
# a different identity, can trip git's ownership check. Register only the exact
# paths this wizard manages — not `*`, which would trust every repo on the
# machine for every user/service, including anything an attacker might drop
# somewhere unrelated.
$safeDirs = @($RepoRoot, "$InstallRoot\chromium\src", "$InstallRoot\depot_tools")
$existingSafeDirs = @(git config --system --get-all safe.directory 2>$null)
foreach ($dir in $safeDirs) {
    $gitStyleDir = $dir -replace '\\', '/'
    if ($existingSafeDirs -contains $gitStyleDir -or $existingSafeDirs -contains $dir) {
        Write-Skip "git safe.directory already trusts $dir"
    } else {
        git config --system --add safe.directory $gitStyleDir
        Write-Ok "added git safe.directory: $dir"
    }
}

# --- 4. Windows Defender exclusion ------------------------------------------
Write-Step "Adding a Defender exclusion for $InstallRoot"
Write-Host "    Chromium's build downloads many prebuilt binaries; real-time scanning" -ForegroundColor DarkGray
Write-Host "    both slows the build a lot and can falsely quarantine legitimate ones." -ForegroundColor DarkGray
$existingExclusions = (Get-MpPreference).ExclusionPath
if ($existingExclusions -contains $InstallRoot) {
    Write-Skip "already excluded"
} else {
    Add-MpPreference -ExclusionPath $InstallRoot
    Write-Ok "excluded $InstallRoot from real-time scanning"
}

# --- 5. Bootstrap: depot_tools + gclient sync (~100GB, first run only) ------
Write-Banner "Chromium sync"
Write-Host "Next: cloning depot_tools and syncing the pinned Chromium revision."
Write-Host "First run is a full ~100GB download — can take hours depending on your"
Write-Host "connection. Safe to re-run if interrupted; it resumes/verifies rather"
Write-Host "than starting over."
if (Confirm-Step "Proceed with sync now?") {
    & "$RepoRoot\scripts\bootstrap.ps1" -RepoRoot $RepoRoot -DepotToolsDir "$InstallRoot\depot_tools" -ChromiumDir "$InstallRoot\chromium"
} else {
    Write-Host "Skipping sync — re-run this script (or bootstrap.ps1 directly) when ready." -ForegroundColor Yellow
    exit 0
}

# --- 6. Detect and install the exact Windows SDK this Chromium revision needs
Write-Banner "Windows SDK"
$chromiumSrc = "$InstallRoot\chromium\src"
$setupToolchain = "$chromiumSrc\build\toolchain\win\setup_toolchain.py"
$requiredSdk = $null
if (Test-Path $setupToolchain) {
    $match = Select-String -Path $setupToolchain -Pattern "^SDK_VERSION\s*=\s*'([\d.]+)'" | Select-Object -First 1
    if ($match) { $requiredSdk = $match.Matches[0].Groups[1].Value }
}
if (-not $requiredSdk) {
    Write-Host "    Could not detect the required SDK version from setup_toolchain.py — skipping." -ForegroundColor Yellow
} else {
    Write-Step "This Chromium revision requires Windows SDK $requiredSdk"
    $sdkPath = "C:\Program Files (x86)\Windows Kits\10\Include\$requiredSdk\um"
    if (Test-Path $sdkPath) {
        Write-Ok "already installed"
    } else {
        Write-Host "    Not installed. Installing via the Visual Studio Installer — another"
        Write-Host "    real download."
        if (Confirm-Step "Install Windows SDK $requiredSdk now?") {
            # e.g. "10.0.28000.0" -> component id suffix "28000"
            $sdkBuild = ($requiredSdk -split '\.')[2]
            $componentId = "Microsoft.VisualStudio.Component.Windows11SDK.$sdkBuild"
            $vsInstaller = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vs_installer.exe"
            & $vsInstaller modify --installPath $vs2022Path --add $componentId --quiet --norestart --force
            if (-not (Test-Path $sdkPath)) {
                throw "SDK install for $requiredSdk did not complete — component '$componentId' may not exist in your VS channel. Try adding it manually via the Visual Studio Installer (Individual Components -> search 'Windows 11 SDK')."
            }
            Write-Ok "installed Windows SDK $requiredSdk"
        } else {
            Write-Host "    build.ps1 will fail at gn gen without this — install it before building." -ForegroundColor Yellow
        }
    }
}

# --- 7. Apply overlay + patches ---------------------------------------------
Write-Banner "Applying patches"
& "$RepoRoot\scripts\apply-patches.ps1" -RepoRoot $RepoRoot -ChromiumSrc $chromiumSrc

if ($SkipBuild) {
    Write-Banner "Setup complete (build skipped)"
    Write-Host "Run scripts\build.ps1 when you're ready to compile."
    exit 0
}

# --- 8. Build ----------------------------------------------------------------
Write-Banner "Build"
Write-Host "Next: the actual compile. Even on fast hardware this typically takes"
Write-Host "1-3+ hours the first time."
if (Confirm-Step "Start the build now?") {
    & "$RepoRoot\scripts\build.ps1" -RepoRoot $RepoRoot -ChromiumSrc $chromiumSrc -DepotToolsDir "$InstallRoot\depot_tools"
} else {
    Write-Host "Skipping build — run scripts\build.ps1 when ready." -ForegroundColor Yellow
    exit 0
}

# --- 9. Smoke test -----------------------------------------------------------
Write-Banner "Verifying the build"
$exe = "$chromiumSrc\out\Release\TheBrowserProject.exe"
if (-not (Test-Path $exe)) {
    throw "Build finished but $exe is missing — something's wrong."
}
& $exe --headless --disable-gpu --dump-dom "about:blank" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "$exe exited with code $LASTEXITCODE" }
Write-Ok "$exe launches and exits cleanly"

Write-Banner "Done — welcome to TheBrowserProject"
Write-Host "Binary: $exe"
