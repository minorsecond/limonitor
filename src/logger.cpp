#include "logger.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>

static const char* level_color(LogLevel l) {
    switch (l) {
        case LogLevel::DEBUG: return "\033[0;37m";
        case LogLevel::INFO:  return "\033[0;32m";
        case LogLevel::WARN:  return "\033[0;33m";
        case LogLevel::ERROR: return "\033[0;31m";
    }
    return "";
}
static const char* level_tag(LogLevel l) {
    switch (l) {
        case LogLevel::DEBUG: return "DBG";
        case LogLevel::INFO:  return "INF";
        case LogLevel::WARN:  return "WRN";
        case LogLevel::ERROR: return "ERR";
    }
    return "???";
}

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::init(const std::string& file_path, bool verbose, size_t rotate_bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    verbose_      = verbose;
    rotate_bytes_ = rotate_bytes;
    if (!file_path.empty()) {
        file_path_ = file_path;
        file_ = std::fopen(file_path.c_str(), "a");
        if (!file_) {
            std::fprintf(stderr, "Logger: cannot open '%s': %s\n",
                         file_path.c_str(), std::strerror(errno));
        }
    }
}

Logger::~Logger() {
    if (file_) std::fclose(file_);
}

void Logger::set_tui_mode(bool enabled) {
    std::lock_guard<std::mutex> lk(mu_);
    tui_mode_ = enabled;
}

std::vector<LogEntry> Logger::recent(size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (n == 0 || n >= buf_.size()) {
        return std::vector<LogEntry>(buf_.begin(), buf_.end());
    }
    auto it = buf_.end() - static_cast<ptrdiff_t>(n);
    return std::vector<LogEntry>(it, buf_.end());
}

void Logger::write_entry(LogLevel level, const char* msg) {
    std::time_t now = std::time(nullptr);
    struct tm tm_buf{};
    localtime_r(&now, &tm_buf);

    char ts_full[32], ts_short[12];
    std::strftime(ts_full,  sizeof(ts_full),  "%Y-%m-%d %H:%M:%S", &tm_buf);
    std::strftime(ts_short, sizeof(ts_short), "%H:%M:%S",           &tm_buf);

    if (tui_mode_) {
        // Suppress stderr — ncurses owns the terminal.
        // Buffer for the TUI log panel instead.
        if (level >= LogLevel::INFO || verbose_) {
            buf_.push_back({ts_short, level, msg});
            while (buf_.size() > BUF_MAX) buf_.pop_front();
        }
    } else {
        // Normal mode: write colored output to stderr.
        if (level >= LogLevel::INFO || verbose_) {
            std::fprintf(stderr, "%s[%s %s]\033[0m %s\n",
                         level_color(level), ts_full, level_tag(level), msg);
            std::fflush(stderr);
        }
    }

    // File logging is always active (unaffected by TUI mode).
    if (file_) {
        int written = std::fprintf(file_, "[%s %s] %s\n",
                                   ts_full, level_tag(level), msg);
        if (written > 0) {
            bytes_written_ += static_cast<size_t>(written);
            std::fflush(file_);
            check_rotate();
        }
    }
}

void Logger::check_rotate() {
    if (bytes_written_ >= rotate_bytes_ && !file_path_.empty()) {
        std::fclose(file_);
        std::string rotated = file_path_ + ".1";
        std::rename(file_path_.c_str(), rotated.c_str());
        file_ = std::fopen(file_path_.c_str(), "a");
        bytes_written_ = 0;
        if (!file_)
            std::fprintf(stderr, "Logger: rotate failed for '%s'\n", file_path_.c_str());
    }
}

void Logger::log(LogLevel level, const char* fmt, ...) {
    char buf[4096];
    va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    std::lock_guard<std::mutex> lk(mu_); write_entry(level, buf);
}
void Logger::debug(const char* fmt, ...) {
    if (!verbose_) return;
    char buf[4096];
    va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    std::lock_guard<std::mutex> lk(mu_); write_entry(LogLevel::DEBUG, buf);
}
void Logger::info(const char* fmt, ...) {
    char buf[4096];
    va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    std::lock_guard<std::mutex> lk(mu_); write_entry(LogLevel::INFO, buf);
}
void Logger::warn(const char* fmt, ...) {
    char buf[4096];
    va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    std::lock_guard<std::mutex> lk(mu_); write_entry(LogLevel::WARN, buf);
}
void Logger::error(const char* fmt, ...) {
    char buf[4096];
    va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    std::lock_guard<std::mutex> lk(mu_); write_entry(LogLevel::ERROR, buf);
}
