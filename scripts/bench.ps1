# Cutline benchmark: measures the plan's performance budgets on this PC and writes
#   bench-results-<date>.json   (raw numbers)
#   bench-results-<date>.txt    (readable PASS/FAIL summary)
# Works from the repo (after scripts\build.ps1) and from the portable package.
#   powershell -ExecutionPolicy Bypass -File bench.ps1 [-Full]
#   -Full: 10-min reference media and 5-minute playback runs (default: short media, 60 s runs)
param([switch]$Full)
$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot
if (Test-Path (Join-Path $here 'tools\mediabench.exe')) {
    $tools = Join-Path $here 'tools'; $gen = Join-Path $tools 'gen-testmedia.ps1'
    $ffmpeg = Join-Path $tools 'ffmpeg.exe'; $app = Join-Path $here 'Cutline.exe'; $outDir = $here
} else {
    $root = Split-Path -Parent $here
    $tools = Join-Path $root 'build\release\bin'; $gen = Join-Path $here 'gen-testmedia.ps1'
    $ffmpeg = Join-Path $root 'deps\ffmpeg\bin\ffmpeg.exe'; $app = Join-Path $tools 'cutline.exe'; $outDir = $root
}
$mb = Join-Path $tools 'mediabench.exe'
$ui = Join-Path $tools 'uibench.exe'
if (-not (Test-Path $mb)) { throw "mediabench.exe not found in $tools (build first)" }
$media = Join-Path $env:LOCALAPPDATA ($(if ($Full) { 'CutlineBench\media-full' } else { 'CutlineBench\media' }))
$playSecs = if ($Full) { 300 } else { 60 }
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$base = Join-Path $outDir "bench-results-$stamp"  # (no computer name: results are shareable)
$results = New-Object System.Collections.ArrayList
function Say($m) { Write-Host "[bench] $m" -ForegroundColor Cyan }
function Run([string]$label, [string[]]$argv) {
    Say $label
    $out = & $mb @argv 2>$null
    foreach ($line in $out) {
        if ($line -match '^\{') {
            try { $o = $line | ConvertFrom-Json; $o | Add-Member label $label -Force; [void]$results.Add($o); Write-Host "        $line" }
            catch { Write-Host "        (unparsed) $line" }
        }
    }
}

# --- machine ------------------------------------------------------------------------------------
$gpu = (Get-CimInstance Win32_VideoController | Sort-Object AdapterRAM -Descending | Select-Object -First 1).Name
$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
$ram = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
$machine = [pscustomobject]@{ test = 'machine'; gpu = $gpu; cpu = $cpu.Trim(); ram_gb = $ram;
    os = (Get-CimInstance Win32_OperatingSystem).Caption; full = [bool]$Full }
[void]$results.Add($machine)
Say "$gpu | $($cpu.Trim()) | $ram GB RAM"

# --- media ----------------------------------------------------------------------------------------
Say "generating test media in $media (first run only)"
$genArgs = @('-ExecutionPolicy', 'Bypass', '-File', $gen, '-FFmpeg', $ffmpeg, '-OutDir', $media)
if (-not $Full) { $genArgs += '-Quick' }
& powershell @genArgs | Out-Null
$proj = Join-Path $media 'projects'
New-Item -ItemType Directory -Force $proj | Out-Null
& $mb makeprojects $proj --media $media | Out-Null
$main = Join-Path $media 'main_1440p60_h264.mkv'
$uw = Join-Path $media 'uw_3440x1440p60_hevc.mkv'

