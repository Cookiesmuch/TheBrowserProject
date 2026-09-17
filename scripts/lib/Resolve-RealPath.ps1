<#
.SYNOPSIS
    Defines Resolve-RealPath, shared by build.ps1 and run-unit-tests.ps1.

.DESCRIPTION
    Dot-source this file to bring Resolve-RealPath into scope:
        . (Join-Path $PSScriptRoot "lib\Resolve-RealPath.ps1")

    Walks a path component by component and substitutes any NTFS junction with
    its real target, recursing to handle nested junctions.

    Normal filesystem I/O (git, gn gen, etc.) has no problem with junctions, so
    this is only needed for one thing: siso.exe. Its internal Go path-walking
    (used for CIPD self-identification, exercised on every invocation including
    a bare `siso version`) breaks through a junction and fails instantly with a
    bare "The system cannot find the path specified."

    Confirmed directly: the identical `siso ninja` invocation failed at 0.00s
    through the junctioned checkout path (C:\actions-runner\TheBrowserProject ->
    junction -> V:\git\.runner\...) but ran normally, actually compiling, when
    given the real V:\ path. Note that siso needs BOTH its own binary path and
    its working directory dereferenced — resolving only the arguments passed to
    it is not sufficient, because Windows does not resolve reparse points when
    setting a process's current directory.
#>

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
