#include "core/lut.h"

#include "core/project_io.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace cut {

bool parseCube(const std::string& text, Lut3D& out, std::string& error) {
    std::istringstream in(text);
    std::string line;
    int size3 = 0, size1 = 0;
    float dmin[3] = {0, 0, 0}, dmax[3] = {1, 1, 1};
    std::vector<float> vals;
    std::string title;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos || line[p] == '#') continue;
        std::istringstream ls(line.substr(p));
        std::string key;
        ls >> key;
        if (key == "TITLE") {
            size_t q = line.find('"');
            if (q != std::string::npos) title = line.substr(q + 1, line.rfind('"') - q - 1);
        } else if (key == "LUT_3D_SIZE") ls >> size3;
        else if (key == "LUT_1D_SIZE") ls >> size1;
        else if (key == "DOMAIN_MIN") ls >> dmin[0] >> dmin[1] >> dmin[2];
        else if (key == "DOMAIN_MAX") ls >> dmax[0] >> dmax[1] >> dmax[2];
        else if (key == "LUT_1D_INPUT_RANGE" || key == "LUT_3D_INPUT_RANGE") {
            float a = 0, b = 1;
            ls >> a >> b;
            for (int i = 0; i < 3; ++i) {
                dmin[i] = a;
                dmax[i] = b;
            }
        } else if ((key[0] >= '0' && key[0] <= '9') || key[0] == '-' || key[0] == '.') {
            std::istringstream vs(line.substr(p));
            float r, g, b;
            if (vs >> r >> g >> b) {
                vals.push_back(r);
                vals.push_back(g);
                vals.push_back(b);
            }
        }
    }
    auto norm = [&](float v, int ch) { return (v - dmin[ch]) / std::max(1e-6f, dmax[ch] - dmin[ch]); };
    if (size3 >= 2) {
        if (vals.size() != size_t(size3) * size3 * size3 * 3) {
            error = "LUT has " + std::to_string(vals.size() / 3) + " entries, expected " +
                    std::to_string(size3 * size3 * size3);
            return false;
        }
        out.size = size3;
        out.data = std::move(vals);
        out.title = title;
        // DOMAIN only affects inputs; fold it in by resampling if non-default.
        bool defaultDomain = dmin[0] == 0 && dmin[1] == 0 && dmin[2] == 0 && dmax[0] == 1 && dmax[1] == 1 && dmax[2] == 1;
        if (!defaultDomain) {
            Lut3D src = out;
            for (int b = 0; b < size3; ++b)
                for (int g = 0; g < size3; ++g)
                    for (int r = 0; r < size3; ++r) {
                        float in3[3] = {norm(float(r) / (size3 - 1), 0), norm(float(g) / (size3 - 1), 1),
                                        norm(float(b) / (size3 - 1), 2)};
                        for (float& v : in3) v = std::clamp(v, 0.f, 1.f);
                        float o[3];
                        applyLut(src, in3, o);
                        size_t i = (size_t(b) * size3 * size3 + size_t(g) * size3 + r) * 3;
                        out.data[i] = o[0];
                        out.data[i + 1] = o[1];
                        out.data[i + 2] = o[2];
                    }
        }
        return true;
    }
    if (size1 >= 2) {
        if (vals.size() != size_t(size1) * 3) {
            error = "1D LUT entry count mismatch";
            return false;
        }
        auto lookup1 = [&](float x, int ch) {
            x = std::clamp(norm(x, ch), 0.f, 1.f) * float(size1 - 1);
            int i0 = int(std::floor(x));
            int i1 = std::min(i0 + 1, size1 - 1);
            float f = x - float(i0);
            return vals[size_t(i0) * 3 + ch] * (1 - f) + vals[size_t(i1) * 3 + ch] * f;
        };
        const int n = 33;
        out.size = n;
        out.title = title;
        out.data.resize(size_t(n) * n * n * 3);
        for (int b = 0; b < n; ++b)
            for (int g = 0; g < n; ++g)
                for (int r = 0; r < n; ++r) {
                    size_t i = (size_t(b) * n * n + size_t(g) * n + r) * 3;
                    out.data[i] = lookup1(float(r) / (n - 1), 0);
                    out.data[i + 1] = lookup1(float(g) / (n - 1), 1);
                    out.data[i + 2] = lookup1(float(b) / (n - 1), 2);
                }
        return true;
    }
    error = "Not a .cube LUT (missing LUT_3D_SIZE / LUT_1D_SIZE).";
    return false;
}

bool loadCubeFile(const std::string& path, Lut3D& out, std::string& error) {
    std::string text;
    if (!readFileUtf8(path, text)) {
        error = "Could not read " + path;
        return false;
    }
    return parseCube(text, out, error);
}

void applyLut(const Lut3D& lut, const float in[3], float out[3]) {
    const int n = lut.size;
    float p[3];
    int i0[3], i1[3];
    float f[3];
    for (int c = 0; c < 3; ++c) {
        p[c] = std::clamp(in[c], 0.f, 1.f) * float(n - 1);
        i0[c] = std::min(int(std::floor(p[c])), n - 2);
        i1[c] = i0[c] + 1;
        f[c] = p[c] - float(i0[c]);
    }
    auto at = [&](int r, int g, int b, int ch) { return lut.data[(size_t(b) * n * n + size_t(g) * n + r) * 3 + ch]; };
    for (int ch = 0; ch < 3; ++ch) {
        float c00 = at(i0[0], i0[1], i0[2], ch) * (1 - f[0]) + at(i1[0], i0[1], i0[2], ch) * f[0];
        float c10 = at(i0[0], i1[1], i0[2], ch) * (1 - f[0]) + at(i1[0], i1[1], i0[2], ch) * f[0];
        float c01 = at(i0[0], i0[1], i1[2], ch) * (1 - f[0]) + at(i1[0], i0[1], i1[2], ch) * f[0];
        float c11 = at(i0[0], i1[1], i1[2], ch) * (1 - f[0]) + at(i1[0], i1[1], i1[2], ch) * f[0];
        float c0 = c00 * (1 - f[1]) + c10 * f[1];
        float c1 = c01 * (1 - f[1]) + c11 * f[1];
        out[ch] = c0 * (1 - f[2]) + c1 * f[2];
    }
}

} // namespace cut
