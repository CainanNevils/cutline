# Cutline toolchain setup. Repeatable: detects what exists, installs only what is missing.
#   powershell -ExecutionPolicy Bypass -File scripts\setup.ps1
# Installs Qt and FFmpeg into deps\ (git-ignored) so nothing lands outside the repo,
# except VS Build Tools / Python which go through winget when absent.
param(
    [string]$QtVersion = '6.8.3',
    [string]$FfmpegTag = 'n8.1'
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $root 'deps'
New-Item -ItemType Directory -Force $deps | Out-Null

function Say($m) { Write-Host "[setup] $m" -ForegroundColor Cyan }

# --- Visual Studio C++ toolset -------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = $null
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vs) {
    Say 'Installing Visual Studio 2022 Build Tools (C++ workload)...'
    winget install --id Microsoft.VisualStudio.2022.BuildTools --accept-package-agreements --accept-source-agreements `
        --override '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
    $vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'VS Build Tools install failed.' }
}
Say "MSVC: $vs"

# CMake + Ninja ship with the VS C++ workload; fall back to winget.
$vsCmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $vsCmake) -and -not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Say 'Installing CMake + Ninja...'
    winget install --id Kitware.CMake --accept-package-agreements --accept-source-agreements
    winget install --id Ninja-build.Ninja --accept-package-agreements --accept-source-agreements
}

# --- Python (for aqtinstall) -------------------------------------------------------
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) {
    Say 'Installing Python...'
    winget install --id Python.Python.3.12 --accept-package-agreements --accept-source-agreements
    $env:PATH = "$env:LOCALAPPDATA\Programs\Python\Python312;$env:PATH"
}

# --- Qt ------------------------------------------------------------------------------
$qtDir = Join-Path $deps "qt\$QtVersion\msvc2022_64"
if (-not (Test-Path (Join-Path $qtDir 'bin\Qt6Core.dll'))) {
    Say "Installing Qt $QtVersion (msvc2022_64) via aqtinstall..."
    python -m pip install --quiet --upgrade aqtinstall
    python -m aqt install-qt windows desktop $QtVersion win64_msvc2022_64 -m qtmultimedia -O (Join-Path $deps 'qt')
    if ($LASTEXITCODE -ne 0) { throw 'aqt install failed.' }
} else { Say "Qt $QtVersion present." }

# --- FFmpeg (GPL shared: d3d11va, nvenc, libx264, avfilter) ------------------------
$ffDir = Join-Path $deps 'ffmpeg'
if (-not (Test-Path (Join-Path $ffDir 'include\libavcodec\avcodec.h'))) {
    $ver = $FfmpegTag.TrimStart('n')
    $name = "ffmpeg-$FfmpegTag-latest-win64-gpl-shared-$ver"
    $url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/$name.zip"
    $zip = Join-Path $deps "$name.zip"
    Say "Downloading $url"
    Invoke-WebRequest $url -OutFile $zip
    $tmp = Join-Path $deps 'ffmpeg_extract'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive $zip $tmp
    if (Test-Path $ffDir) { Remove-Item -Recurse -Force $ffDir }
    Move-Item (Join-Path $tmp $name) $ffDir
    Remove-Item -Recurse -Force $tmp, $zip
} else { Say 'FFmpeg present.' }
$ffmpegExe = Join-Path $ffDir 'bin\ffmpeg.exe'
$enc = & $ffmpegExe -hide_banner -encoders 2>$null | Out-String
$hw = & $ffmpegExe -hide_banner -hwaccels 2>$null | Out-String
foreach ($need in 'h264_nvenc', 'hevc_nvenc', 'libx264', 'aac') {
    if ($enc -notmatch $need) { throw "FFmpeg build lacks encoder $need" }
}
if ($hw -notmatch 'd3d11va') { throw 'FFmpeg build lacks d3d11va' }
Say 'FFmpeg has d3d11va, nvenc, libx264, aac.'

# --- Header-only libraries (vendored into third_party, committed) -------------------
$tp = Join-Path $root 'third_party'
$headers = @{
    'nlohmann\json.hpp'  = 'https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp'
    'doctest\doctest.h'  = 'https://raw.githubusercontent.com/doctest/doctest/v2.4.12/doctest/doctest.h'
}
foreach ($k in $headers.Keys) {
    $dst = Join-Path $tp $k
    if (-not (Test-Path $dst)) {
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        Say "Fetching $k"
        Invoke-WebRequest $headers[$k] -OutFile $dst
    }
}

Say 'Done. Next: scripts\build.ps1'
