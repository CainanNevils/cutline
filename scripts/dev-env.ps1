# Puts MSVC, the Windows SDK (fxc), CMake and Ninja on PATH for this PowerShell session.
# Usage:  . .\scripts\dev-env.ps1   (dot-source)
$ErrorActionPreference = 'Stop'
if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vswhere = Join-Path $installer 'vswhere.exe'
$env:PATH = "$installer;$env:PATH"
$vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio / Build Tools with the C++ x64 toolset found. Run scripts\setup.ps1.' }
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
