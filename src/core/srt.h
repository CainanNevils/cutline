#pragma once
#include "core/model.h"

#include <string>
#include <vector>

namespace cut {

struct SrtEntry {
    Tick start = 0, end = 0;
    std::string text;  // lines joined with '\n'
    friend bool operator==(const SrtEntry&, const SrtEntry&) = default;
};

// Parses SRT (and the cue body of simple WebVTT). Tolerates BOM, CRLF, '.' as ms separator,
// missing index lines. Returns false only if nothing could be parsed from non-empty input.
bool parseSrt(const std::string& text, std::vector<SrtEntry>& out, std::string& error);
std::string writeSrt(const std::vector<SrtEntry>& entries);
std::string writeVtt(const std::vector<SrtEntry>& entries);

std::vector<SrtEntry> captionsToSrt(const Sequence& s);

} // namespace cut
