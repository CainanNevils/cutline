# Decisions

Short records of choices made while building Cutline (newest last).

## D1. Toolchain and dependencies (M0)
- **Compiler:** the installed *Visual Studio Build Tools 2026* (MSVC 14.50) is used instead of installing VS 2022; it is ABI-compatible with Qt's `msvc2022_64` binaries. `setup.ps1` installs VS 2022 Build Tools only when no C++ toolset exists.
- **CMake / Ninja:** the copies bundled with the VS C++ workload (CMake 4.x).
- **Qt 6.8.3** (latest open-source 6.8 LTS patch in aqtinstall) into `deps/qt`, with `qtmultimedia` (QAudioSink fallback) and `qtsvg` (icons).
- **FFmpeg 8.1 GPL shared** from BtbN into `deps/ffmpeg` (has d3d11va, h264/hevc/av1_nvenc, libx264, avfilter). The sibling `gamerec` project builds a minimal FFmpeg (avformat only, no x264/avfilter) through vcpkg, so it is not reusable here; it was not modified.
- Header-only `nlohmann/json` 3.12 and `doctest` 2.4.12 are vendored in `third_party/` (committed) so builds work offline.

## D2. Time base
Integer ticks of 1/35,280,000 s everywhere in the model (unit-tested to be an exact multiple of 23.976..60 fps frame durations and 44.1/48 kHz sample periods). FFmpeg timestamps are converted at the boundary with 128-bit mul/div.

## D3. Undo = snapshot diffs, not hand-written inverse commands
Every edit runs inside a `Document` transaction (`begin` -> mutate -> `commit`). Commit diffs the project against the snapshot: changed clips (by id) plus the small non-clip header (tracks, transitions, captions, markers, assets) only when it changed. This gives command-pattern undo/redo for *every* operation with no per-command inverse code to get wrong, and a whole mouse drag naturally becomes one undo step. History is unlimited in steps, capped at ~512 MB.

## D4. Frame lookup tolerance
"Nearest earlier source frame" is evaluated with a tolerance of half a frame (min of sequence and source frame durations). MKV stores millisecond timestamps, so frame 1 of a 60 fps file is at 17 ms while timeline frame 1 starts at 16.67 ms; a strict `pts <= t` lookup would show frame 0 twice. The tolerance absorbs timestamp rounding without ever skipping a real frame.

## D5. Positions are offsets from centre
Motion position/anchor are stored as pixel offsets from the frame/source centre (default 0,0) and displayed as absolute coordinates in Effect Controls. Clips stay centred if the sequence size changes.

## D6. One D3D11 device per render owner
Preview, export and proxy generation each create their own `GpuDevice` (device + immediate context). The rule "one owner for the immediate context" then holds per device without cross-thread locking, and an export never stalls the preview. Decoder threads share their owner's device: FFmpeg's D3D11VA decode calls and our `CopySubresourceRegion` out of the decoder pool run on decoder threads under D3D11 multithread protection; they never touch pipeline state.

## D7. Per-clip video sources
Preview opens one decoder thread per (viewer, clip) rather than per file, and prefetches clips that start within 1.5 s. Playing across a cut therefore never waits for a seek in a long-GOP file. Idle sources close after 4 s.

## D8. Audio is pulled, never pushed
A mixer thread renders 10 ms blocks into a lock-free SPSC ring (~80 ms ahead); the WASAPI thread only copies from the ring. The clock is the device's played position mapped to the session's first sample. If the ring underruns, the clock is held for the missing samples, so video waits for audio instead of drifting.

## D9. Time stretch streams through `atempo`
Speed-changed audio is produced on the fly by an FFmpeg `atempo` chain (factors outside 0.5-2.0 chained), re-primed on discontinuities, instead of being pre-rendered into the cache. Cheap enough for real time and no disk churn.

## D10. Effects order
The per-layer shader applies crop -> (LUT/colour correction in stack order) -> transform/opacity. One instance per effect type per clip.

## D11. Text is rasterised on the CPU, animated on the GPU
Titles and captions are drawn with QPainter into a tightly bounded premultiplied image (stroke,
blurred shadow, background box) and cached by a hash of text+style; position/scale/rotation/
opacity/fades are applied by the layer shader. Re-rasterising happens only when text or style
changes.

## D12. One undoable step per user action, structure-aware repaint
Panels rebuild their lists only when tracks/assets/bins actually change (a cheap key comparison
before/after each edit); clip-level edits just repaint. Drags reuse the start state and re-apply
the whole gesture on every mouse move (D3), publishing a fresh render snapshot so the Program
monitor previews the edit live.

## D13. Source monitor = a synthetic one-clip sequence
The Source monitor plays a generated sequence (asset size/fps, one video clip + one audio clip per
stream) through exactly the same engine as the Program monitor, so there is a single playback
path to optimise and test.

## D14. Proxies keep the original timestamps
Proxy frames are written with the source media time (90 kHz time base), not renumbered, so a
proxy lookup uses the same source time as the original - VFR recordings stay frame-accurate.

## D15. Export runs on its own device and threads
ExportJob creates a private GpuDevice, decoders (originals only) and encoders; the UI keeps
editing/playing. Audio is mixed offline in parallel but kept within ~2 s of the video so the
muxer interleaves without large buffers. N timeline frames -> exactly N encoded frames.

## D16. Test isolation
`CUTLINE_TEST=1` keeps tools/tests from touching the app's session lock and autosaves;
`CUTLINE_APPDATA` redirects logs/autosave/cache; `CUTLINE_AUDIO_DEVICE` pins playback to a given
WASAPI endpoint so automated runs never play test tones on the user's speakers.

## D17. Multiple sequences: the open one stays `Project::sequence`
All editing, rendering and undo code works on `Project::sequence`; the other sequences live in
`Project::otherSequences` (tab order recorded by `activeSequence`). Switching swaps them inside an
ordinary undo transaction, so history stays linear and exact (undoing past a switch switches
back) and nothing else had to learn about multiple sequences. Sequences have stable `uid`s.

## D18. Nesting renders the inner sequence into a canvas
A nested clip's sequence is rendered (same FrameRenderer, recursion depth <= 3, per-depth scratch
textures) into an offscreen canvas at the target's scale; that canvas is then an ordinary layer,
so motion, opacity and every effect apply to the whole nest. Clip ids are only unique per
sequence, so providers get `nestedClipKey(nest, inner)`. Audio: a sub-mixer renders the nest.
Effects measured in source pixels use the layer's logical size (`LayerParams::logicalW/H`).

## D19. Speed ramps are segments, not a time-remap curve
A ramp is built from short constant-speed, frame-blended pieces (razor + setSpeed), all of which
are fuzzed primitives; no new time model in the decoder, mixer or trimming code. To keep that
cheap at playback, a piece that continues the previous one on the same media inherits its audio
reader and video decoder (no reopen, no seek), and each source caches only what its clip shows.

## D20. Randomized testing guards the edit model
`tests/test_fuzz.cpp` (core edits, sequences, nesting, locks: invariants after every step,
whole-project undo/redo and save/load) and `uitest --monkey` / `--monkey-window` (real widgets,
real engine) run in CI-sized form on every test run and in long campaigns by hand. Several real
bugs (setSpeed with Alt-trimmed partners, ripple head trim, edits on locked tracks) were found
this way and are covered now.
