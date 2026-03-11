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

    // Toggle DTR — some devices (EpicPowerGate) reset/wake on DTR edge
    int modem = 0;
    ioctl(fd, TIOCMGET, &modem);
    modem &= ~(TIOCM_DTR | TIOCM_RTS);
    ioctl(fd, TIOCMSET, &modem);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    modem |= (TIOCM_DTR | TIOCM_RTS);
    ioctl(fd, TIOCMSET, &modem);

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
    std::string line_buf;
    line_buf.reserve(256);
    std::string pending_status;
    char rbuf[1024];
    size_t line_len = 0;
    auto last_log_time = std::chrono::steady_clock::now();

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

            if (!line.empty() && (line.back() == ':' || line.back() == '?')) {
                const char* nl = "\r\n";
                (void)write(fd_, nl, 2);
                continue;
            }

            if (line.find("PS=") != std::string::npos) {
                pending_status = std::move(line);
            } else if (!pending_status.empty() &&
                       line.find("TargetV=") != std::string::npos) {
                PwrGateSnapshot snap;
                if (pwrgate::parse(pending_status, line, snap) && cb_)
                    cb_(snap);
                pending_status.clear();
            }
        }
    }
}
