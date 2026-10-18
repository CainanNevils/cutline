#include "core/tracker.h"

#include <algorithm>
#include <cmath>

namespace cut {

namespace {
inline double sampleBilinear(const uint8_t* l, int w, int h, double x, double y) {
    x = std::clamp(x, 0.0, double(w - 1));
    y = std::clamp(y, 0.0, double(h - 1));
    const int x0 = int(x), y0 = int(y);
    const int x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
    const double fx = x - x0, fy = y - y0;
    const double a = l[size_t(y0) * w + x0], b = l[size_t(y0) * w + x1];
    const double c = l[size_t(y1) * w + x0], d = l[size_t(y1) * w + x1];
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}
} // namespace

bool RegionTracker::start(const uint8_t* luma, int w, int h, double cx, double cy, double rw, double rh) {
    if (!luma || w < 4 || h < 4 || rw < 2 || rh < 2) return false;
    cx_ = cx, cy_ = cy, rw_ = rw, rh_ = rh;
    const int nx = std::clamp(int(rw), 4, 32), ny = std::clamp(int(rh), 4, 32);
    ox_.clear(), oy_.clear(), tpl_.clear();
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            ox_.push_back(float(-rw / 2 + (i + 0.5) * rw / nx));
            oy_.push_back(float(-rh / 2 + (j + 0.5) * rh / ny));
        }
    double mean = 0;
    for (size_t k = 0; k < ox_.size(); ++k) {
        tpl_.push_back(float(sampleBilinear(luma, w, h, cx + ox_[k], cy + oy_[k])));
        mean += tpl_.back();
    }
    mean /= double(tpl_.size());
    tplNorm_ = 0;
    for (auto& v : tpl_) {
        v = float(v - mean);
        tplNorm_ += double(v) * v;
    }
    tplNorm_ = std::sqrt(tplNorm_);
    conf_ = 1;
    return tplNorm_ > 1.0;  // something to follow (not a flat area)
}

double RegionTracker::score(const uint8_t* luma, int w, int h, double dx, double dy) const {
    const size_t n = tpl_.size();
    double sum = 0, sum2 = 0, cross = 0;
    for (size_t k = 0; k < n; ++k) {
        const double v = sampleBilinear(luma, w, h, cx_ + dx + ox_[k], cy_ + dy + oy_[k]);
        sum += v;
        sum2 += v * v;
        cross += v * tpl_[k];
    }
    const double var = sum2 - sum * sum / double(n);
    if (var <= 1e-6 || tplNorm_ <= 0) return 0;
    return cross / (tplNorm_ * std::sqrt(var));  // (tpl is zero-mean, so the candidate mean drops out)
}

bool RegionTracker::track(const uint8_t* luma, int w, int h) {
    if (tpl_.empty()) return false;
    const int R = searchRadius > 0 ? searchRadius : std::clamp(int(0.6 * std::max(rw_, rh_)), 12, 80);
    double best = -2;
    int bx = 0, by = 0;
    for (int dy = -R; dy <= R; dy += 2)
        for (int dx = -R; dx <= R; dx += 2) {
            const double sc = score(luma, w, h, dx, dy);
            if (sc > best) best = sc, bx = dx, by = dy;
        }
    // refine around the coarse best
    const int cxi = bx, cyi = by;
    for (int dy = cyi - 2; dy <= cyi + 2; ++dy)
        for (int dx = cxi - 2; dx <= cxi + 2; ++dx) {
            const double sc = score(luma, w, h, dx, dy);
            if (sc > best) best = sc, bx = dx, by = dy;
        }
    // sub-pixel: parabola through the neighbours
    auto peak = [](double l, double c, double r) {
        const double d = l - 2 * c + r;
        return std::abs(d) > 1e-9 ? std::clamp(0.5 * (l - r) / d, -0.5, 0.5) : 0.0;
    };
    const double sx = peak(score(luma, w, h, bx - 1, by), best, score(luma, w, h, bx + 1, by));
    const double sy = peak(score(luma, w, h, bx, by - 1), best, score(luma, w, h, bx, by + 1));
    conf_ = best;
    if (best < 0.5) return false;  // lost: stay where it was
    cx_ += bx + sx;
    cy_ += by + sy;
    if (best > 0.8) {  // adapt the template a little to what it looks like now
        std::vector<float> now(tpl_.size());
        double mean = 0;
        for (size_t k = 0; k < tpl_.size(); ++k) mean += now[k] = float(sampleBilinear(luma, w, h, cx_ + ox_[k], cy_ + oy_[k]));
        mean /= double(now.size());
        double norm = 0;
        for (size_t k = 0; k < tpl_.size(); ++k) {
            tpl_[k] = float(0.9 * tpl_[k] + 0.1 * (now[k] - mean));
            norm += double(tpl_[k]) * tpl_[k];
        }
        tplNorm_ = std::sqrt(norm);
    }
    return true;
}

int bestWindow(const std::vector<double>& energy, int cw) {
    const int n = int(energy.size());
    if (cw >= n || n == 0) return 0;
    double sum = 0;
    for (int i = 0; i < cw; ++i) sum += energy[size_t(i)];
    double best = sum;
    int at = 0;
    const int mid = (n - cw) / 2;
    for (int i = 1; i + cw <= n; ++i) {
        sum += energy[size_t(i + cw - 1)] - energy[size_t(i - 1)];
        if (sum > best + 1e-9 || (std::abs(sum - best) <= 1e-9 && std::abs(i - mid) < std::abs(at - mid))) best = sum, at = i;
    }
    return at;
}

} // namespace cut
