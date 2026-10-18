#pragma once
// CPU text rasterization (Qt QPainter) into a tightly bounded premultiplied BGRA image.
// Called only when text or style changes; transform/opacity animate on the GPU.
#include "core/model.h"

#include <cstdint>
#include <vector>

namespace cut {

struct TextImage {
    int width = 0, height = 0;
    std::vector<uint8_t> bgra;  // premultiplied, pitch = width*4
    // offset of the text block centre from the image centre (non-zero when a shadow pads one side)
    float centerDx = 0, centerDy = 0;
};

// `scale` multiplies all pixel sizes (preview at reduced resolution still uses 1.0: the
// texture is sampled down on the GPU).
// visibleChars >= 0 draws only that many characters (line breaks not counted) inside the layout
// of the whole text, so a typewriter reveal does not move or reflow anything.
TextImage rasterizeText(const std::string& utf8, const TextStyle& st, double scale = 1.0, int visibleChars = -1);
uint64_t textHash(const std::string& utf8, const TextStyle& st, int visibleChars = -1);

} // namespace cut
