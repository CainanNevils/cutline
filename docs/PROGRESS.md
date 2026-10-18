# Progress

## Milestones
- [x] M0 Scaffolding
- [x] M1 Risk spikes
- [x] M2 Media engine
- [x] M3 Render engine
- [x] M4 Core model + commands + project IO
- [x] M5 UI shell
- [x] M6 Timeline + editing tools
- [x] M7 Audio engine
- [x] M8 Caches + proxies + background jobs
- [x] M9 Effects, speed, keyframes
- [x] M10 Titles, transitions, captions
- [x] M11 Export
- [x] M12 Performance + polish

## M0 Scaffolding
- `scripts/setup.ps1` detects VS Build Tools / CMake / Ninja / Python and installs Qt 6.8.3 + FFmpeg 8.1 into `deps/` (re-runnable, ~1 min on this machine).
- `scripts/build.ps1`, `scripts/test.ps1` (CMake presets `release`, `debug`, Ninja), `scripts/gen-testmedia.ps1`.
- Core library written early (model, edits, undo, project IO, SRT, LUT, LRU, evaluation) with 25 doctest cases / 397 assertions green.
- Empty Qt main window launches.

## M1 Risk spikes (measured on the dev PC: i7-12700K, RTX 5070, driver 617.42)
Tool: `build\release\bin\mediabench.exe` (one JSON line per run).

**Spike A, decode -> GPU -> present** (D3D11VA on the shared device, NV12 copied into our own pooled texture, YUV->RGB in the layer shader, flip-model swapchain):
| file | decoder | decode fps | x realtime | present loop (decode+render+present) |
|---|---|---|---|---|
| 2560x1440@60 H.264 (x264, GOP 250) | GPU | 1160 | 19.3x | 1230 fps, 0.81 ms avg, 1.10 ms p99 |
| 3440x1440@60 HEVC (NVENC, GOP 250, B-frames) | GPU | 861 | 14.3x | 924 fps, 1.08 ms avg, 1.43 ms p99 |
| 2560x1440@60 HEVC 10-bit (P010) | GPU | 1037 | 17.3x | - |
| 2560x1440@60 H.264 forced software | CPU (16 threads) | 748 | 12.5x | - |
VRAM per decoder incl. its surface pool and 16 cached frames: 230-330 MB at 1440p. Accept (>= 3x realtime): **met** (14-19x).

**Spike B, GPU -> NVENC** (test pattern rendered by the compositor -> RGB->NV12 shader -> encoder):
| path | 1080p60 | 1440p60 |
|---|---|---|
| h264_nvenc, D3D11 frames (zero copy) | 414 fps (6.9x) | 239 fps (4.0x) |
| h264_nvenc, readback to system memory | - | 234 fps (3.9x) |
| libx264 veryfast (fallback) | - | 236 fps (3.9x) on this simple pattern |
FFmpeg's NVENC wrapper accepts `AV_PIX_FMT_D3D11` frames from an `AVHWFramesContext` built on our device; our NV12 render target is `CopySubresourceRegion`'d into a pool surface. Accept (>= 2x realtime): **met**.

**Spike C, audio**: WASAPI shared, event-driven, MMCSS "Pro Audio" thread, 48 kHz float stereo, producer thread decoding AAC into a lock-free ring. 10-minute run on the S/PDIF output: **0 underruns, 0 device glitches, clock drift vs wall clock max 0.55 ms**. Device buffer 22.0 ms (the shared-mode default period is 10 ms; the endpoint reports a 2-period buffer). Target was < 20 ms: **missed by 2 ms**; the IAudioClient3 low-latency path is a possible later improvement, it does not affect sync.

## M2 Media engine
Probe (VFR detection from packet timestamps, multi-audio, images, HDR flags, colour tags), HW/SW decode with automatic fallback, PTS-accurate seek with back-off for sparse indexes, windowed frame cache with bounded reverse reading, sample-accurate audio reader (resample to 48 kHz stereo, mono centred at -3 dB), streaming atempo time-stretch.
- Frame-exact seek tests (`cutline_tests`): random seeks compared against a sequential reference decode by hashing the burned-in frame counter: CFR 1440p H.264, VFR 1080p, forced software decode, HEVC with B-frames, plus reverse frame stepping: **all exact**.
- Seek latency (`mediabench seek`, 60 random seeks, long-GOP 250 originals, no proxy): 1440p H.264 avg 114 ms, p95 228 ms, **max 247 ms** (budget 250 ms: met, barely); 3440x1440 HEVC avg 128 ms, **max 275 ms (over budget by 25 ms; proxies address this, M8)**. Re-showing a frame already in the decode window: ~1 us.

