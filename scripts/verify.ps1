<#
.SYNOPSIS
Configures, builds and tests Composer with the 64-bit MSVC toolchain.

.DESCRIPTION
Runs one CMake workflow preset (configure, build, test) from the repository root.
When the current shell is not already an x64 MSVC developer environment, the
newest Visual Studio installation with the C++ x64 tools is located with
vswhere and its environment is imported for this process only.

.PARAMETER Configuration
Debug or Release. Selects the windows-debug or windows-release workflow preset.

.PARAMETER Clean
Removes the preset's binary directory first, so the run starts from a clean
configure and refetches the pinned dependencies.

.EXAMPLE
pwsh -NoProfile -File scripts/verify.ps1 -Configuration Release -Clean
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

function Test-MsvcEnvironment {
    $compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
    return $null -ne $compiler -and $env:VSCMD_ARG_TGT_ARCH -eq 'x64'
}

function Import-MsvcEnvironment {
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $installer)) {
        throw "vswhere.exe was not found at $installer; install Visual Studio or its Build Tools with the C++ workload."
    }
    $installation = & $installer -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) {
        throw 'No Visual Studio installation with the C++ x64 tools was found.'
    }
    $vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars)) {
        throw "vcvars64.bat was not found at $vcvars."
    }
    Write-Host "Using the MSVC environment from $installation"
    # vcvars64.bat looks for vswhere.exe on PATH.
    $env:PATH = "$(Split-Path -Parent $installer);$env:PATH"
    # cmd.exe parses its own command line; pass the quoted string through unchanged whatever
    # argument-passing mode the session uses.
    $PSNativeCommandArgumentPassing = 'Legacy'
    $lines = & cmd.exe /d /s /c "`"$vcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "vcvars64.bat failed with exit code $LASTEXITCODE."
    }
    foreach ($line in $lines) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            [Environment]::SetEnvironmentVariable($line.Substring(0, $separator), $line.Substring($separator + 1), 'Process')
        }
    }
    if (-not (Test-MsvcEnvironment)) {
        throw 'The imported environment does not provide the x64 MSVC compiler.'
    }
}

function Invoke-Checked {
    param([string] $Command, [string[]] $Arguments)
    Write-Host "> $Command $($Arguments -join ' ')"
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command exited with code $LASTEXITCODE."
    }
}

$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root

if (-not (Test-MsvcEnvironment)) {
    Import-MsvcEnvironment
}
foreach ($tool in 'cmake', 'ninja') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool was not found on PATH; install the Visual Studio C++ CMake tools or add $tool to PATH."
    }
}

$preset = "windows-$($Configuration.ToLowerInvariant())"
if ($Clean) {
    $binary = Join-Path $root 'out\windows'
    if (Test-Path -LiteralPath $binary) {
        Write-Host "Removing $binary"
        Remove-Item -LiteralPath $binary -Recurse -Force
    }
}

Write-Host "MSVC tools $env:VCToolsVersion, Windows SDK $("$env:WindowsSDKVersion".TrimEnd('\'))"
Invoke-Checked cmake @('--version')
Invoke-Checked ninja @('--version')
Invoke-Checked cmake @('--workflow', '--preset', $preset)
