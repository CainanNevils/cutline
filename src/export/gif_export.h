#pragma once
// Animated GIF export for short clips: the range is rendered at a small size (same renderer as
// export, original media), then FFmpeg's palettegen + paletteuse pick one 256-colour palette for
// the whole clip (Bayer dithering) and the gif encoder writes a looping GIF.
#include "engine/snapshot.h"

#include <atomic>
#include <string>

namespace cut {

struct GifSettings {
    std::string path;
    int width = 480;  // height follows the sequence aspect
    int fps = 15;
    Tick from = 0, to = 0;
};

constexpr Tick kGifMaxLength = 60 * kTicksPerSecond;  // frames are held in memory for the palette

bool exportGif(SnapshotPtr snap, const GifSettings& s, std::atomic<double>& progress, std::atomic<bool>& cancel,
               std::string& error);

} // namespace cut
