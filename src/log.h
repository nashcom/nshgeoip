#pragma once

// Minimal leveled logger to stdout/stderr. systemd/journald captures both
// streams, so no log file handling is implemented here.
//
// INFO  -> stdout (startup, config, lifecycle events)
// WARN  -> stderr (malformed/invalid client requests, non-fatal issues)
// ERROR -> stderr (failed operations that do not require exiting)
// FATAL -> stderr (logged immediately before process exit)
// DEBUG -> stdout, only when debug logging is enabled in the config

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace nshgeoip
{

enum class LogLevel
{
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

class Logger
{
public:
    static Logger &instance()
    {
        static Logger logger;
        return logger;
    }

    void set_debug_enabled(bool enabled)
    {
        debug_enabled_ = enabled;
    }
    bool debug_enabled() const
    {
        return debug_enabled_;
    }

    void log(LogLevel level, const std::string &msg)
    {
        if (level == LogLevel::Debug && !debug_enabled_)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        FILE *stream = (level == LogLevel::Info) ? stdout : stderr;
        std::fprintf(stream, "%s [%s] %s\n", timestamp().c_str(), level_name(level), msg.c_str());
        std::fflush(stream);
    }

private:
    Logger() = default;

    static const char *level_name(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Fatal:
            return "FATAL";
        }
        return "?";
    }

    static std::string timestamp()
    {
        using namespace std::chrono;
        auto now = system_clock::now();
        std::time_t t = system_clock::to_time_t(now);
        std::tm tm_buf{};
        gmtime_r(&t, &tm_buf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        return std::string(buf);
    }

    std::mutex mutex_;
    bool debug_enabled_ = false;
};

inline void log_debug(const std::string &msg)
{
    Logger::instance().log(LogLevel::Debug, msg);
}
inline void log_info(const std::string &msg)
{
    Logger::instance().log(LogLevel::Info, msg);
}
inline void log_warn(const std::string &msg)
{
    Logger::instance().log(LogLevel::Warn, msg);
}
inline void log_error(const std::string &msg)
{
    Logger::instance().log(LogLevel::Error, msg);
}
inline void log_fatal(const std::string &msg)
{
    Logger::instance().log(LogLevel::Fatal, msg);
}

} // namespace nshgeoip