## M3 Render engine
One draw per layer (YUV->RGB with range+matrix, crop, colour correction, LUT, frame blend, opacity, affine transform, blend modes) on an RGBA8 (or RGBA16F "high quality") canvas, transition pass, present/overlay pass, RGB->NV12 pass, texture pool.
- `rendertest`: limited/full BT.709 and limited BT.601 charts decode within **1-3 levels** of the analytic values; NV12 round trip within 2 levels; 33 golden frames (transform, opacity, 3-layer stack, ultrawide fill, crop, colour, LUT, 8 transitions x 0/50/100 %, title, caption) - reviewed by eye and committed under `tests/golden`.
- 3-layer 2560x1440 composite with colour correction: **0.05 ms GPU** (budget 4 ms).

## M4 Core model + commands + project IO
Model (sequence/tracks/clips/effects/keyframes/transitions/markers/captions/assets/bins), integer
time, edit operations (split/razor/add edit, trim with media and neighbour limits, ripple trim,
ripple delete incl. gaps and multi-track sync limits, insert/overwrite, move/duplicate across
tracks, link/unlink, speed with ripple, frame hold, transitions, captions), snapshot-diff undo
(D3), versioned JSON `.cutproj` with migration hook, relative+absolute media paths, offline
detection + relink, autosave (60 s, 5 rotating files) and crash recovery.
- Unit tests: split/trim/ripple/insert/overwrite/move/speed/hold math, undo/redo round trip of
  every operation, a drag = one undo step, save -> load -> save **byte-identical**, newer-schema
  refusal, keyframe interpolation, SRT, LUT, LRU.
- `recoverytest`: edit -> autosave -> process killed -> next session offers the autosave and the
  recovered project contains the edit: **PASS**.

## M5 UI shell / M6 Timeline + editing tools
Qt Widgets, Fusion + dark palette, original SVG icon set, dockable Premiere "Editing" layout
(saved/restored, Window > Reset Workspace), Project panel (list/icon view with thumbnails, bins,
search, columns, drag in from Explorer, drag out to the timeline), Source/Program monitors on
native flip-model swapchains (zoom, preview quality Auto/Full/1/2/1/4, proxy toggle, safe
margins, mini scrub bar, timecode entry, transport), on-canvas move/scale/rotate handles, Type
tool, performance HUD (F3), tools strip, full menus with Premiere default shortcuts, status bar
showing decode/audio fallbacks.
Timeline: track headers (target, lock, eye/mute, solo), ruler with adaptive timecode, in/out,
markers, playhead scrubbing, snapping (edges, playhead, markers, in/out), thumbnails strips,
waveforms (progressive), fx/speed/hold/offline badges, disabled look, audio fade handles and
volume line (+keyframes), transitions with missing-handle warning, caption track, selection
(click/shift/ctrl/rubber band, linked), move/duplicate/insert-drag across tracks (creating tracks
past the last one), trim / ripple trim, razor (C, Ctrl+K, Ctrl+Shift+K), ripple delete (clips and
gaps), copy/paste/paste insert, enable/disable, link/unlink, insert/overwrite from the Source
monitor (buttons, `,`/`.`, or drag the In-Out range as video+audio / video-only / audio-only),
drag from the Project panel (Ctrl = insert), context menus, J/K/L shuttle.
- Timeline paint, 750 clips while zooming/panning (`uibench`, offscreen): **avg 3.8 ms, p99 5.8
  ms, max 6.1 ms** (budget: 60 fps and no frame > 33 ms).
- Program playback through the full engine (`mediabench playback`, 60 s, this PC):
  | sequence | fps | dropped | late | audio underruns | render ms | GPU ms |
  |---|---|---|---|---|---|---|
  | 2560x1440@60 H.264 + colour correction, 2 audio tracks | 60 | **0** | 0 | 0 | 0.11 | 0.08 |
  | 3440x1440@60 HEVC + colour correction, 2 audio tracks | 60 | **0** | 0 | 0 | 0.11 | 0.09 |
  | 3 stacked tracks (1440p + ultrawide PiP with colour + title) | 60 | **0** | 0 | 0 | 0.13 | 0.06 |
- Cold start to interactive (logged by the app): **~360 ms** (budget 2 s). Idle CPU while
  paused: **0.04-0.10 %** of the machine (budget 2 %). Working set after opening a project: 146 MB.

