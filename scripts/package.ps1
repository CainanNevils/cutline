# Builds the release package: <OutDir>\Cutline-<version>-win64.zip (+ .sha256) and the same
# files unpacked in <OutDir>\Cutline-<version>-win64\.
# Contents: Cutline.exe + Qt/FFmpeg/VC++ runtime DLLs, Install.cmd / install.ps1 / uninstall.ps1,
# benchmark tools and scripts. Double-click Install.cmd to install, or run Cutline.exe in place.
# The version comes from project() in CMakeLists.txt. Requires a DirectX 11 GPU.
param([switch]$NoBuild, [string]$OutDir = 'dist')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $NoBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -Config release }
$Version = [regex]::Match((Get-Content (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(cutline VERSION ([0-9.]+)').Groups[1].Value
if (-not $Version) { throw 'version not found in CMakeLists.txt' }
$bin = Join-Path $root 'build\release\bin'
$name = "Cutline-$Version-win64"
$dist = if ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $root $OutDir }
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

# executables
Copy-Item (Join-Path $bin 'cutline.exe') (Join-Path $stage 'Cutline.exe')
$tools = Join-Path $stage 'tools'
New-Item -ItemType Directory -Force $tools | Out-Null
foreach ($t in 'mediabench.exe', 'uibench.exe', 'ffmpeg.exe', 'ffprobe.exe') { Copy-Item (Join-Path $bin $t) $tools }

# Qt runtime (windeployqt) for the app, then the same DLL set for the tools folder
$wdq = Join-Path $root 'deps\qt\6.8.3\msvc2022_64\bin\windeployqt.exe'
& $wdq --release --no-translations --no-system-d3d-compiler --no-opengl-sw --no-quick-import `
    --no-compiler-runtime (Join-Path $stage 'Cutline.exe') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }
foreach ($d in Get-ChildItem $stage -Filter 'Qt6*.dll') { Copy-Item $d.FullName $tools }
foreach ($sub in 'platforms', 'styles', 'imageformats', 'iconengines', 'multimedia', 'tls', 'networkinformation') {
    $src = Join-Path $stage $sub
    if (Test-Path $src) { Copy-Item -Recurse $src (Join-Path $tools $sub) }
}
Copy-Item (Join-Path $root 'deps\qt\6.8.3\msvc2022_64\plugins\platforms\qoffscreen.dll') (Join-Path $tools 'platforms')

# FFmpeg shared libraries
foreach ($d in Get-ChildItem (Join-Path $root 'deps\ffmpeg\bin') -Filter '*.dll') {
    Copy-Item $d.FullName $stage
    Copy-Item $d.FullName $tools
}

# Visual C++ runtime (app-local, so the target PC needs no redistributable install)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -prerelease -products * -property installationPath
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Directory | Sort-Object Name -Descending |
    ForEach-Object { Get-ChildItem (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue } |
    Select-Object -First 1
if (-not $crt) { throw 'VC++ runtime DLLs not found' }
foreach ($d in Get-ChildItem $crt.FullName -Filter '*.dll') {
    Copy-Item $d.FullName $stage
    Copy-Item $d.FullName $tools
}

# installer, scripts + docs
foreach ($f in 'Install.cmd', 'install.ps1', 'uninstall.ps1', 'bench.ps1') { Copy-Item (Join-Path $PSScriptRoot $f) $stage }
Copy-Item (Join-Path $PSScriptRoot 'gen-testmedia.ps1') $tools
foreach ($f in 'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.txt') { Copy-Item (Join-Path $root $f) $stage }
@"
Cutline $Version
==============
Install: double-click Install.cmd (no admin rights; adds Desktop and Start Menu shortcuts and an
entry in Settings > Apps to uninstall). Windows may say "Windows protected your PC" because the
app isn't code-signed: More info > Run anyway. You can also run Cutline.exe straight from this
folder. Settings, autosaves and the media cache live in %LOCALAPPDATA%\Cutline.
Requires Windows 10/11 x64 and a DirectX 11 GPU (NVIDIA recommended for hardware encoding).

Benchmark this PC: right-click bench.ps1 > Run with PowerShell (or
  powershell -ExecutionPolicy Bypass -File bench.ps1
). It generates synthetic test videos (no real footage needed), measures playback, seeking,
timeline drawing and export, and writes bench-results-<date>.json and .txt.

Cutline is free software under the GNU GPL v3 or later (LICENSE); source code:
https://github.com/CainanNevils/cutline. Bundled third-party components (FFmpeg GPL v3 build,
Qt 6 LGPL v3, ...) and where their source code is: THIRD_PARTY_NOTICES.txt.
"@ | Set-Content -Encoding utf8 (Join-Path $stage 'README.txt')

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$zip.sha256", "$hash  $name.zip`n")
$mb = [math]::Round((Get-Item $zip).Length / 1MB)
Write-Host "[package] $zip ($mb MB)" -ForegroundColor Green
