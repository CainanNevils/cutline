# Configure + build.  scripts\build.ps1 [-Config release|debug] [-Clean] [-Target name]
param([string]$Config = 'release', [switch]$Clean, [string]$Target = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'dev-env.ps1')
Set-Location $root
$bdir = Join-Path $root "build\$Config"
if ($Clean -and (Test-Path $bdir)) { Remove-Item -Recurse -Force $bdir }
if (-not (Test-Path (Join-Path $bdir 'build.ninja'))) {
    cmake --preset $Config
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
}
if ($Target) { cmake --build --preset $Config --target $Target } else { cmake --build --preset $Config }
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

# Qt runtime next to the executables (windeployqt only copies what changed).
$wdq = Join-Path $root 'deps\qt\6.8.3\msvc2022_64\bin\windeployqt.exe'
$mode = if ($Config -eq 'debug') { '--debug' } else { '--release' }
foreach ($name in 'cutline.exe', 'mediabench.exe', 'rendertest.exe', 'e2e.exe', 'uibench.exe', 'recoverytest.exe', 'uishots.exe', 'uitest.exe') {
    $exe = Join-Path $bdir "bin\$name"
    $stamp = "$exe.deployed"
    if ((Test-Path $exe) -and -not ((Test-Path $stamp) -and ((Get-Item $stamp).LastWriteTime -ge (Get-Item $exe).LastWriteTime))) {
        & $wdq $mode --no-translations --no-system-d3d-compiler --no-opengl-sw --no-quick-import $exe | Out-Null
        if ($LASTEXITCODE -eq 0) { Set-Content $stamp '' }
    }
}
# Headless tools (rendertest, e2e) run with QT_QPA_PLATFORM=offscreen; windeployqt only ships 'windows'.
$plat = Join-Path $bdir 'bin\platforms'
$suffix = if ($Config -eq 'debug') { 'd' } else { '' }
$off = Join-Path $root "deps\qt\6.8.3\msvc2022_64\plugins\platforms\qoffscreen$suffix.dll"
if ((Test-Path $plat) -and -not (Test-Path (Join-Path $plat "qoffscreen$suffix.dll"))) { Copy-Item $off $plat }
Write-Host "[build] OK ($Config)" -ForegroundColor Green
