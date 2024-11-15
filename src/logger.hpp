#pragma once
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

enum class LogLevel { DEBUG = 0, INFO, WARN, ERROR };

struct LogEntry {
    std::string time;    // "HH:MM:SS"
    LogLevel    level;
    std::string msg;
};

class Logger {
public:
    static Logger& instance();

    void init(const std::string& file_path, bool verbose, size_t rotate_bytes);

    // When TUI mode is on:
    //   - stderr output is suppressed (ncurses owns the terminal)
    //   - messages are buffered in a ring for the TUI log panel
    void set_tui_mode(bool enabled);

    std::vector<LogEntry> recent(size_t n = 8) const;

    void log(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
    void debug(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void info (const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void warn (const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void error(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

private:
    Logger() = default;
    ~Logger();

    void write_entry(LogLevel level, const char* msg);
    void check_rotate();

    mutable std::mutex mu_;
    FILE*       file_{nullptr};
    std::string file_path_;
    size_t      rotate_bytes_{10 * 1024 * 1024};
    size_t      bytes_written_{0};
    bool        verbose_{false};
    bool        tui_mode_{false};

    std::deque<LogEntry> buf_;          // ring for TUI panel
    static constexpr size_t BUF_MAX = 200;
};

#define LOG_DEBUG(...) Logger::instance().debug(__VA_ARGS__)
#define LOG_INFO(...)  Logger::instance().info (__VA_ARGS__)
#define LOG_WARN(...)  Logger::instance().warn (__VA_ARGS__)
#define LOG_ERROR(...) Logger::instance().error(__VA_ARGS__)
