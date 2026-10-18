# Builds and runs all tests.  scripts\test.ps1 [-Config release|debug] [-Filter pattern]
param([string]$Config = 'release', [string]$Filter = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'build.ps1') -Config $Config
. (Join-Path $PSScriptRoot 'dev-env.ps1')
Set-Location $root
$args = @('--preset', $Config)
if ($Filter) { $args += @('-R', $Filter) }
ctest @args
if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
Write-Host '[test] all green' -ForegroundColor Green
