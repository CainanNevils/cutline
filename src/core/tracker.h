#pragma once
// Point/region tracker for masks: normalized cross-correlation of a luma template (sampled on a
// grid of at most 32 x 32 points) over a search window around the last position, coarse (every
// 2 px) then refined (1 px, then sub-pixel by a parabola fit). The template adapts slowly so
// lighting changes do not break it while drift stays small.
#include <cstdint>
#include <vector>

namespace cut {

class RegionTracker {
public:
    // luma: w*h bytes. (cx, cy) centre and (rw, rh) size of the region in pixels.
    bool start(const uint8_t* luma, int w, int h, double cx, double cy, double rw, double rh);
    // Finds the region in the next frame; updates the centre. False when it is lost (low match).
    bool track(const uint8_t* luma, int w, int h);
    double x() const { return cx_; }
    double y() const { return cy_; }
    double confidence() const { return conf_; }
    int searchRadius = 0;  // px; 0 = automatic (60 % of the region size, at least 12)

private:
    double score(const uint8_t* luma, int w, int h, double dx, double dy) const;
    std::vector<float> tpl_;          // zero-mean template samples
    std::vector<float> ox_, oy_;      // sample offsets from the centre
    double tplNorm_ = 0;
    double cx_ = 0, cy_ = 0, rw_ = 0, rh_ = 0;
    double conf_ = 0;
};

// Auto reframe: the left edge of a window of width `cw` that holds the most of `energy` (one value
// per column; ties prefer the middle).
int bestWindow(const std::vector<double>& energy, int cw);

} // namespace cut