## M7 Audio engine
Mixer (clip gain + volume keyframes + fades + constant-power crossfades, per-track fader/pan/
mute/solo, master, peak meters with hold and clip latch), Audio Mixer panel, waveform peak cache,
WASAPI output with Qt fallback, audio-clock-mastered playback with underrun clock hold (D8).
- Meters: a -21.07 dBFS tone reads -21.07 +- 0.5 dB, a -6 dB fader reads 6 dB lower, mute is
  silent (unit test). Constant-power crossfade never dips (unit test).
- A/V sync: verified on exported files (e2e: beep onset vs flash frame, **worst 9.0 ms**, limit
  16.7 ms). Playback sync follows from the same clock design (video time = device-played audio
  position); there is no loopback capture here to measure the speakers directly.

## M8 Caches + proxies + background jobs
Disk cache keyed by path+size+mtime (thumbnail strips from keyframes only, waveform peaks with
mip levels, proxies), LRU purge to the cap (default 20 GB, at start-up and after each proxy),
job pool at below-normal priority, paused while playing/exporting, single proxy lane (NVENC
session limit: at most 1 proxy + 1 export; a second export is refused while one runs). Proxies:
720p all-intra H.264 via NVENC on its own GPU device, keeping source timestamps (VFR-safe);
built automatically above 1080p and on demand when reverse playback (J) needs them.
- Scrub/seek with proxies (100 random seeks): **avg 1.1-1.2 ms, p95 1.4 ms, max 26 ms** (budget
  80 ms). Originals (long GOP): see M2.
- Export never uses proxies: e2e gives every asset a deliberately wrong proxy file and the export
  still matches the goldens (**PASS**).
- Playback while jobs run: the pool pauses during playback (HUD/status shows "paused").

## M9 Effects, speed, keyframes
Motion (position/scale/uniform/rotation/anchor, fit/fill/native), opacity + blend modes, crop,
colour correction (exposure/contrast/highlights/shadows/saturation/temperature/tint), .cube LUT
(1D/3D, intensity), effect stack (reorder, enable, reset, remove), keyframes on every numeric
parameter (linear/hold/ease, stopwatch, add/remove, prev/next, draggable keyframe lane), speed
10-1000 %, reverse, frame hold / hold segment, frame blending, pitch-preserving audio speed,
reversed audio.
- Goldens for every effect (rendertest), keyframe interpolation unit tests, **reversed clip
  frame-exact** (reverse frame k == forward frame N-1-k, pixel-identical), colour + transform on
  3440x1440 at 60 fps with 0 dropped (M6 table) and 0.09 ms GPU per frame.

## M10 Titles, transitions, captions
Titles (Type tool / Ctrl+T; font, size, weight, italic, fill, stroke, shadow, background box,
alignment, line spacing, fades; rasterised only when text changes), 8 video transitions + constant
power audio crossfade (duration, alignment, missing-handle warning), captions track + panel
(edit, add/split/merge/delete, SRT/VTT import/export, global style), burn-in and sidecar .srt.
- rendertest: title (stroke/shadow/box) and caption goldens, every transition at 0/50/100 %,
  "caption ends on the right frame"; SRT round-trip unit test; e2e checks burned captions and the
  sidecar .srt.

## M11 Export
Export dialog (presets, size, fps, range, encoder, VBR/CQ, NVENC preset/B-frames/lookahead,
AAC 192-320, MP4/MKV, burn-in/sidecar captions, high-quality compositing), progress with fps,
x realtime, ETA, cancel, open folder, disk-space warning. Zero-copy NVENC path; system-memory NVENC
and libx264 fallbacks; audio mixed offline in parallel and throttled to the video for clean
interleaving.
- e2e (22 s sequence: razor, ripple delete, title, cross dissolve, colour correction, ultrawide
  fill, 50 % speed, freeze frame, SRT captions, fades, A/V sync clip), NVENC and libx264: exact
  1320 frames, 2560x1440, 60/1 fps, 48 kHz stereo AAC, 0 decode errors, A/V sync worst 9 ms,
  sampled frames match goldens: **31/31 PASS**.
- Full-project exports (`mediabench export`): 5 min 1440p60 + colour correction **3.87x realtime**;
  2 min 3-layer composite **3.86x realtime** (budget 2x). Memory flat: RSS trend -12.9 MB/min
  (no growth), peak private bytes 0.8-1.0 GB, VRAM peak 0.47-0.72 GB.

## M12 Performance + polish
- Crash handling: unhandled exceptions write a stack trace + minidump to
  `%LOCALAPPDATA%\Cutline` and point the user at autosave recovery; Qt warnings and UI-thread
  stalls > 70 ms are logged (watchdog thread). Observed: two ~110 ms stalls during start-up only.
- `scripts/bench.ps1` (repo or portable package): generates media, runs the whole budget suite,
  writes JSON + PASS/FAIL summary. `scripts/package.ps1`: portable zip with app-local Qt, FFmpeg and
  VC++ runtime.
