# Installs Cutline for the current user (no admin rights needed):
#   - copies the app to %LOCALAPPDATA%\Programs\Cutline (replacing an older version)
#   - Desktop and Start Menu shortcuts
#   - an entry in Settings > Apps (with Uninstall)
# Run it from an unpacked release folder (Install.cmd does that), or from the repo after
# .\scripts\package.ps1. Projects, settings and the media cache are never touched.
#
# Usage: install.ps1 [-Source DIR] [-InstallDir DIR] [-NoShortcuts] [-NoRegister] [-Launch]
param(
    [string]$Source = '',
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\Cutline'),
    [switch]$NoShortcuts,
    [switch]$NoRegister,
    [switch]$Launch
)
$ErrorActionPreference = 'Stop'

# Where the files come from: this script's folder if it holds Cutline.exe (a release
# folder), else the newest package in the repo's dist\.
if (-not $Source) {
    if (Test-Path (Join-Path $PSScriptRoot 'Cutline.exe')) {
        $Source = $PSScriptRoot
    } else {
        $pkg = Get-ChildItem (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist') -Directory -Filter 'Cutline-*' -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $pkg) { throw 'No package found. Run .\scripts\package.ps1 first, or pass -Source.' }
        $Source = $pkg.FullName
    }
}
if (-not (Test-Path (Join-Path $Source 'Cutline.exe'))) { throw "Cutline.exe not found in $Source" }
$exeTarget = Join-Path $InstallDir 'Cutline.exe'

# A running copy holds its files open, and closing it for the user could lose unsaved work.
$running = Get-Process Cutline -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exeTarget }
if ($running) {
    Write-Host 'Cutline is running. Save your work, close it, then run the installer again.' -ForegroundColor Yellow
    exit 1
}

# Replace an older version completely (only a folder that really is a Cutline install).
if (Test-Path $InstallDir) {
    if (-not (Test-Path $exeTarget)) { throw "$InstallDir exists but is not a Cutline install; not touching it." }
    Remove-Item -Recurse -Force $InstallDir
}
New-Item -ItemType Directory -Force $InstallDir | Out-Null
foreach ($item in Get-ChildItem $Source) {
    if ($item.Name -in 'install.ps1', 'Install.cmd') { continue }
    Copy-Item $item.FullName $InstallDir -Recurse -Force
}
$uninstaller = Join-Path $PSScriptRoot 'uninstall.ps1'  # next to this script (release folder or repo scripts\)
if (Test-Path $uninstaller) { Copy-Item $uninstaller $InstallDir -Force }

if (-not $NoShortcuts) {
    $shell = New-Object -ComObject WScript.Shell
    foreach ($dir in [Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs')) {
        $s = $shell.CreateShortcut((Join-Path $dir 'Cutline.lnk'))
        $s.TargetPath = $exeTarget
        $s.WorkingDirectory = $InstallDir
        $s.IconLocation = "$exeTarget,0"
        $s.Description = 'Cutline video editor'
        $s.Save()
    }
}

if (-not $NoRegister) {
    $version = (Get-Item $exeTarget).VersionInfo.ProductVersion
    $size = [int]((Get-ChildItem $InstallDir -Recurse -File | Measure-Object Length -Sum).Sum / 1KB)
    $key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Cutline'
    New-Item -Force $key | Out-Null
    $uninstall = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$InstallDir\uninstall.ps1`""
    $values = @{
        DisplayName = 'Cutline'; DisplayVersion = $version; Publisher = 'Cutline'
        DisplayIcon = "$exeTarget,0"; InstallLocation = $InstallDir
        UninstallString = $uninstall; QuietUninstallString = "$uninstall -Quiet"
    }
    foreach ($k in $values.Keys) { Set-ItemProperty $key $k $values[$k] }
    foreach ($k in 'NoModify', 'NoRepair') { New-ItemProperty $key $k -Value 1 -PropertyType DWord -Force | Out-Null }
    New-ItemProperty $key 'EstimatedSize' -Value $size -PropertyType DWord -Force | Out-Null
}

Write-Host "Cutline installed in $InstallDir"
if ($Launch) { Start-Process $exeTarget }
