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

# See bootstrap.ps1 — process-local, doesn't carry over between CI steps.
$env:GIT_TERMINAL_PROMPT = "0"
$env:GCM_INTERACTIVE = "Never"

function Write-Step($msg) {
    Write-Host "==> $msg" -ForegroundColor Cyan
}

# Walks $Path component by component and substitutes any NTFS junction with its
# real target, recursing to handle nested junctions. Filesystem access through a
# junction works fine for normal file I/O (git, gn gen etc. have no issue with
# it), but siso.exe's own internal Go path-walking (used for its CIPD
# version/self-identification logic, exercised on every invocation including a
# bare `siso version`) breaks through one and fails instantly with a bare
# "The system cannot find the path specified." Confirmed directly: the identical
# `siso ninja` invocation failed at 0.00s through the junctioned checkout path
# (C:\actions-runner\TheBrowserProject -> junction -> V:\git\.runner\...) but ran
# normally — actually compiling — given the real V:\ path instead. Only siso's
# own invocation needs the dereferenced path; everything else in this script can
# keep using the junctioned $RepoRoot/$ChromiumSrc as before.
function Resolve-RealPath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    $root = [System.IO.Path]::GetPathRoot($full)
    $relParts = $full.Substring($root.Length).Split([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar) | Where-Object { $_ }
    $current = $root.TrimEnd('\', '/')
    for ($i = 0; $i -lt $relParts.Length; $i++) {
        $current = Join-Path $current $relParts[$i]
        if (Test-Path $current) {
            $item = Get-Item $current -Force -ErrorAction SilentlyContinue
            if ($item -and $item.LinkType -eq "Junction" -and $item.Target) {
                $target = $item.Target
                if ($target -is [array]) { $target = $target[0] }
                $resolved = $target
                if ($i + 1 -le $relParts.Length - 1) {
                    $remainder = ($relParts[($i + 1)..($relParts.Length - 1)] -join [System.IO.Path]::DirectorySeparatorChar)
                    $resolved = Join-Path $target $remainder
                }
                return Resolve-RealPath $resolved
            }
        }
    }
    return $current
}

if (-not (Test-Path $ChromiumSrc)) {
    throw "Chromium checkout not found at $ChromiumSrc — run bootstrap.ps1 first"
}

$env:PATH = "$DepotToolsDir;$env:PATH"
# bootstrap.ps1 sets these too, but they're process-local — CI runs each script as
# a separate step/process, so they don't carry over. Without DEPOT_TOOLS_WIN_TOOLCHAIN,
# gn/ninja may try to fetch depot_tools' bundled Windows toolchain instead of the
# installed VS one. Without DEPOT_TOOLS_UPDATE=0, `gn` (also a depot_tools tool)
# can trigger the same self-update-on-invocation hang seen in bootstrap.
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = "0"
$env:DEPOT_TOOLS_UPDATE = "0"

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
# install alongside VS2022, that preferred version can be a preview SDK that was
# never actually installed — vcvarsall then reports a nonexistent include path
# and gn gen fails. Derive the EXACT version this pinned Chromium revision
# requires (same SDK_VERSION constant setup.ps1 reads from setup_toolchain.py),
# not just whatever's newest installed — a machine with both the required SDK
# and a newer one present would otherwise silently build against the wrong SDK,
# defeating the pin and reintroducing the exact mismatch this exists to prevent.
$sdkRoot = "C:\Program Files (x86)\Windows Kits\10\Include"
$setupToolchain = Join-Path $ChromiumSrc "build\toolchain\win\setup_toolchain.py"
$requiredSdk = $null
if (Test-Path $setupToolchain) {
    $match = Select-String -Path $setupToolchain -Pattern "^SDK_VERSION\s*=\s*'([\d.]+)'" | Select-Object -First 1
    if ($match) { $requiredSdk = $match.Matches[0].Groups[1].Value }
}
if ($requiredSdk) {
    $requiredSdkPath = Join-Path $sdkRoot $requiredSdk
    if (-not (Test-Path $requiredSdkPath)) {
        throw "Chromium requires Windows SDK $requiredSdk, which isn't installed at $requiredSdkPath. Run scripts\setup.ps1 (or add it manually via the Visual Studio Installer) before building."
    }
    $env:WindowsSDKVersion = "$requiredSdk\"
    Write-Step "Pinned Windows SDK version to $($env:WindowsSDKVersion) (required by this Chromium revision)"
} elseif (Test-Path $sdkRoot) {
    Write-Warning "Could not detect the required SDK version from setup_toolchain.py — falling back to newest installed. This may not match what Chromium actually needs."
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

    # Calling siso directly rather than through the `autoninja` wrapper.
    # autoninja fails instantly in CI's non-interactive execution context with an
    # unhelpful "Error: The system cannot find the path specified." — most likely
    # from depot_tools' gclient_paths.FindGclientRoot() walking up the directory
    # tree to locate the ninja/siso binary and resolving it differently there than
    # in an interactive session (this project's checkout sits behind an NTFS
    # junction, which is a plausible trigger). Direct siso invocation has been
    # 100% reliable across every build today; autoninja's wrapper logic mainly
    # adds -j core-count tuning, which GN already bakes into the generated
    # .siso_config for this machine (see gn_logs:cpu_count etc. in that file), so
    # nothing meaningful is lost by skipping the wrapper.
    $sisoPathViaJunction = Join-Path $ChromiumSrc "third_party\siso\cipd\siso.exe"
    if (-not (Test-Path $sisoPathViaJunction)) { throw "siso.exe not found at $sisoPathViaJunction" }
    # Resolving the -C argument and the binary path alone (previous two attempts)
    # still failed identically. The remaining culprit: this whole script has been
    # sitting in Push-Location $ChromiumSrc (the junctioned path) since the top,
    # and Push-Location calls SetCurrentDirectory with that literal string —
    # Windows does not resolve reparse points when setting cwd, so the process's
    # actual OS-level working directory stays the junctioned path no matter what
    # -C argument siso is given. Go's os.Getwd() (whatever siso's CIPD/path logic
    # actually reads) would still see the junctioned path. Fix: actually change
    # directory into the resolved real path before invoking siso, then change
    # back. This does not touch any cache file; it only changes which literal
    # path siso's process is launched from, so this build resumes from whatever's
    # already in .siso_deps / .siso_fs_state exactly as before.
    $sisoPath = Resolve-RealPath $sisoPathViaJunction
    $realOutPath = Resolve-RealPath $outPath
    Write-Step "siso ninja -C $realOutPath $Target (via $sisoPath)"
    Push-Location $realOutPath
    try {
        & $sisoPath ninja -C $realOutPath $Target
        if ($LASTEXITCODE -ne 0) { throw "siso build failed" }
    } finally {
        Pop-Location
    }

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
