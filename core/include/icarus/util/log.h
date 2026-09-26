// Minimal logging sink. Hosts (CLI, Godot) can install a callback.
#pragma once

#include <functional>
#include <string>

namespace icarus {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

using LogSink = std::function<void(LogLevel, const std::string&)>;

void set_log_sink(LogSink sink);
void set_log_level(LogLevel min_level);
void log_msg(LogLevel lvl, const std::string& msg);

inline void log_debug(const std::string& m) { log_msg(LogLevel::Debug, m); }
inline void log_info(const std::string& m) { log_msg(LogLevel::Info, m); }
inline void log_warn(const std::string& m) { log_msg(LogLevel::Warn, m); }
inline void log_error(const std::string& m) { log_msg(LogLevel::Error, m); }

std::string strfmt(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

}  // namespace icarus
