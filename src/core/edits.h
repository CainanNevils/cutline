#pragma once
// Timeline edit operations. Pure functions on the model; the undo system diffs before/after.
// All times passed in are expected to be frame-aligned (callers snap with Sequence::frameDur).
#include "core/model.h"

#include <set>
#include <vector>

namespace cut::edit {

// Media length lookup for trim limits (0 = unlimited, e.g. stills and titles).
Tick mediaLimit(const std::vector<Asset>& assets, const Clip& c);

// Sort/normalize after every edit: drops invalid transitions, empty clips, sorts by id.
void normalize(Sequence& s);

// --- basic clip ops ---------------------------------------------------------------------------
// Splits clip at t (start < t < end). Returns the id of the new right-hand part (0 if no split).
Id splitClip(Sequence& s, Id clipId, Tick t, Id newLinkId = 0);
// Razor: split clip (and linked partners unless `alone`) at t. Returns the right-hand ids.
std::vector<Id> razor(Sequence& s, Id clipId, Tick t, bool alone = false);
// Add Edit (Ctrl+K): split the given clips at t, or every clip under t on unlocked tracks if empty.
void addEdit(Sequence& s, Tick t, const std::vector<Id>& onlyClips);
// Removes media in [a,b) on a track (trim / split / delete). Clips in `except` are untouched.
void clearRange(Sequence& s, Id trackId, Tick a, Tick b, const std::set<Id>& except = {});
void deleteClips(Sequence& s, const std::vector<Id>& ids);
// Delete and close the gap on all unlocked tracks (as far as every track allows).
void rippleDelete(Sequence& s, const std::vector<Id>& ids);
// Close the empty gap at t on track. Returns false if t is not in a gap.
bool rippleDeleteGap(Sequence& s, Id trackId, Tick t);
// Gap [a,b) at t on track (b = next clip start). Returns false if t is inside a clip or after all.
bool gapAt(const Sequence& s, Id trackId, Tick t, Tick& a, Tick& b);

// Shift clips/captions with start >= from on unlocked tracks left by up to `amount`, limited so
// nothing overlaps. Returns the amount actually shifted.
Tick rippleShiftLeft(Sequence& s, Tick from, Tick amount, const std::set<Id>& onlyTracks = {});
// Split everything crossing `from` on the tracks and push clips at/after it right by `amount`.
void insertSpace(Sequence& s, Tick from, Tick amount, const std::set<Id>& onlyTracks = {});

// --- moving -----------------------------------------------------------------------------------
struct MoveResult {
    bool ok = false;
    std::vector<Id> moved;
};
// Moves clips by dt; video clips shift dTrackV tracks, audio clips dTrackA (positive = up for
// video/down for audio, i.e. higher index). Overwrite semantics, or insert (ripple) if `insert`.
MoveResult moveClips(Sequence& s, const std::vector<Id>& ids, Tick dt, int dTrackV, int dTrackA,
                     bool insert = false);
// Duplicates clips (Alt-drag) then moves the copies.
MoveResult duplicateClips(Sequence& s, const std::vector<Id>& ids, Tick dt, int dTrackV, int dTrackA);

// --- trimming ---------------------------------------------------------------------------------
enum class Edge { Head, Tail };
// Valid range for the edge position of a clip (accounts for media, neighbours, min 1 frame).
TickRange trimRange(const Sequence& s, const std::vector<Asset>& assets, const Clip& c, Edge e,
                    bool ripple);
// Trim an edge to absolute timeline position `pos` (clamped). Linked partners follow unless alone.
void trimClip(Sequence& s, const std::vector<Asset>& assets, Id clipId, Edge e, Tick pos, bool ripple,
              bool alone = false);

// --- placing new media ------------------------------------------------------------------------
// Overwrite: clears the destination ranges then adds clips. Assigns ids. Returns new ids.
std::vector<Id> overwriteClips(Sequence& s, std::vector<Clip> clips);
// Insert at `at`: pushes everything on unlocked tracks right by the clips' span, then adds.
std::vector<Id> insertClips(Sequence& s, std::vector<Clip> clips, Tick at);
// Ensures there are at least n tracks of a kind; returns the track ids in order.
std::vector<Id> ensureTracks(Sequence& s, TrackKind k, int n);
Id addTrack(Sequence& s, TrackKind k);
bool deleteTrack(Sequence& s, Id trackId);  // fails for the last track of a kind

// Builds the linked clip set for an asset (video + one audio clip per stream).
// vTrack: index of target video track, aTrack: first audio track index. Tracks are created.
std::vector<Clip> clipsForAsset(Sequence& s, const Asset& a, Tick at, Tick srcIn, Tick srcOut,
                                int vTrack, int aTrack, bool video = true, bool audio = true);

// --- properties -------------------------------------------------------------------------------
void linkClips(Sequence& s, const std::vector<Id>& ids);
void unlinkClips(Sequence& s, const std::vector<Id>& ids);
void setEnabled(Sequence& s, const std::vector<Id>& ids, bool enabled);
// Speed in (0, 10], reverse; duration scales. With ripple, later clips shift; otherwise the new
// duration is clamped to the free space after the clip.
void setSpeed(Sequence& s, const std::vector<Asset>& assets, Id clipId, double speed, bool reverse,
              bool ripple, bool frameBlend);
// Splits at t and turns the right part into a hold of the frame at t (to the clip end).
Id addFrameHold(Sequence& s, Id clipId, Tick t);
// Inserts a hold segment of `len` at t (ripples later material).
Id insertFrameHoldSegment(Sequence& s, Id clipId, Tick t, Tick len);

// --- transitions --------------------------------------------------------------------------------
// Adds (or replaces) a transition at a cut / clip edge. Returns its id or 0 if invalid.
Id addTransition(Sequence& s, Id trackId, Id clipA, Id clipB, TransitionType type, Tick duration,
                 TransitionAlign align);
void removeTransition(Sequence& s, Id id);
// True when the transition needs source frames beyond a clip's in/out that do not exist.
bool transitionMissingHandles(const Sequence& s, const std::vector<Asset>& assets, const Transition& t);

// --- captions -------------------------------------------------------------------------------------
Id addCaption(Sequence& s, Tick start, Tick end, const std::string& text);
bool splitCaption(Sequence& s, Id id, Tick t);
bool mergeCaptionWithNext(Sequence& s, Id id);
void deleteCaptions(Sequence& s, const std::vector<Id>& ids);
void sortCaptions(Sequence& s);

// --- markers ----------------------------------------------------------------------------------------
Id addMarker(Sequence& s, Tick t, const std::string& name = {});

// Re-bases keyframes when the clip-local origin moves by `delta` (local' = local - delta).
void shiftKeys(Clip& c, Tick delta);
// Edit points (clip starts/ends on all tracks), sorted unique.
std::vector<Tick> editPoints(const Sequence& s);

// --- sequences (tabs; the open one is Project::sequence) --------------------------------------------
int sequenceCount(const Project& p);
void switchSequence(Project& p, int tab);
void insertSequence(Project& p, int at, Sequence s);  // and open it
bool removeSequence(Project& p, int tab);             // fails for the last one
void renameSequence(Project& p, int tab, const std::string& name);

// --- nesting ---------------------------------------------------------------------------------------
// Moves the clips into a new sequence (stored after the open one) and puts a nested clip in their
// place (video + linked audio carrying the nested mix). Returns the nested clip ids.
std::vector<Id> nestClips(Project& p, const std::vector<Id>& ids, const std::string& name);

// --- animation presets -------------------------------------------------------------------------------
// Writes ordinary keyframes (so they stay editable) on a visual clip's motion / opacity.
enum class Animation { FadeIn, FadeOut, SlideUpIn, SlideLeftIn, SlideRightIn, PopIn, SlowZoom, SlideDownOut, Typewriter };
bool applyAnimation(Clip& c, Animation a, Tick frameDur, int seqW, int seqH);
// Punch-in zoom from clip-local time `local`: scales by `factor` toward the sequence point (px, py)
// - which stays where it is on screen - over `ramp`, holds, and eases back. Keyframes on Motion.
bool punchIn(Clip& c, Tick local, double px, double py, double factor, Tick ramp, Tick hold, Tick frameDur, int seqW,
             int seqH);

// --- silence ----------------------------------------------------------------------------------------
// Pauses in a level envelope: `levels` are peak amplitudes (0..1) of consecutive windows of
// `window` ticks starting at `origin`. Returns the quiet spans (below thresholdDb for at least
// minLength) shrunk by `pad` on both sides so speech edges survive, snapped to frames.
std::vector<TickRange> findSilences(const std::vector<float>& levels, Tick origin, Tick window, double thresholdDb,
                                    Tick minLength, Tick pad, Tick frameDur);
// Ripple-removes the spans (sorted, non-overlapping) on every unlocked track, last first.
void removeSpans(Sequence& s, std::vector<TickRange> spans);

// --- speed ramp ---------------------------------------------------------------------------------------
// Slow motion around timeline time t of a normal-speed clip: `slowSource` of media plays at `slow`
// speed, entered and left through ramps of `rampSource` media each, built from `steps` constant-
// speed segments easing between 100 % and `slow`. Later material ripples. Returns false if the
// clip cannot hold it (too short, not at 100 %, reversed).
bool speedRamp(Sequence& s, const std::vector<Asset>& assets, Id clipId, Tick t, double slow, Tick slowSource,
               Tick rampSource, int steps = 6);

// --- beats ------------------------------------------------------------------------------------------
// Onsets in a level envelope (peak amplitudes of `window`-tick windows from `origin`): rises well
// above the recent average, at least `minGap` apart, frame-snapped. For "markers at beats".
std::vector<Tick> findBeats(const std::vector<float>& levels, Tick origin, Tick window, Tick minGap, Tick frameDur,
                            double sensitivity = 1.0);

// --- frame rate -----------------------------------------------------------------------------------
// Changes the sequence frame rate and puts everything back on frames: cut points shared by two
// clips stay shared (each boundary is snapped once), nothing overlaps, sources stay inside their
// media, keyframes follow moved heads; transitions, markers, In/Out and captions are snapped too.
void changeFrameRate(Sequence& s, const std::vector<Asset>& assets, Rational fps);

// --- replace ------------------------------------------------------------------------------------
// Replace With Clip: the clips (and their linked partners) show `a` from source time `in` instead;
// position, speed, effects and keyframes stay. Partners keep their offset to each other; audio
// clips keep their stream when `a` has it. A clip longer than the new media is shortened.
// Locked tracks and reversed clips are skipped. Returns the number of clips changed.
int replaceMedia(Sequence& s, const Asset& a, const std::vector<Id>& clipIds, Tick in);

// --- music ----------------------------------------------------------------------------------------
// An audio clip (background music) made to last until `end` (or the next clip on its track): a
// longer one is cut there, a shorter one loops - each repeat joined by a constant-power crossfade
// - and the last piece fades out over `fade`. Returns the number of pieces (0 = not possible).
int fitAudioToEnd(Sequence& s, const std::vector<Asset>& assets, Id clipId, Tick end, Tick fade);

// --- instant replay -------------------------------------------------------------------------------
// The `len` before t on unlocked tracks, repeated right after t at `speed` (< 1 = slow motion);
// everything from t on ripples right. Keyframes follow the new timing. Returns the replay range
// (empty when there is nothing before t).
TickRange instantReplay(Sequence& s, Tick t, Tick len, double speed);

// --- highlights ------------------------------------------------------------------------------------
// Moments that get much louder than their surroundings (shots, explosions, shouting): the 0.3 s
// level against the median level of the half minute around it. Up to maxCount, the strongest
// first but returned in time order, at least minGap apart, at the start of each loud moment.
std::vector<Tick> findHighlights(const std::vector<float>& levels, Tick origin, Tick window, int maxCount, Tick minGap,
                                 Tick frameDur);

} // namespace cut::edit