# --- benchmarks ---------------------------------------------------------------------------------------
Run 'decode 1440p H.264 (GPU)' @('decode', $main, '--seconds', '30')
Run 'decode 3440x1440 HEVC (GPU)' @('decode', $uw, '--seconds', '30')
Run 'decode 1440p H.264 (CPU fallback)' @('decode', $main, '--seconds', '10', '--sw')
Run 'seek originals 1440p' @('seek', $main, '--count', '40')
Run 'seek originals ultrawide' @('seek', $uw, '--count', '40')
$proxyDir = Join-Path $media 'proxies'
New-Item -ItemType Directory -Force $proxyDir | Out-Null
Run 'proxy build 1440p' @('makeproxy', $main, '--out', (Join-Path $proxyDir 'main_proxy.mp4'))
Run 'seek proxy 1440p' @('seek', (Join-Path $proxyDir 'main_proxy.mp4'), '--count', '60')
Run 'composite 3 layers 1440p' @('composite', $main, '--layers', '3')
Run "playback 1440p60 + colour, ${playSecs}s" @('playback', (Join-Path $proj 'bench_1440p.cutproj'), '--seconds', "$playSecs")
Run "playback 3440x1440p60 + colour, ${playSecs}s" @('playback', (Join-Path $proj 'bench_ultrawide.cutproj'), '--seconds', "$playSecs")
Run "playback 3 stacked tracks, ${playSecs}s" @('playback', (Join-Path $proj 'bench_3layers.cutproj'), '--seconds', "$playSecs")
Run 'playback speed ramps, 45s' @('playback', (Join-Path $proj 'bench_ramp.cutproj'), '--seconds', '45')
Run 'playback nested 3 layers, 30s' @('playback', (Join-Path $proj 'bench_nested.cutproj'), '--seconds', '30')
Run 'export 1440p60 NVENC with effects' @('export', (Join-Path $proj 'bench_1440p.cutproj'), '--seconds', $(if ($Full) { '300' } else { '60' }))
Run 'export 1440p60 libx264 fallback (20 s)' @('export', (Join-Path $proj 'bench_1440p.cutproj'), '--seconds', '20', '--codec', 'libx264')
Say 'timeline paint, 750 clips'
$env:CUTLINE_TEST = '1'
$uo = & $ui (Join-Path $proj 'bench_500clips.cutproj') 2>$null
foreach ($line in $uo) { if ($line -match '^\{') { $o = $line | ConvertFrom-Json; $o | Add-Member label 'timeline paint' -Force; [void]$results.Add($o); Write-Host "        $line" } }
Remove-Item Env:\CUTLINE_TEST

# cold start + idle CPU of the real app (isolated settings folder, so no prompts)
Say 'app cold start and idle CPU'
$appData = Join-Path $env:TEMP 'cutline-bench-appdata'
Remove-Item -Recurse -Force $appData -ErrorAction SilentlyContinue
$env:CUTLINE_APPDATA = $appData
$p = Start-Process $app -ArgumentList (Join-Path $proj 'bench_1440p.cutproj') -PassThru
Start-Sleep -Seconds 8
$c0 = (Get-Process -Id $p.Id).CPU; Start-Sleep -Seconds 10; $c1 = (Get-Process -Id $p.Id).CPU
$ws = (Get-Process -Id $p.Id).WorkingSet64 / 1MB
Stop-Process -Id $p.Id -Force
Remove-Item Env:\CUTLINE_APPDATA
$startMs = $null
$log = Join-Path $appData 'cutline.log'
if (Test-Path $log) { $m = Select-String $log -Pattern 'interactive after (\d+) ms' | Select-Object -Last 1; if ($m) { $startMs = [int]$m.Matches[0].Groups[1].Value } }
$idle = ($c1 - $c0) / 10 / [Environment]::ProcessorCount * 100
[void]$results.Add([pscustomobject]@{ test = 'app'; label = 'app startup / idle'; startup_ms = $startMs; idle_cpu_pct = [math]::Round($idle, 3); working_set_mb = [math]::Round($ws) })

