<#
.SYNOPSIS
    Defines Get-PatchStateHash, shared by bootstrap.ps1 and apply-patches.ps1 to
    decide whether chromium.version + patches/ + overlay/ have changed since the
    last successful apply on this runner (see issue #12).

.DESCRIPTION
    Dot-source this file to bring Get-PatchStateHash into scope:
        . (Join-Path $PSScriptRoot "lib\Get-PatchStateHash.ps1")

    The hash covers exactly the inputs that determine what the patched tree
    should look like: the pinned version string, and every patch/overlay file's
    relative path and content (order-independent — sorted before hashing so
    filesystem enumeration order never affects the result).
#>

function Get-PatchStateHash {
    param(
        [Parameter(Mandatory)][string]$VersionFile,
        [Parameter(Mandatory)][string]$PatchesDir,
        [Parameter(Mandatory)][string]$OverlayDir
    )

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $ms = New-Object System.IO.MemoryStream
    try {
        $writer = New-Object System.IO.BinaryWriter($ms)

        $writer.Write([System.IO.File]::ReadAllBytes($VersionFile))

        if (Test-Path $OverlayDir) {
            $overlayFiles = Get-ChildItem -Path $OverlayDir -Recurse -File |
                Where-Object { $_.Name -ne ".gitkeep" } |
                Sort-Object FullName
            foreach ($f in $overlayFiles) {
                $relPath = $f.FullName.Substring($OverlayDir.Length).TrimStart('\', '/')
                $writer.Write([System.Text.Encoding]::UTF8.GetBytes($relPath))
                $writer.Write([System.IO.File]::ReadAllBytes($f.FullName))
            }
        }

        if (Test-Path $PatchesDir) {
            $patchFiles = Get-ChildItem -Path $PatchesDir -Filter "*.patch" | Sort-Object Name
            foreach ($f in $patchFiles) {
                $writer.Write([System.Text.Encoding]::UTF8.GetBytes($f.Name))
                $writer.Write([System.IO.File]::ReadAllBytes($f.FullName))
            }
        }

        $writer.Flush()
        $hashBytes = $sha256.ComputeHash($ms.ToArray())
        return [System.BitConverter]::ToString($hashBytes) -replace '-', ''
    } finally {
        $sha256.Dispose()
        $ms.Dispose()
    }
}