- D3D debug layer: supported (`CUTLINE_DEBUG` builds request it) but the Windows "Graphics Tools"
  optional feature is not installed on this PC, so the debug device falls back silently. Not
  installed here because it changes system settings.

### Budgets (section 1) on this PC
| budget | measured | |
|---|---|---|
| 1440p60 playback, 1V+2A+colour: 60 fps, 0 dropped | 60 fps, 0 dropped (60 s) | met |
| 3440x1440p60: <1 % dropped | 0 dropped | met |
| 3 stacked tracks: 60 fps or auto 1/2 | 60 fps full res, 0 dropped | met |
| seek first frame < 250 ms (proxy < 80 ms) | 1440p max 247 ms; ultrawide HEVC max 275 ms; proxy max 26 ms | met with proxies; ultrawide originals 10 % over |
| timeline 500 clips 60 fps, no frame > 33 ms | 750 clips: max 6.1 ms | met |
| export 1440p60 NVENC with effects >= 2x | 3.87x | met |
| cold start < 2 s | ~0.36 s | met |
| idle CPU < 2 % | 0.04-0.10 % | met |
| < 2 GB RAM, VRAM cap 3 GB | playback/edit: 146 MB working set; export peak 1.0 GB private; VRAM <= 0.75 GB | met |
Headroom rule (>= 2x here): playback renders in ~0.1 ms of a 16.7 ms frame; decode runs at
14-19x realtime; export at 3.9x. The friend's RTX 3060 numbers come from `bench.ps1`.

### Cut / not done (v1)
- Automatic proxies for long-GOP media <= 1080p (proxies are created for > 1080p and on demand).
- Exact live A/V sync measurement during playback (no loopback here); covered by export test.

### Status 2026-10-17
- Portable zip built: `dist\Cutline-0.1.0-win64.zip`. Dependency check (`dumpbin /dependents`
  on every exe/dll): all non-system DLLs are in the package; the only external ones are Windows
  system DLLs (msimg32, usp10, avicap32, winhttp). Not yet run on a machine without dev tools.
- First 30-min soak crashed after ~11.5 min: a data race (play() reset viewer stats that the render
  thread was using). Fixed (render-thread-private stats published under the lock).
- **Soak re-runs (30 min, 3-layer 1440p project, playback loops + 20-seek bursts every 20 s):**
  run 1: 18 loops, 1680 seeks, 0 audio underruns, 6 dropped frames, no crash; run 2 (while
  builds ran in parallel): 19 loops, 1680 seeks, 0 underruns, 11 dropped. RSS sampled every
  5 min: 1073, 1077, 999, 948, 948, 940 MB (no growth); VRAM flat at 992 MB. No leaks.
- **bench.ps1 (2026-10-17 01:53): all 10 budgets PASS** - playback 1440p60 60.6 fps 0 dropped,
  ultrawide 0 % dropped, 3 layers full res 0 dropped, seek original avg 87 / p95 186 ms, proxy
  avg 4.2 / p95 27 ms, 750-clip timeline max 5.1 ms, export NVENC 3.80x, cold start 388 ms, idle
  CPU 0.06 %, peak private bytes 891 MB.
- **bench.ps1 (2026-10-17 07:52): all 12 checks PASS**, now including speed-ramp playback (many short
  clips: 0 dropped, 0 underruns, peak 434 MB) and nested-sequence playback (60 fps min).
- Audio latency target (< 20 ms, missed in M1 at 22 ms): **met** with the IAudioClient3
  low-latency shared mode (5.33 ms periods): 11.7 ms on the S/PDIF output. Devices that only
  offer 10 ms periods (e.g. some USB headsets) keep the 22 ms path.

### Additions after v1 (polish pass)
- Panels: Scopes (waveform, RGB parade, vectorscope, histogram; GPU read-back only while visible,
  async), History (jump to any undo step), Markers (rename, colours, jump).
- Effects: Flip, Black & White, Gaussian Blur (48-tap Vogel disk, ~0.7 ms at 1440p on the RTX
  5070), Sharpen, Vignette; audio clip effects High-Pass, Low-Pass, Parametric EQ, Compressor,
  Noise Gate (in the mixer, so preview = export); effect presets; Effects search.
- Editing: Q / W ripple trim, Close All Gaps, Paste Attributes, clip label colours, Premiere-style
  timecode entry (digits, +/- relative, click the timeline timecode), zoom scroll bar.
- Audio: device choice in Preferences, follows the Windows default device; master loudness
  (momentary / short-term / integrated); export loudness normalization (-14/-16/-23 LUFS, e2e
  verified to 0.0 LU).
