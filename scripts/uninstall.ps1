# Removes Cutline installed by install.ps1: the program folder, its shortcuts and the
# Settings > Apps entry. Projects, settings and the media cache (%LOCALAPPDATA%\Cutline) are kept.
# Usage: uninstall.ps1 [-InstallDir DIR] [-Quiet]
param(
    [string]$InstallDir = $PSScriptRoot,
    [switch]$Quiet
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $InstallDir 'Cutline.exe'
if (-not (Test-Path $exe)) { throw "Cutline.exe not found in $InstallDir" }

if (-not $Quiet) {
    Add-Type -AssemblyName System.Windows.Forms
    $answer = [System.Windows.Forms.MessageBox]::Show(
        "Remove Cutline?`n`nYour projects and settings are kept.", 'Uninstall Cutline', 'OKCancel', 'Question')
    if ($answer -ne 'OK') { exit 1 }
}
if (Get-Process Cutline -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) {
    if (-not $Quiet) { [System.Windows.Forms.MessageBox]::Show('Close Cutline first (save your work), then uninstall again.', 'Uninstall Cutline') | Out-Null }
    exit 1
}

# Shortcuts that point at this copy only.
$shell = New-Object -ComObject WScript.Shell
foreach ($dir in [Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs')) {
    $l = Join-Path $dir 'Cutline.lnk'
    if ((Test-Path $l) -and $shell.CreateShortcut($l).TargetPath -eq $exe) { Remove-Item $l -Force }
}
# Settings > Apps entry, if it belongs to this copy.
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Cutline'
if ((Test-Path $key) -and (Get-ItemProperty $key).InstallLocation -eq $InstallDir) { Remove-Item $key -Force }

Set-Location $env:TEMP  # (not inside the folder being removed)
Remove-Item -Recurse -Force $InstallDir
if (-not $Quiet) { [System.Windows.Forms.MessageBox]::Show('Cutline was removed.', 'Uninstall Cutline') | Out-Null }
