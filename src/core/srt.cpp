#include "core/srt.h"

#include <cstdio>
#include <sstream>

namespace cut {

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// "HH:MM:SS,mmm" or "MM:SS.mmm"
bool parseTime(const std::string& s, Tick& out) {
    int h = 0, m = 0, sec = 0, ms = 0;
    std::string t = trim(s);
    for (char& c : t)
        if (c == ',') c = '.';
    int n = std::sscanf(t.c_str(), "%d:%d:%d.%d", &h, &m, &sec, &ms);
    if (n < 4) {
        h = 0;
        n = std::sscanf(t.c_str(), "%d:%d.%d", &m, &sec, &ms);
        if (n < 3) return false;
    }
    // normalise fractional part length (e.g. ".5" = 500 ms)
    size_t dot = t.rfind('.');
    if (dot != std::string::npos) {
        size_t digits = 0;
        while (dot + 1 + digits < t.size() && isdigit((unsigned char)t[dot + 1 + digits])) ++digits;
        if (digits == 1) ms *= 100;
        else if (digits == 2) ms *= 10;
    }
    int64_t totalMs = ((int64_t(h) * 60 + m) * 60 + sec) * 1000 + ms;
    out = totalMs * (kTicksPerSecond / 1000);
    return true;
}

std::string fmtTime(Tick t, char sep) {
    if (t < 0) t = 0;
    int64_t ms = (t + (kTicksPerSecond / 2000)) / (kTicksPerSecond / 1000);
    int64_t h = ms / 3600000, m = (ms / 60000) % 60, s = (ms / 1000) % 60, r = ms % 1000;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld%c%03lld", (long long)h, (long long)m, (long long)s, sep,
                  (long long)r);
    return buf;
}
} // namespace

bool parseSrt(const std::string& textIn, std::vector<SrtEntry>& out, std::string& error) {
    out.clear();
    std::string text = textIn;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);
    std::istringstream in(text);
    std::string line;
    SrtEntry cur;
    bool inCue = false;
    auto flush = [&] {
        if (inCue) {
            while (!cur.text.empty() && cur.text.back() == '\n') cur.text.pop_back();
            out.push_back(cur);
        }
        cur = {};
        inCue = false;
    };
    while (std::getline(in, line)) {
        std::string l = trim(line);
        size_t arrow = l.find("-->");
        if (arrow != std::string::npos) {
            flush();
            std::string a = l.substr(0, arrow), b = l.substr(arrow + 3);
            // strip VTT cue settings after the end time
            b = trim(b);
            size_t sp = b.find(' ');
            if (sp != std::string::npos) b = b.substr(0, sp);
            if (!parseTime(a, cur.start) || !parseTime(b, cur.end)) {
                cur = {};
                continue;
            }
            inCue = true;
            continue;
        }
        if (l.empty()) {
            flush();
            continue;
        }
        if (!inCue) continue;  // index lines, WEBVTT header, notes
        if (!cur.text.empty()) cur.text += '\n';
        cur.text += l;
    }
    flush();
    if (out.empty() && !trim(text).empty()) {
        error = "No subtitles found (expected SRT \"00:00:01,000 --> 00:00:02,000\" lines).";
        return false;
    }
    return true;
}

std::string writeSrt(const std::vector<SrtEntry>& entries) {
    std::string s;
    int i = 1;
    for (const auto& e : entries) {
        s += std::to_string(i++) + "\n";
        s += fmtTime(e.start, ',') + " --> " + fmtTime(e.end, ',') + "\n";
        s += e.text + "\n\n";
    }
    return s;
}

std::string writeVtt(const std::vector<SrtEntry>& entries) {
    std::string s = "WEBVTT\n\n";
    for (const auto& e : entries) {
        s += fmtTime(e.start, '.') + " --> " + fmtTime(e.end, '.') + "\n";
        s += e.text + "\n\n";
    }
    return s;
}

std::vector<SrtEntry> captionsToSrt(const Sequence& s) {
    std::vector<SrtEntry> r;
    for (const auto& c : s.captions) r.push_back({c.start, c.end, c.text});
    return r;
}

} // namespace cut
