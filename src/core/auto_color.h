#pragma once
// Auto Color: Color Correction settings that neutralise a colour cast and use the tonal range,
// estimated from one frame. White balance is solved against the same multipliers the shader
// applies (layer.hlsl colorCorrect), on low-saturation midtones (grey world, partial strength so
// a frame that really is mostly green stays green-ish).
#include <cstdint>

namespace cut {

struct ColorFix {
    double exposure = 0;     // stops
    double contrast = 0;     // -100..100 (Color Correction units)
    double temperature = 0;  // -100..100
    double tint = 0;         // -100..100
    bool ok = false;         // enough usable pixels
};

// rgba: 8-bit, gamma-encoded, premultiplied (pixels with low alpha are ignored).
ColorFix autoColor(const uint8_t* rgba, int w, int h, int pitch);

} // namespace cut
