#pragma once
#include <string>
#include <vector>

namespace cut {

// A 3D LUT (1D .cube files are expanded to 3D). data = size^3 RGB triplets, red fastest.
struct Lut3D {
    int size = 0;
    std::vector<float> data;
    std::string title;
    bool valid() const { return size >= 2 && data.size() == size_t(size) * size * size * 3; }
};

bool parseCube(const std::string& text, Lut3D& out, std::string& error);
bool loadCubeFile(const std::string& path, Lut3D& out, std::string& error);
// Trilinear lookup (for tests / CPU reference).
void applyLut(const Lut3D& lut, const float in[3], float out[3]);

} // namespace cut