# --- summary ----------------------------------------------------------------------------------------------
$results | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 "$base.json"
function Get([string]$label) { $results | Where-Object { $_.label -eq $label } | Select-Object -First 1 }
$lines = @("Cutline benchmark  $stamp", "PC: $gpu | $($cpu.Trim()) | $ram GB", "")
function Check([string]$name, [bool]$ok, [string]$detail) { $script:lines += ("{0}  {1,-48} {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail) }
$pb = Get "playback 1440p60 + colour, ${playSecs}s"
if ($pb) { Check "Playback 1440p60 (0 dropped)" ($pb.dropped -eq 0 -and $pb.audio_underruns -eq 0) "dropped $($pb.dropped), late $($pb.late), fps avg $($pb.fps_avg), audio underruns $($pb.audio_underruns), decoders GPU/CPU $($pb.decoders_gpu)/$($pb.decoders_cpu)" }
$pu = Get "playback 3440x1440p60 + colour, ${playSecs}s"
if ($pu) { Check "Playback 3440x1440p60 (<1% dropped)" ($pu.dropped_pct -lt 1) "dropped $($pu.dropped_pct)%, fps avg $($pu.fps_avg)" }
$p3 = Get "playback 3 stacked tracks, ${playSecs}s"
if ($p3) { Check "3 stacked tracks (60 fps or auto 1/2, no audio glitch)" (($p3.dropped_pct -lt 1 -or $p3.preview -ne 'Full') -and $p3.audio_underruns -eq 0) "dropped $($p3.dropped_pct)%, preview $($p3.preview), underruns $($p3.audio_underruns)" }
$pr = Get 'playback speed ramps, 45s'
if ($pr) { Check "Speed ramps (many short clips): no drops or audio glitches" ($pr.dropped_pct -lt 1 -and $pr.audio_underruns -eq 0) "dropped $($pr.dropped_pct)%, underruns $($pr.audio_underruns), peak $($pr.rss_mb) MB" }
$pn = Get 'playback nested 3 layers, 30s'
if ($pn) { Check "Nested sequence playback (60 fps)" ($pn.dropped_pct -lt 1 -and $pn.audio_underruns -eq 0) "dropped $($pn.dropped_pct)%, fps min $($pn.fps_min)" }
$so = Get 'seek originals 1440p'
if ($so) { Check "Seek long-GOP original (<250 ms)" ($so.ms_max -lt 250) "avg $($so.ms_avg) ms, p95 $($so.ms_p95) ms, max $($so.ms_max) ms" }
$sp = Get 'seek proxy 1440p'
if ($sp) { Check "Seek with proxy (<80 ms)" ($sp.ms_p95 -lt 80) "avg $($sp.ms_avg) ms, p95 $($sp.ms_p95) ms, max $($sp.ms_max) ms" }
$tp = Get 'timeline paint'
if ($tp) { Check "Timeline 750 clips pan/zoom (60 fps, no frame >33 ms)" ($tp.ms_max -lt 33 -and $tp.ms_p99 -lt 16.7) "avg $($tp.ms_avg) ms, p99 $($tp.ms_p99) ms, max $($tp.ms_max) ms" }
$ex = Get 'export 1440p60 NVENC with effects'
if ($ex) { Check "Export 1440p60 NVENC (>=2x realtime)" ($ex.x_realtime -ge 2) "$($ex.x_realtime)x, $($ex.encoder)" }
$a = Get 'app startup / idle'
Check "Cold start (<2 s)" ($a.startup_ms -and $a.startup_ms -lt 2000) "$($a.startup_ms) ms"
Check "Idle CPU while paused (<2%)" ($a.idle_cpu_pct -lt 2) "$($a.idle_cpu_pct)%"
$rss = @($pb, $pu, $p3 | Where-Object { $_ } | ForEach-Object { $_.rss_mb }) | Measure-Object -Maximum
Check "Memory (<2 GB RAM)" ($rss.Maximum -lt 2048) "peak private bytes during playback $([math]::Round($rss.Maximum)) MB, app idle working set $($a.working_set_mb) MB"
$lines += ""
$lines += "Raw numbers: $base.json"
$lines | Set-Content -Encoding utf8 "$base.txt"
Write-Host ""
$lines | ForEach-Object { Write-Host $_ }
Write-Host "`n[bench] wrote $base.json and $base.txt" -ForegroundColor Green
