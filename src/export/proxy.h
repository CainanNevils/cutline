#pragma once
// Proxy generation: 720p, all-intra (GOP 1) H.264 via NVENC (libx264 fallback), video only.
// Frames keep the source's media timestamps so proxy time == original time (VFR-safe).
#include "core/model.h"
#include "engine/job_pool.h"

#include <string>

namespace cut {

int proxyHeight();
bool generateProxy(const Asset& a, const std::string& outPath, JobToken& tok, std::string& error);

} // namespace cut
