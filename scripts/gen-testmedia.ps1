# Generates synthetic test media into testmedia\ with FFmpeg lavfi (no real footage needed).
#   scripts\gen-testmedia.ps1 [-Force] [-Quick]   (-Quick: shorter main/ultrawide files)
param([switch]$Force, [switch]$Quick, [string]$FFmpeg = '', [string]$OutDir = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ff = if ($FFmpeg) { $FFmpeg } else { Join-Path $root 'deps\ffmpeg\bin\ffmpeg.exe' }
$out = if ($OutDir) { $OutDir } else { Join-Path $root 'testmedia' }
New-Item -ItemType Directory -Force $out | Out-Null

$encs = & $ff -hide_banner -encoders 2>$null | Out-String
# NVENC is listed even without an NVIDIA GPU; probe it with a tiny encode.
& $ff -hide_banner -loglevel error -f lavfi -i color=black:s=256x256:d=0.1 -c:v h264_nvenc -f null - 2>$null
$nv = ($LASTEXITCODE -eq 0) -and ($encs -match 'h264_nvenc')
Write-Host "[testmedia] NVENC: $nv"
$h264 = if ($nv) { @('-c:v', 'h264_nvenc', '-preset', 'p4', '-rc', 'vbr', '-cq', '23') } else { @('-c:v', 'libx264', '-preset', 'veryfast', '-crf', '20') }
$hevc = if ($nv) { @('-c:v', 'hevc_nvenc', '-preset', 'p4', '-rc', 'vbr', '-cq', '25') } else { @('-c:v', 'libx265', '-preset', 'veryfast', '-crf', '24') }

$font = 'C\:/Windows/Fonts/consola.ttf'
# Burned-in frame counter (source frame number) - used by frame-exact seek tests.
function Counter([int]$size) { "drawtext=fontfile='$font':text='%{frame_num}':x=40:y=40:fontsize=${size}:fontcolor=white:box=1:boxcolor=black@0.8:boxborderw=12" }

function Gen([string]$name) {
    # Callers write `Gen name @(a) + $codec + @(b)`; in command mode PowerShell passes the pieces
    # (and the '+' tokens) as separate arguments, so flatten them here.
    $ffargs = @()
    foreach ($a in $args) {
        if ($a -is [array]) { $ffargs += $a } elseif ($a -ne '+') { $ffargs += $a }
    }
    $dst = Join-Path $out $name
    if ((Test-Path $dst) -and -not $Force) { Write-Host "[testmedia] $name exists"; return }
    Write-Host "[testmedia] $name ..."
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $ff -hide_banner -loglevel error -y @ffargs $dst
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed for $name" }
    Write-Host ("[testmedia] {0} done in {1:n1}s" -f $name, $sw.Elapsed.TotalSeconds)
}

$mainDur = if ($Quick) { 60 } else { 600 }
$uwDur = if ($Quick) { 30 } else { 300 }

# 1) main reference: 2560x1440@60 H.264, GOP 250, game (stereo 48k) + mic (mono 48k), MKV like gamerec
Gen 'main_1440p60_h264.mkv' @(
    '-f', 'lavfi', '-i', "testsrc2=s=2560x1440:r=60:d=$mainDur",
    '-f', 'lavfi', '-i', "sine=f=440:sample_rate=48000:d=$mainDur",
    '-f', 'lavfi', '-i', "sine=f=660:sample_rate=48000:d=$mainDur",
    '-filter_complex', "[0:v]$(Counter 120),format=yuv420p[v];[1:a]pan=stereo|c0=c0|c1=c0,volume=0.25[g];[2:a]volume=0.2[m]",
    '-map', '[v]', '-map', '[g]', '-map', '[m]') + $h264 + @('-g', '250', '-bf', '0', '-c:a', 'aac', '-b:a', '160k',
    '-metadata:s:a:0', 'title=game', '-metadata:s:a:1', 'title=mic', '-color_range', 'tv', '-colorspace', 'bt709',
    '-color_primaries', 'bt709', '-color_trc', 'bt709')

# 2) ultrawide: 3440x1440@60 HEVC, GOP 250, B-frames (tests reordering)
Gen 'uw_3440x1440p60_hevc.mkv' @(
    '-f', 'lavfi', '-i', "testsrc2=s=3440x1440:r=60:d=$uwDur",
    '-f', 'lavfi', '-i', "sine=f=330:sample_rate=48000:d=$uwDur",
    '-filter_complex', "[0:v]$(Counter 120),format=yuv420p[v];[1:a]pan=stereo|c0=c0|c1=c0,volume=0.25[a]",
    '-map', '[v]', '-map', '[a]') + $hevc + @('-g', '250', '-bf', '2', '-c:a', 'aac', '-b:a', '160k')

# 2b) AV1 (what OBS records with AV1 on RTX 40/50): 1440p60, 20 s, keyframe every 2 s
Gen 'av1_1440p60.mkv' @(
    '-f', 'lavfi', '-i', 'testsrc2=s=2560x1440:r=60:d=20',
    '-filter_complex', "[0:v]$(Counter 120),format=yuv420p[v]", '-map', '[v]',
    '-c:v', 'libsvtav1', '-preset', '10', '-crf', '35', '-g', '120')

# 3) baseline 1080p30 H.264 short
Gen 'base_1080p30_h264.mp4' @(
    '-f', 'lavfi', '-i', 'testsrc2=s=1920x1080:r=30:d=20',
    '-f', 'lavfi', '-i', 'sine=f=523:sample_rate=48000:d=20',
    '-vf', "$(Counter 80),format=yuv420p", '-ac', '2') + $h264 + @('-g', '60', '-c:a', 'aac', '-movflags', '+faststart')

# 4) VFR: irregular timestamps (drops a varying subset of 60 fps frames); counter = original frame number
Gen 'vfr_1080p.mkv' @(
    '-f', 'lavfi', '-i', 'testsrc2=s=1920x1080:r=60:d=20',
    '-vf', "$(Counter 80),select='not(eq(mod(n\,7)\,3))*not(eq(mod(n\,11)\,5))*not(eq(mod(n\,13)\,1))',format=yuv420p",
    '-fps_mode', 'vfr') + $h264 + @('-g', '120')

# 5) multi-audio: game 48k stereo + mic 44.1k mono
Gen 'multiaudio.mkv' @(
    '-f', 'lavfi', '-i', 'testsrc2=s=1920x1080:r=60:d=15',
    '-f', 'lavfi', '-i', 'sine=f=440:sample_rate=48000:d=15',
    '-f', 'lavfi', '-i', 'sine=f=880:sample_rate=44100:d=15',
    '-filter_complex', "[0:v]$(Counter 80),format=yuv420p[v];[1:a]pan=stereo|c0=c0|c1=c0,volume=0.3[g];[2:a]volume=0.3[m]",
    '-map', '[v]', '-map', '[g]', '-map', '[m]') + $h264 + @('-g', '120', '-c:a', 'aac',
    '-metadata:s:a:0', 'title=game', '-metadata:s:a:1', 'title=mic')

# 6) A/V sync: white flash + 1 kHz beep of exactly one frame each second, 60 fps
Gen 'sync_1080p60.mp4' @(
    '-f', 'lavfi', '-i', 'color=c=black:s=1920x1080:r=60:d=30',
    '-f', 'lavfi', '-i', "aevalsrc='if(lt(mod(t\,1)\,1/60)\,0.8*sin(2*PI*1000*t)\,0)':s=48000:d=30",
    '-vf', "drawbox=enable='eq(mod(n\,60)\,0)':color=white:t=fill,$(Counter 80),format=yuv420p", '-ac', '2') + $h264 + @('-g', '60', '-c:a', 'aac', '-b:a', '192k')

# 7) colour: same patch chart encoded limited/full range and BT.601/BT.709
$chart = "color=c=black:s=1280x720:r=30:d=2,drawbox=x=0:y=0:w=320:h=360:color=white:t=fill,drawbox=x=320:y=0:w=320:h=360:color=0x808080:t=fill,drawbox=x=640:y=0:w=320:h=360:color=0xC03020:t=fill,drawbox=x=960:y=0:w=320:h=360:color=0x2060C0:t=fill,drawbox=x=0:y=360:w=640:h=360:color=0x20A040:t=fill"
Gen 'color_limited_709.mp4' @('-f', 'lavfi', '-i', $chart, '-vf', 'scale=out_color_matrix=bt709:out_range=tv,format=yuv420p',
    '-c:v', 'libx264', '-crf', '4', '-color_range', 'tv', '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709')
Gen 'color_full_709.mp4' @('-f', 'lavfi', '-i', $chart, '-vf', 'scale=out_color_matrix=bt709:out_range=pc,format=yuv420p',
    '-c:v', 'libx264', '-crf', '4', '-color_range', 'pc', '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709')
Gen 'color_limited_601.mp4' @('-f', 'lavfi', '-i', $chart, '-vf', 'scale=out_color_matrix=bt601:out_range=tv,format=yuv420p',
    '-c:v', 'libx264', '-crf', '4', '-color_range', 'tv', '-colorspace', 'smpte170m', '-color_primaries', 'smpte170m', '-color_trc', 'smpte170m')

# 8) 10-bit HEVC, PNG still, MP3 audio
Gen 'hevc10_1440p60.mkv' (@('-f', 'lavfi', '-i', 'testsrc2=s=2560x1440:r=60:d=10', '-vf', "$(Counter 120),format=p010le") + $hevc + @('-g', '120', '-profile:v', 'main10'))
Gen 'still.png' @('-f', 'lavfi', '-i', 'testsrc2=s=1920x1080:d=1', '-frames:v', '1')
Gen 'music.mp3' @('-f', 'lavfi', '-i', 'sine=f=392:sample_rate=44100:d=12', '-ac', '2', '-c:a', 'libmp3lame', '-b:a', '192k')
# "speech" with pauses for Remove Silence: tone 1 s, silence 1.5 s, tone 0.5 s, pause 0.2 s, tone 1 s
Gen 'pauses.wav' @('-f', 'lavfi', '-i', "aevalsrc='0.3*sin(2*PI*300*t)*(lt(t,1)+between(t,2.5,3)+gte(t,3.2))':s=48000:d=4.2", '-ac', '1', '-c:a', 'pcm_s16le')

# a 120 BPM kick pattern over a quiet pad, for Add Markers at Beats
Gen 'beats.wav' @('-f', 'lavfi', '-i', "aevalsrc='0.7*sin(2*PI*70*t)*exp(-25*mod(t,0.5))+0.03*sin(2*PI*330*t)':s=48000:d=4", '-ac', '2', '-c:a', 'pcm_s16le')
# three different scenes (2 s each) for scene change detection
Gen 'scenes.mp4' @('-f', 'lavfi', '-i', 'testsrc2=s=1280x720:r=30:d=2', '-f', 'lavfi', '-i', 'color=c=0x2050a0:s=1280x720:r=30:d=2', '-f', 'lavfi', '-i', 'mandelbrot=s=1280x720:r=30', '-filter_complex', '[2]trim=duration=2,setpts=PTS-STARTPTS[m];[0][1][m]concat=n=3:v=1:a=0,format=yuv420p', '-c:v', 'libx264', '-g', '60', '-bf', '2', '-preset', 'veryfast')

Write-Host '[testmedia] done' -ForegroundColor Green
