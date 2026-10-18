#pragma once
// CPU-side extraction for caches: thumbnail strips (keyframes only, fast) and audio peaks.
#include "core/model.h"

#include <cstdint>
#include <functional>
#include <string>

namespace cut {

// Calls `sink` with BGRA images (thumbH tall, aspect kept) at roughly `interval` spacing, using
// keyframes only. Returns false on failure or cancellation.
bool extractThumbnails(const std::string& path, const MediaInfo& info, int thumbH, Tick interval,
                       const std::function<void(Tick pts, const uint8_t* bgra, int w, int h, int pitch)>& sink,
                       const std::function<bool()>& cancelled);

} // namespace cut