- UI: empty-state hints (monitors, Project panel with recent projects), audio-only clips show a
  waveform in the Source monitor, customisable shortcuts (Edit > Keyboard Shortcuts), title style
  presets, proxy build progress.
- More: multiple sequences per project (tabs; undoable switching), nested sequences (Clip > Nest;
  nested 3-layer 1440p60 plays at 60 fps with 0 dropped frames and exports at 3.81x), Chroma Key, Shape Mask, Rounded
  Corners + Drop Shadow, Push / Cross Zoom / Iris / Blur Dissolve transitions, Ease In / Ease Out
  keyframes, track ducking, X / D shortcuts, YouTube chapters from markers.
- Robustness: 	ests/test_fuzz.cpp runs random edit sequences (25 operation kinds incl. sequences and
  track locks) with invariants after every step (no overlaps, frame alignment, sources inside media,
  valid transitions, locked tracks untouched) and whole-project undo/redo + save/load round trips;
  5000 seeds x 400 ops clean after fixing what it found (setSpeed with Alt-trimmed partners, ripple
  head trim, frame hold / link on locked tracks, a dangling pointer). uitest --monkey and
  --monkey-window drive random mouse/keyboard/menu input (the latter with playback, scrubbing and
  sequence switching): 60k+ actions clean.
- Playback of many short pieces: audio readers and video decoders are handed from a piece to the
  piece that continues it; ramp project went from 723 underruns / fps min 35 / 2.6 GB to 0 / 60 /
  434 MB. Readers survive between playback sessions.
- Record Voice-over (Sequence menu, Ctrl+Shift+R): WASAPI capture to a float WAV while the timeline
  plays, placed in sync from QPC capture timestamps vs the playback clock (median of samples). The
  uitest records the S/PDIF output via loopback (never the microphone) and checks the take against
  the source by envelope cross-correlation: 0 ms lag.
- Synchronize (by Audio): FFT cross-correlation of log-level envelopes (5 ms) over the whole clips,
  refined by direct correlation at 8 kHz on the loudest shared 6 s; sub-frame remainder goes into
  the in-points. uitest: synthetic second recording (quieter, inverted, noisy) lands within 0.03 ms.
- Create Vertical Version (9:16): one undo step, nested + Fill.
- Markers at Highlights (loud moments vs the half-minute median), Picture in Picture corners,
  Auto Color (grey world solved against the shader's WB maths + luma percentiles), Blur / Mosaic
  Region (masked adjustment layers now change only the masked area - before, the mask blacked
  out the rest), Shape Mask handles in the Program monitor, Mosaic effect.
- Export queue: background exports one after another while editing; the Export dialog follows its
  queue entry. Vertical sequences default to the vertical preset.
- UI polish from screenshot review: spin box arrows and checkboxes visible, marker labels on
  chips, Project names readable, tools dock title removed.
- Bench after these additions (2026-10-17 11:49): 12/12 PASS, unchanged (0 dropped frames,
  cold start 403 ms, NVENC export 3.8x).
- Typewriter titles, Instant Replay (fuzzed; the fuzzer caught duplicate clip ids in the first
  version - unique ids are now an invariant in both the core fuzzer and the UI monkey), Fit Music
  to Video End (looped with constant-power crossfades), Punch-In zoom from the Program monitor,
  Add Edits at Markers, title Templates (Lower Third, Subscribe, Countdown), Export Thumbnail,
  audio-only export (WAV / MP3 / FLAC / M4A), Help > User Guide (README embedded), Project panel
  hover-scrub - and a fix for clips whose icon thumbnail loaded behaving like sequences.
- Track Mask Forward (NCC region tracker; a Blur Region follows a moving title within 0.17 % of
  the width), Stabilize (translation; removes ~94 % of synthetic shake), Replace With Source
  Clip, Remove Unused Media, keyframe copy/paste, AV1 export preset; AV1 recordings decode on the
  GPU (2x faster than libdav1d, seeks 71 ms avg).
- Fixes found by new tests/fuzz ops: a frame-rate change left clips off-frame (Sequence Settings)
  or overlapping (Match Sequence) - now edit::changeFrameRate, fuzzed to random rates and back;
  replace-with-clip edge case on fast one-frame clips.
- Bench re-run 2026-10-17 17:13: 12/12 PASS, unchanged.
- Tests: 71 unit cases (fuzzer 5000 seeds x 34 op kinds clean), 74 render checks, 67 e2e checks
  (H.264 / HEVC / AV1 / x264 exports, audio-only formats), 99 UI checks + monkeys (all passing).
