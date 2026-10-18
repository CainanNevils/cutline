#include "media/ff.h"

#include <cstdlib>
#include <mutex>

namespace cut::ff {

void initLogging() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char* v = std::getenv("CUTLINE_FFLOG");
        av_log_set_level(v ? AV_LOG_INFO : AV_LOG_ERROR);
        if (!v) av_log_set_level(AV_LOG_QUIET);
    });
}

FormatPtr openInput(const std::string& path, std::string& error) {
    initLogging();
    AVFormatContext* fmt = nullptr;
    int r = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (r < 0) {
        error = "Cannot open file: " + err(r);
        return nullptr;
    }
    FormatPtr p(fmt);
    r = avformat_find_stream_info(fmt, nullptr);
    if (r < 0) {
        error = "Cannot read stream info: " + err(r);
        return nullptr;
    }
    return p;
}

} // namespace cut::ff
