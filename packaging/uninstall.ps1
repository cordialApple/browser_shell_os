<#
.SYNOPSIS
    Remove Peekbar's per-user logon autostart and installed files.
.DESCRIPTION
    Deletes the "Peekbar" HKCU Run value and the %LOCALAPPDATA%\Peekbar install
    directory. Per-user only: HKCU, no HKLM, no elevation.
    NOTE: the install dir also holds config.txt if you created one there; pass
    -KeepConfig to preserve it. Supports -WhatIf to preview.
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\uninstall.ps1
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [switch]$KeepConfig
)

$ErrorActionPreference = 'Stop'

$installDir = Join-Path $env:LOCALAPPDATA 'Peekbar'
$exe        = Join-Path $installDir 'peekbar.exe'
$runKey     = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

if ($PSCmdlet.ShouldProcess($runKey, "Remove autostart value 'Peekbar'")) {
    Remove-ItemProperty -Path $runKey -Name 'Peekbar' -ErrorAction SilentlyContinue
    Write-Host "Removed logon autostart value (if present)."
}

if ($KeepConfig) {
    if ((Test-Path -LiteralPath $exe) -and
        $PSCmdlet.ShouldProcess($exe, 'Remove peekbar.exe (config preserved)')) {
        Remove-Item -LiteralPath $exe -Force
        Write-Host "Removed peekbar.exe; kept config in $installDir."
    }
} else {
    if ((Test-Path -LiteralPath $installDir) -and
        $PSCmdlet.ShouldProcess($installDir, 'Remove install directory')) {
        Remove-Item -LiteralPath $installDir -Recurse -Force
        Write-Host "Removed install directory $installDir."
    }
}
