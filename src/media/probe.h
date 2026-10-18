#pragma once
#include "core/model.h"

#include <string>

namespace cut {

// Reads container/stream info (runs in the job pool, never on the UI thread). Unsupported or
// corrupt files return info.valid=false with a user-readable error; never throws.
MediaInfo probeMedia(const std::string& path);

bool isImageExtension(const std::string& path);
bool isSupportedExtension(const std::string& path);
std::string mediaFileFilter();  // for file dialogs

} // namespace cut
