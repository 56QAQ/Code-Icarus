#include "icarus/util/log.h"

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace icarus {

namespace {
LogSink g_sink;
LogLevel g_level = LogLevel::Info;
}  // namespace

void set_log_sink(LogSink sink) { g_sink = std::move(sink); }
void set_log_level(LogLevel min_level) { g_level = min_level; }

void log_msg(LogLevel lvl, const std::string& msg) {
    if ((int)lvl < (int)g_level) return;
    if (g_sink) {
        g_sink(lvl, msg);
        return;
    }
    static const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    std::fprintf(stderr, "[%s] %s\n", names[(int)lvl], msg.c_str());
}

std::string strfmt(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string out;
    if (n > 0) {
        std::vector<char> buf((size_t)n + 1);
        std::vsnprintf(buf.data(), buf.size(), fmt, ap2);
        out.assign(buf.data(), (size_t)n);
    }
    va_end(ap2);
    return out;
}

}  // namespace icarus
