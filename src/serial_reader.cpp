#include "serial_reader.hpp"
#include "logger.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <chrono>
#include <unistd.h>

// ---------------------------------------------------------------------------
// serial_prompts free functions
// ---------------------------------------------------------------------------

namespace serial_prompts {

const char* prompt_response(const std::string& line) {
    std::string trimmed = line;
    while (!trimmed.empty() &&
           (trimmed.back() == ' ' || trimmed.back() == '\t'))
        trimmed.pop_back();
    if (trimmed.empty() ||
        (trimmed.back() != ':' && trimmed.back() != '?'))
        return nullptr;

    // Max charge current: set to 10 A (charger max for 100 Ah LiFePO4).
    if (line.find("Max charge current") != std::string::npos)
        return "10\r";
    // Reset to defaults: decline so saved settings are preserved.
    if (line.find("Reset to default") != std::string::npos)
        return "N\r";
    // Battery Save Mode: disable.
    if (line.find("Battery Save Mode") != std::string::npos)
        return "N\r";
    // All other prompts (including Battery type): accept the current/default
    // value with a bare CR.  Explicitly re-sending the battery type number
    // (e.g. "4\r") causes the device to soft-reset even when the value is
    // already correct, so we always fall through to the default here.
    return "\r";
}

bool is_unterminated_prompt(const std::string& partial) {
    std::string trimmed = partial;
    while (!trimmed.empty() &&
           (trimmed.back() == ' ' || trimmed.back() == '\t'))
        trimmed.pop_back();
    if (trimmed.size() < 2) return false;
    char prev = trimmed[trimmed.size() - 2];
    char last = trimmed[trimmed.size() - 1];
    return prev == '>' && (last == ':' || last == '?');
}

} // namespace serial_prompts

// ---------------------------------------------------------------------------

SerialReader::SerialReader(std::string device, int baud)
    : device_(std::move(device)), baud_(baud) {}

SerialReader::~SerialReader() { stop(); }

static speed_t to_speed(int baud) {
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:
            LOG_WARN("Serial: unknown baud %d, defaulting to 115200", baud);
            return B115200;
    }
}

int SerialReader::open_port() {
    int fd = open(device_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        LOG_ERROR("Serial: cannot open %s: %s", device_.c_str(), strerror(errno));
        return -1;
    }

    struct termios tty{};
    tcgetattr(fd, &tty);

    speed_t spd = to_speed(baud_);
    cfsetospeed(&tty, spd);
    cfsetispeed(&tty, spd);

    tty.c_cflag  = (tty.c_cflag & ~CSIZE) | CS8; // 8-bit chars
    tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS); // 8N1, no flow control
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // raw mode
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 10; // 1s read timeout

    tcsetattr(fd, TCSANOW, &tty);

    // Switch back to blocking mode (timeout via VTIME)
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    LOG_INFO("Serial: opened %s at %d baud", device_.c_str(), baud_);
    return fd;
}

bool SerialReader::start() {
    fd_ = open_port();
    if (fd_ < 0) return false;
    running_ = true;
    thread_ = std::thread(&SerialReader::read_loop, this);
    return true;
}

void SerialReader::stop() {
    running_ = false;
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    if (thread_.joinable()) thread_.join();
}

void SerialReader::read_loop() {
    // Toggle DTR from inside the thread so this loop is already running when the
    // device responds with setup prompts.
    // 1. Drop DTR/RTS to signal disconnect to the device.
    // 2. Wait 200 ms for the device to react (streaming stops).
    // 3. Flush the kernel RX buffer NOW — clears stale streaming bytes that
    //    accumulated before the DTR drop, while preserving any prompts that
    //    arrive after we raise DTR in step 4.
    // 4. Raise DTR/RTS — device will send fresh setup prompts.
    // 5. Read — next bytes will be the fresh prompt sequence.
    {
        int modem = 0;
        ioctl(fd_, TIOCMGET, &modem);
        modem &= ~(TIOCM_DTR | TIOCM_RTS);
        ioctl(fd_, TIOCMSET, &modem);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        tcflush(fd_, TCIFLUSH); // discard stale streaming data accumulated before DTR drop
        modem |= (TIOCM_DTR | TIOCM_RTS);
        ioctl(fd_, TIOCMSET, &modem);
        LOG_DEBUG("Serial: DTR wake pulse sent, RX buffer flushed");
    }

    std::string line_buf;
    line_buf.reserve(256);
    std::string pending_status;
    char rbuf[1024];
    size_t line_len = 0;
    auto last_log_time = std::chrono::steady_clock::now();
    bool reconfigure_sent = false; // send 'S' at most once per session

    // Returns true if the line is a prompt that was handled (caller should not
    // process it further as streaming data).
    auto handle_prompt = [&](const std::string& line) -> bool {
        const char* resp = serial_prompts::prompt_response(line);
        if (!resp) return false;
        LOG_DEBUG("Serial TX: prompt reply for [%s]", line.c_str());
        (void)write(fd_, resp, __builtin_strlen(resp));
        return true;
    };

    while (running_) {
        ssize_t nr = read(fd_, rbuf, sizeof(rbuf));
        if (nr < 0) {
            if (!running_) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            LOG_WARN("Serial: read error: %s", strerror(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        if (nr == 0) {
            // Device stopped sending. If we have an unterminated 'S'-menu prompt
            // in line_buf (these never end with '\n'), respond to it now.
            if (!line_buf.empty() && serial_prompts::is_unterminated_prompt(line_buf)) {
                if (handle_prompt(line_buf)) {
                    line_buf.clear();
                    line_len = 0;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        for (ssize_t i = 0; i < nr; ++i) {
            char c = rbuf[i];
            if (c == '\r') continue;
            if (c != '\n') {
                if (line_len < 1024) {
                    line_buf += c;
                    line_len++;
                }
                continue;
            }

            std::string line = std::move(line_buf);
            line_buf.clear();
            line_len = 0;

            // Throttled debug logging to avoid flooding and thread contention
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time).count() >= 500) {
                LOG_DEBUG("Serial RX: [%s]", line.c_str());
                last_log_time = now;
            }

            if (handle_prompt(line)) continue;

            if (line.find("PS=") != std::string::npos) {
                pending_status = std::move(line);
            } else if (!pending_status.empty() &&
                       line.find("TargetV=") != std::string::npos) {
                PwrGateSnapshot snap;
                if (pwrgate::parse(pending_status, line, snap) && cb_) {
                    cb_(snap);
                    // If the device was reset by its jumper (target_a reverts to
                    // default 1A), reconfigure it via the 'S' settings menu.
                    if (!reconfigure_sent && snap.target_a < 9.9) {
                        LOG_INFO("Serial: target_a=%.2fA, sending 'S' to reconfigure",
                                 snap.target_a);
                        (void)write(fd_, "S\r", 2);
                        reconfigure_sent = true;
                    }
                }
                pending_status.clear();
            }
        }

    }
}
