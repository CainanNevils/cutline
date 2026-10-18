# Cutline

A fast, offline, GPU-accelerated non-linear video editor for Windows, built for editing
gameplay recordings (1440p / 3440x1440, 60 fps, multi-track audio). Premiere-style layout and
shortcuts, nothing more than needed, and no network access at all.

- Decoding on the GPU (D3D11VA / NVDEC) - H.264, HEVC and AV1 recordings - with automatic
  software fallback
- Compositing, effects, transitions and titles in Direct3D 11 shaders
- Audio-clock-mastered playback (WASAPI), video drops frames rather than glitching audio
- Export with NVENC (H.264 / HEVC / AV1) from GPU frames (zero copy), libx264 fallback; audio-only
  WAV / MP3 / FLAC
- Proxies (720p all-intra) for instant scrubbing of long-GOP recordings
- Unlimited undo with a History panel, autosave every minute with crash recovery
- Video scopes (waveform, RGB parade, vectorscope, histogram), effect presets, clip labels
- Loudness-normalized export (-14 / -16 / -23 LUFS) and a low-latency WASAPI path

## Getting started

1. Download the latest release zip from the
   [Releases page](https://github.com/CainanNevils/cutline/releases/latest), unzip it and
   double-click `Install.cmd` (Windows may say "Windows protected your PC" because the app isn't
   code-signed: More info > Run anyway). Or build it yourself (see below) and run
   `build\release\bin\cutline.exe`.
2. **Import** recordings: `Ctrl+I`, the Import button in the Project panel, or drag files from
   Explorer onto the Project panel. Multi-track recordings (game + mic) become one linked video
   clip plus one audio clip per track, each on its own audio track.
3. Double-click a clip to open it in the **Source monitor**, set **In** (`I`) and **Out** (`O`),
   then **Insert** (`,`) or **Overwrite** (`.`) at the playhead - or just drag clips from the
   Project panel onto the timeline (`Ctrl` while dropping = insert).
4. Edit in the **Timeline** (see shortcuts), adjust clips in **Effect Controls**, mix in the
   **Audio Mixer**, add **Captions**.
5. **Export** with `Ctrl+M`. Exports always use the original media, never proxies. **Add to Queue**
   exports in the background while you keep editing (Window > Export Queue shows progress; queued
   exports run one after another and keep the sequence as it was when queued) - e.g. the YouTube
   master and its vertical version in one go. The **Audio only** presets write the mix as WAV, MP3
   (320 kbps) or FLAC - no picture, no GPU - for podcasts or sharing a voice track.
   Video exports use the GPU encoder: H.264, HEVC or AV1 (the AV1 preset gives YouTube the same
   quality in smaller uploads); libx264 on the CPU is the fallback.

This guide is also inside the app: **Help > User Guide** (`F1`), with a find box.

**Replace a take**: open the new recording in the Source monitor (set its In point), select the clips
on the timeline and choose Clip > Replace With Source Clip - same place and length, effects and
keyframes kept, linked audio swapped too.

### Ultrawide to 16:9
Put a 3440x1440 clip in a 2560x1440 sequence, right-click it > **Scale to Fill**, then in Effect
Controls animate **Position X** (stopwatch, move the playhead, change the value) to pan across.

### Sequences
A project can hold several sequences (e.g. the full video and a vertical Shorts cut): the tabs above
the timeline switch between them, + or Ctrl+N adds one (with its own size and frame rate), and
right-click duplicates or deletes. Switching is part of the undo history. Sequences are also listed in
the Project panel: double-click opens one, dragging one onto the timeline nests it there.

**Nesting**: select clips and choose Clip > Nest (Ctrl+Alt+G). They move into a new sequence and a
single teal nested clip (plus a linked audio clip with the nest's mix) takes their place - move,
scale or add effects to the whole group at once. Double-click a nest to edit inside it; Match Frame
(F) looks through nests to the original media.

### Panels
- **Project**: list or icon view; in icon view, move the mouse across a clip to scrub its
  thumbnail - double-click opens it in Source at that moment. File > Remove Unused Media clears out clips
  no sequence uses (files stay on disk; Undo brings them back).
- **Scopes** (tab next to Source): luma waveform, RGB parade, vectorscope (75 % targets and a
  skin-tone line) and histogram of the Program frame. Reading back the frame costs nothing while
  the tab is hidden.
- **Markers**: every sequence marker with timecode; click a name to rename it, right-click for
  colours, double-click to jump.
- **History**: every edit; click a step to go back (or forward) to it.
- **Export Queue**: queued and finished exports with progress; cancel, remove, show in folder.
- **Effects**: search box; **Looks** has one-click colour grades (Punchy, Vivid Gaming, Warm Sunset,
  Cool Blue, Faded Film, Night, Bleach Bypass) as editable Color Correction settings; **Presets** holds effects saved from Effect Controls (bookmark button on
  any effect header) - drag a preset onto a clip or double-click it to apply to the selection.

### Graphics
Graphics > New Text Layer (Ctrl+T), New Color Matte (a solid colour - crop it and add Rounded Corners +
Drop Shadow for a lower-third bar) and New Adjustment Layer (its effects, motion and opacity apply to
everything below it: one colour grade for the whole edit).

**Templates**: Graphics > Templates adds a Lower Third (name + @channel on a dark box, slides in),
a Subscribe Reminder (red badge bottom right, pops in) or a 3-2-1 Countdown at the playhead, on a
free track above everything - ordinary titles and keyframes, so text, colours and timing stay
editable.

**Animate**: Clip > Animate (or right-click a clip) adds Fade In / Out, Slide Up In, Slide In from Left /
Right, Pop In, Slow Zoom (Ken Burns) or Slide Down Out as ordinary keyframes you can adjust afterwards.
On titles, **Typewriter** reveals the text character by character (about 30 a second) through the
Typewriter effect's Revealed % keyframes - the layout never moves while it types.

### Keyframes
Every numeric parameter has a stopwatch (animate), keyframe buttons and a lane; right-click a keyframe for
Linear / Ease In / Ease Out / Ease In-Out / Hold. Double-click a lane for its value graph: drag keyframes in
time and value (Shift: value only). Right-click a lane > Copy Keyframes, then on any parameter of any
clip > Paste Keyframes at Playhead to reuse an animation.

### Effects
Video: Motion, Opacity (blend modes), Crop, Flip, Color Correction, LUT (.cube), Black & White,
Gaussian Blur, Sharpen, Vignette, Mosaic, Glow, Rounded Corners (with border), Drop Shadow, Chroma Key
(green / blue screen with spill suppression) and Shape Mask (ellipse / rectangle, feather, invert) - a
keyed or rounded facecam picture-in-picture takes a few clicks. Audio (per clip): DeNoise (removes steady hiss / fan noise, no noise print needed),
High-Pass, Low-Pass, Parametric EQ, Compressor, Noise Gate. Transitions: Cross Dissolve, Dip to Black/White, four Wipes, Slide,
Push, Cross Zoom, Iris Round, Blur Dissolve; Constant Power audio crossfade. All numeric parameters
can be keyframed.

### Proxies
Media above 1080p gets a 720p all-intra proxy in the background (Project panel, "Proxy" column).
Toggle **Proxies** in the Program monitor's toolbar for instant scrubbing and smooth reverse
playback (`J`). Proxies are preview-only; export ignores them.

## Keyboard shortcuts (Premiere defaults)

| Key | Action | Key | Action |
|---|---|---|---|
| Space | Play / stop | J / K / L | Shuttle reverse / stop / forward (press again: 2x, 4x, 8x) |
| Left / Right | Step 1 frame | Shift+Left / Right | Step 5 frames |
| Up / Down | Previous / next edit point | Home / End | Start / end |
| I / O | Mark In / Out | Shift+I / Shift+O | Go to In / Out |
| , / . | Insert / Overwrite from Source | M | Add marker |
| V C B T H Z R | Selection, Razor, Ripple Edit, Type, Hand, Zoom, Rate Stretch tools | S | Snapping on/off |
| Ctrl+K | Add edit at playhead (selected or all clips) | Ctrl+Shift+K | Add edit to all tracks |
| Delete | Clear (leave gap) | Shift+Delete | Ripple delete (also on a selected gap) |
| Ctrl+L | Link / unlink | Shift+E | Enable / disable |
| Ctrl+R | Speed / duration | Ctrl+D / Ctrl+Shift+D | Default video / audio transition |
| Ctrl+C / X / V | Copy / cut / paste | Ctrl+Shift+V | Paste insert |
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo | Ctrl+S | Save |
| Ctrl+I | Import | Ctrl+M | Export |
| = / - / \ | Zoom in / out / fit timeline | Alt+wheel | Zoom timeline at the mouse |
| Ctrl+T | New title | F3 | Performance HUD |
| Q / W | Ripple trim previous / next edit to the playhead | ; / ' | Lift / Extract In-Out |
| F | Match frame | Ctrl+Alt+V | Paste attributes |
| G | Audio gain | Ctrl+Shift+E | Export frame (PNG) |
| X | Mark clip under the playhead | D | Select clip under the playhead |
| Ctrl+N | New sequence | Ctrl+Alt+G | Nest selection |
| Ctrl+Shift+R | Record voice-over (again / Space stops) | F1 | User Guide |

Mouse: drag clips to move (Alt-drag duplicates, Ctrl-drop inserts), drag clip edges to trim
(Ripple tool or Ctrl = ripple trim, Alt = ignore links), drag the white squares on audio clips for
fades, drag the yellow line for clip volume (Ctrl+click adds a volume keyframe). In the Program
monitor, drag the selected clip to move it, drag a corner to scale, drag just outside a corner to
rotate (Shift snaps); on a Blur / Mosaic Region (or with Ctrl on any clip with a Shape Mask) the
drag moves and sizes the mask instead. Drag the ends of the timeline's scroll bar to zoom; click the timecode in the
timeline's corner to type a time ("500" = 5 s, "+30" = 30 frames later). Help > Keyboard
Shortcuts lists everything, and Edit > Keyboard Shortcuts changes them.

### Audio
File > Preferences > Audio output picks the device (default: follow Windows, including when
headphones are plugged in). Devices whose mix format is 48 kHz float stereo use the
low-latency shared mode (about 12 ms instead of 22 ms). The export dialog can normalize the
mix to a loudness target with one measured gain (peaks stay under -1 dBFS). The Audio Mixer shows
the master's short-term loudness (green near -14 LUFS), and any track can **duck** under another
(e.g. music under the microphone track) with a chosen amount.

### Stabilize
Clip > Stabilize steadies a shaky facecam or phone shot: it follows the middle of the picture
through the clip, smooths the path (0.6 s) and counters the shake with Position keyframes, zooming
in just enough (at most 25 %) to keep the moving edges out of frame. Translation only - rotation
and rolling shutter stay.

### Punch-in zoom
Right-click the spot in the Program monitor (the kill, the scoreboard) > Punch In Here (1.5x or
2x): from the playhead the clip zooms toward that spot over 0.3 s - the spot stays where it is on
screen - holds for 2 s and eases back. The keyframes are ordinary Motion keys.

### Instant Replay
Put the playhead right after the moment and choose Sequence > Insert Instant Replay > Last 5 s at
50 % (or 3 s, 10 s, 25 %): everything on the unlocked tracks from those seconds plays again in slow
motion right after it (frame-blended, audio pitch kept), the rest of the edit moves along, and a
red REPLAY title pops in over it. One undo step; speed, title and timing stay editable.

### Speed Ramp
Put the playhead on the moment, then Clip > Speed Ramp...: the footage around it slows to the chosen speed
(e.g. 25 %) and eases back to 100 %, built from short frame-blended segments (each still adjustable)
with the rest of the timeline rippled.

### Voice and music helpers
- Clip > Enhance Voice: adds High-Pass (80 Hz), DeNoise, a presence EQ and a Compressor to the selected
  microphone clips - one undo step, every setting adjustable afterwards.
- Clip > Synchronize (by Audio): select the gameplay capture and a separately recorded mic (or camera)
  clip; the earliest one stays and the others move so their sound lines up - found by
  cross-correlating the recordings over their whole length, then sharpened to a fraction of a
  millisecond (the part smaller than a frame goes into the source in-point).
- Clip > Audio > Fit Music to Video End: the selected music clip ends with the last video frame -
  cut and faded out over 3 s when it is longer, looped with constant-power crossfades when it is
  shorter (stopping at the next clip on its track).
- Markers > Add Markers at Highlights: red markers where the selected gameplay clip gets much
  louder than the half minute around it (fights, explosions, shouting) - about one per minute,
  the strongest first. A quick way to find the moments worth keeping in a long recording.
- Markers > Add Markers at Beats: yellow markers on the beats of the selected music clip; cuts and
  the playhead snap to them, Shift+M jumps from beat to beat.
  Sequence > Add Edits at Markers then cuts every unlocked track at each marker (inside In/Out
  when set) - a montage cut to the music in one step.

### Record Voice-over
Sequence > Record Voice-over (Ctrl+Shift+R) plays the timeline from the playhead and records the
microphone chosen in Preferences (Voice-over input). Press Space (or the shortcut again) to stop: the
take is saved as a WAV next to the project (Voice-over folder) and placed on the target audio track -
or the next free one - lined up with the picture using the audio device clocks (measured in sync to
the millisecond). The status bar shows the take length and the input level while recording.

### Auto Color
Clip > Auto Color looks at the selected clip under the playhead (on its own, without its current
correction) and sets Color Correction: white balance from the near-neutral midtones (solved
against the effect's own temperature/tint maths, at 75 % so a green forest stays green),
exposure to use the tonal range and a little contrast for flat footage. Every value stays
adjustable in Effect Controls.

### Blur / Mosaic Region
Graphics > New Blur Region (or New Mosaic Region) adds an adjustment layer that blurs (or
pixelates) a rectangle - Shape Mask + Gaussian Blur / Mosaic - above everything at the playhead. Drag it in the Program monitor to move it, drag a
corner to size it (with keyframes on, each drag sets a key to follow something), and stretch the
layer over the part to hide. Any adjustment layer with a Shape Mask or Crop changes only that
area; Ctrl+drag in the Program monitor edits the Shape Mask of any other clip the same way.

**Track Mask Forward** (Clip menu, `Ctrl+Alt+T`, or right-click in the Program monitor): put the playhead
where the region covers the thing to hide and run it - the region follows it frame by frame
(Center X / Y keyframes) until the clip ends or the thing disappears; move the mask and track again
from there.

### Picture in Picture (facecam)
Clip > Picture in Picture > Top Left / Top Right / Bottom Left / Bottom Right shrinks the selected
clip to 30 % of the frame width, insets it from the corner and adds Rounded Corners and a Drop
Shadow (all adjustable in Effect Controls). Full Frame puts it back.

### Vertical version
Sequence > Create Vertical Version (9:16) makes a 1080x1920 sequence showing the current one (nested,
scaled to fill and centred) for Shorts / TikTok / Reels. Then select the clip in it and choose
Clip > Auto Reframe: the 9:16 window slides (eased Position X keyframes) to wherever the most motion
is, never past the picture's edges - adjust any keyframe by hand. Edits to the original show up in
the vertical version.

### Scene changes
Markers > Add Markers at Scene Changes (or Clip > Cut at Scene Changes) analyses the selected video clip in
the background (about 8x realtime for 1440p60) and marks or cuts every abrupt picture change - deaths,
menus, map loads in a long recording.

### Remove Silence
Select a microphone clip (or a clip with linked mic audio) and choose Clip > Remove Silence...: pauses
quieter than the threshold for longer than the minimum are cut from every track at once (keeping a
little padding around speech), as one undo step. The dialog shows how much will go before you
confirm.

### YouTube thumbnail
File > Export Thumbnail saves the frame under the playhead as a 1280x720 JPEG (filled and centred,
quality lowered only if needed to stay under YouTube's 2 MB) - put a big title over the moment
first, export, then delete the title.

### YouTube chapters
Markers > Copy Markers as YouTube Chapters puts "0:00 Intro", "1:15 Boss fight", ... on the
clipboard for the video description (unnamed markers become "Chapter N").

## Performance HUD (F3)
Shows playback fps, dropped/late frames, render and GPU time, preview resolution (Auto steps down
if rendering cannot keep up), how many decoders run on the GPU vs the CPU fallback, VRAM, audio
backend/latency/underruns and background jobs.

## Moving a project to another PC
File > Collect Files... copies the project and the media it uses (or all imported media) into one
folder (Media\ next to the .cutproj). Open the copied project there; proxies and thumbnails are
rebuilt as needed.

## Files and folders
- Projects: `.cutproj` (JSON). Media paths are stored absolute and relative to the project, so
  moving a project together with its media keeps it linked. Missing media shows as offline;
  double-click it (or right-click > Relink Media) to point at the new location.
- `%LOCALAPPDATA%\Cutline\cache`: proxies, thumbnails, waveforms (default limit 20 GB,
  least-recently-used purge; folder and limit in File > Preferences).
- `%LOCALAPPDATA%\Cutline\autosave`: rotating autosaves (5). After a crash Cutline offers to recover.
- `%LOCALAPPDATA%\Cutline\cutline.log`: log, including crash reports.

## Benchmark a PC
`powershell -ExecutionPolicy Bypass -File bench.ps1` (portable build) or
`scripts\bench.ps1` (source tree) generates synthetic test videos and measures playback, seeking,
timeline drawing, export, start-up and idle CPU against the budgets in `PLAN.md`. It writes
`bench-results-<date>.json` and `.txt`. Add `-Full` for 5-minute playback runs.

## Releases
Each version is published on the Releases page with an installer zip, its SHA-256 checksum and
the source of the FFmpeg it bundles; this repository's history holds one commit per release.
Maintainers publish with `scripts\release.ps1` (tests, package, personal-information scan, then
upload): see `docs/RELEASING.md`.

## Building from source
Requirements: Windows 10/11 x64, Visual Studio 2022+ Build Tools (C++ workload), Python 3.

```
powershell -ExecutionPolicy Bypass -File scripts\setup.ps1      # Qt 6.8 + FFmpeg 8.1 into deps\
powershell -ExecutionPolicy Bypass -File scripts\build.ps1      # build\release\bin\cutline.exe
powershell -ExecutionPolicy Bypass -File scripts\gen-testmedia.ps1
powershell -ExecutionPolicy Bypass -File scripts\test.ps1       # unit, render, end-to-end tests
powershell -ExecutionPolicy Bypass -File scripts\package.ps1    # dist\Cutline-<ver>-win64.zip
```

Layout: `src/core` (model, edits, undo, project IO - no Qt/FFmpeg), `src/media` (probe, decode),
`src/render` (D3D11 compositor, shaders), `src/audio` (mixer, WASAPI), `src/engine` (playback,
caches, jobs), `src/export` (encoders, export, proxies), `src/ui` (Qt Widgets), `tools/`
(mediabench, rendertest, e2e, uibench, recoverytest), `tests/`, `docs/PROGRESS.md` (measured
numbers per milestone), `docs/DECISIONS.md`.

## License
Cutline is free software under the **GNU General Public License v3** or (at your option) any later
version: see `LICENSE`. You may use, study, share and change it; if you distribute changed versions,
they must be under the GPL too, with their source.

It ships FFmpeg (a GPL v3 build including x264) and Qt 6 (LGPL v3, dynamically linked); details,
licenses and where their source code is are in `THIRD_PARTY_NOTICES.txt`. Icons and artwork are
original.
