#pragma once
// Scene change detection for long recordings: decodes the media in [from, to) (non-reference
// frames skipped), compares tiny luma thumbnails of consecutive decoded frames (pixel and
// histogram difference) and reports abrupt changes that stand out from the recent motion.
#include "core/model.h"

#include <functional>
#include <string>
#include <vector>

namespace cut {

struct SceneOptions {
    double threshold = 0.30;       // minimum change score (0..1)
    Tick minGap = kTicksPerSecond / 2;
};

// Returns media times (ticks, media start = 0) of the first frame of each new scene.
// progress(0..1) is called now and then; cancelled() stops early (returns false).
bool detectScenes(const std::string& path, const MediaInfo& info, Tick from, Tick to, const SceneOptions& opt,
                  std::vector<Tick>& cuts, const std::function<void(double)>& progress, const std::function<bool()>& cancelled);

// The decision rule, separately testable: scores[i] is the change between frame i-1 and i.
std::vector<size_t> pickSceneCuts(const std::vector<double>& scores, const std::vector<Tick>& times, const SceneOptions& opt);

} // namespace cut
