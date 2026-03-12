#include "serial_reader.hpp"
#include "logger.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <chrono>
#include <unistd.h>
#ifdef __linux__
#include <linux/usbdevice_fs.h>
#include <cstdlib>   // realpath
#include <climits>   // PATH_MAX
#endif

// ---------------------------------------------------------------------------
// USB device reset (Linux only)
// Walks sysfs to find the USB device behind a tty node and issues
// USBDEVFS_RESET — equivalent to physically unplugging and replugging.
// ---------------------------------------------------------------------------

#ifdef __linux__
static bool usb_reset_tty_device(const std::string& dev) {
    // Extract bare tty name: "/dev/ttyACM0" -> "ttyACM0"
    std::string ttyname = dev;
    auto slash = dev.rfind('/');
    if (slash != std::string::npos) ttyname = dev.substr(slash + 1);

    // /sys/class/tty/<name>/device -> symlink to USB interface directory
    std::string sysfs = "/sys/class/tty/" + ttyname + "/device";
    char link[512];
    ssize_t len = readlink(sysfs.c_str(), link, sizeof(link) - 1);
    if (len < 0) {
        LOG_WARN("USB reset: readlink(%s): %s", sysfs.c_str(), strerror(errno));
        return false;
    }
    link[len] = '\0';

    // Resolve symlink relative to /sys/class/tty/<name>/
    std::string base = "/sys/class/tty/" + ttyname + "/";
    std::string iface = (link[0] == '/') ? link : base + link;
    char real[PATH_MAX];
    if (!realpath(iface.c_str(), real)) {
        LOG_WARN("USB reset: realpath(%s): %s", iface.c_str(), strerror(errno));
        return false;
    }

    // Parent of the interface directory = USB device directory
    std::string usb_dir = real;
    auto p = usb_dir.rfind('/');
    if (p == std::string::npos) return false;
    usb_dir = usb_dir.substr(0, p);

    // Read busnum / devnum from sysfs
    auto read_int = [](const std::string& path) -> int {
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return -1;
        int v = -1;
        fscanf(f, "%d", &v);
        fclose(f);
        return v;
    };
    int busnum = read_int(usb_dir + "/busnum");
    int devnum = read_int(usb_dir + "/devnum");
    if (busnum < 0 || devnum < 0) {
        LOG_WARN("USB reset: could not read busnum/devnum from %s", usb_dir.c_str());
        return false;
    }

    // Open /dev/bus/usb/NNN/NNN and issue reset ioctl
    char usbpath[64];
    snprintf(usbpath, sizeof(usbpath), "/dev/bus/usb/%03d/%03d", busnum, devnum);
    int fd = open(usbpath, O_WRONLY);
    if (fd < 0) {
        LOG_WARN("USB reset: open(%s): %s", usbpath, strerror(errno));
        return false;
    }
    int rc = ioctl(fd, USBDEVFS_RESET, 0);
    close(fd);
    if (rc < 0) {
        LOG_WARN("USB reset: USBDEVFS_RESET on %s: %s", usbpath, strerror(errno));
        return false;
    }
    LOG_INFO("USB reset: issued reset to %s (bus%03d dev%03d)", dev.c_str(), busnum, devnum);
    return true;
}
#else
static bool usb_reset_tty_device(const std::string& dev) {
    LOG_WARN("USB reset: not supported on this platform (%s)", dev.c_str());
    return false;
}
#endif

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

void SerialReader::process_charger_logic(const PwrGateSnapshot& snap,
                                           std::chrono::steady_clock::time_point now) {
    // ── Charger reconfiguration (jumper reset) ───────────────
    if (!reconfigure_sent_ && serial_prompts::needs_reconfigure(snap.target_a)) {
        LOG_INFO("Serial: target_a=%.2fA, sending 'S' to reconfigure",
                 snap.target_a);
        (void)write(fd_, "S\r", 2);
        reconfigure_sent_ = true;
    }

    // ── Charger stuck watchdog ───────────────────────────────
    // Stuck = PSU present, battery not near full, charger PWM=0,
    // no charge current.  After STUCK_TIMEOUT_S we enter the
    // settings menu which forces a charge-cycle restart on exit.
    bool psu_present   = snap.ps_v > 12.5;
    bool batt_not_full = (snap.target_v > 0.1)
                         ? snap.bat_v < snap.target_v - 0.30
                         : snap.bat_v < 14.0;
    bool charger_off   = std::abs(snap.bat_a) < 0.05 && snap.pwm < 5;
    bool stuck = psu_present && batt_not_full && charger_off;

    if (stuck) {
        if (!charger_was_stuck_) {
            charger_stuck_since_ = now;
            charger_was_stuck_ = true;
            LOG_DEBUG("Serial: charger idle with PSU %.2fV bat %.2fV — monitoring",
                      snap.ps_v, snap.bat_v);
        }
        long stuck_s = std::chrono::duration_cast<std::chrono::seconds>(
            now - charger_stuck_since_).count();
        long since_rec = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_charger_recovery_).count();

        if (stuck_s >= STUCK_TIMEOUT_S && since_rec >= RECOVERY_COOLDOWN_S) {
            ++charger_recovery_count_;
            std::string reason = "Charger idle for " + std::to_string(stuck_s) +
                "s (PSU " + std::to_string(static_cast<int>(snap.ps_v * 100) / 100.0) +
                "V, bat " + std::to_string(static_cast<int>(snap.bat_v * 100) / 100.0) +
                "V, PWM=0) — recovery #" + std::to_string(charger_recovery_count_.load());
            LOG_WARN("Serial: %s — sending 'S' to restart charge cycle",
                     reason.c_str());
            (void)write(fd_, "S\r", 2);
            last_charger_recovery_ = now;
            charger_stuck_since_   = now; // reset so next window is another STUCK_TIMEOUT_S
            if (recovery_cb_) recovery_cb_(reason);
        }
    } else {
        if (charger_was_stuck_) {
            long stuck_s = std::chrono::duration_cast<std::chrono::seconds>(
                now - charger_stuck_since_).count();
            LOG_INFO("Serial: charger resumed after %lds (bat_a=%.2fA PWM=%d)",
                     stuck_s, snap.bat_a, snap.pwm);
        }
        charger_was_stuck_ = false;
    }
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
    auto last_log_time        = std::chrono::steady_clock::now();
    auto last_snap_time       = std::chrono::steady_clock::now(); // optimistic: assume fresh at start
    int  consecutive_errors   = 0;

    // ── Charger stuck watchdog state ────────────────────────────────────────
    // "Stuck" = PSU present but charger not running and battery not full.
    // After STUCK_TIMEOUT_S we send 'S\r' to enter settings menu; the
    // auto-responder (handle_prompt) walks through all prompts which forces
    // the device to restart its charge cycle on exit.

    auto last_stale_reconnect      = std::chrono::steady_clock::time_point{};
    int  stale_reconnects_since_data = 0; // escalation counter

    // Helper: close and reopen the port, re-send DTR wake pulse
    auto reconnect = [&]() -> bool {
        LOG_INFO("Serial: reconnecting to %s...", device_.c_str());
        close(fd_);
        std::this_thread::sleep_for(std::chrono::seconds(5));
        fd_ = open_port();
        if (fd_ < 0) {
            LOG_WARN("Serial: reconnect to %s failed, retrying in 30s", device_.c_str());
            std::this_thread::sleep_for(std::chrono::seconds(30));
            return false;
        }
        // Re-send DTR wake pulse on reconnect
        int modem = 0;
        ioctl(fd_, TIOCMGET, &modem);
        modem &= ~(TIOCM_DTR | TIOCM_RTS);
        ioctl(fd_, TIOCMSET, &modem);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        tcflush(fd_, TCIFLUSH);
        modem |= (TIOCM_DTR | TIOCM_RTS);
        ioctl(fd_, TIOCMSET, &modem);
        consecutive_errors = 0;
        reconfigure_sent_ = false;
        charger_was_stuck_ = false;
        line_buf.clear();
        line_len = 0;
        pending_status.clear();
        ++reconnect_count_;
        LOG_INFO("Serial: reconnected to %s (reconnect #%d)", device_.c_str(), reconnect_count_.load());
        return true;
    };

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
            LOG_WARN("Serial: read error on %s: %s", device_.c_str(), strerror(errno));
            if (++consecutive_errors >= 5) reconnect();
            else std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        consecutive_errors = 0;
        if (nr == 0) {
            // Device stopped sending. If we have an unterminated 'S'-menu prompt
            // in line_buf (these never end with '\n'), respond to it now.
            if (!line_buf.empty() && serial_prompts::is_unterminated_prompt(line_buf)) {
                if (handle_prompt(line_buf)) {
                    line_buf.clear();
                    line_len = 0;
                }
            }

            // ── Stale data watchdog ──────────────────────────────────────────
            // VTIME=10 gives ~1s read timeout, so this runs roughly each second.
            {
                auto now_w = std::chrono::steady_clock::now();
                auto stale_s = std::chrono::duration_cast<std::chrono::seconds>(
                    now_w - last_snap_time).count();
                auto since_reconnect = std::chrono::duration_cast<std::chrono::seconds>(
                    now_w - last_stale_reconnect).count();
                if (stale_s >= STALE_RECONNECT_S && since_reconnect >= STALE_RECONNECT_S) {
                    last_stale_reconnect = now_w;
                    last_snap_time = now_w; // prevent rapid retrigger on reconnect failure

                    if (stale_reconnects_since_data >= USB_RESET_AFTER_RECONNECTS) {
                        // Serial reconnects haven't helped — escalate to USB device reset
                        LOG_WARN("Serial: %d serial reconnects, still no data — issuing USB reset on %s",
                                 stale_reconnects_since_data, device_.c_str());
                        if (fd_ >= 0) { close(fd_); fd_ = -1; }
                        bool ok = usb_reset_tty_device(device_);
                        if (ok) {
                            // Allow 3s for the OS to re-enumerate the device
                            std::this_thread::sleep_for(std::chrono::seconds(3));
                            stale_reconnects_since_data = 0;
                            std::string reason = "USB reset after " +
                                std::to_string(stale_s) + "s stale data";
                            if (recovery_cb_) recovery_cb_(reason);
                        }
                        reconnect(); // re-open port regardless
                    } else {
                        LOG_WARN("Serial: no PwrGate data from %s for %llds — reconnecting (attempt %d)",
                                 device_.c_str(), static_cast<long long>(stale_s),
                                 stale_reconnects_since_data + 1);
                        reconnect();
                        ++stale_reconnects_since_data;
                        if (recovery_cb_)
                            recovery_cb_("No data for " +
                                std::to_string(static_cast<long long>(stale_s)) + "s — reconnected");
                    }
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
                // Check if this line ALSO has TargetV= (single-line status)
                if (line.find("TargetV=") != std::string::npos) {
                    PwrGateSnapshot snap;
                    if (pwrgate::parse(line, line, snap) && cb_) {
                        cb_(snap);
                        last_snap_time = std::chrono::steady_clock::now();
                        stale_reconnects_since_data = 0;
                        process_charger_logic(snap, last_snap_time);
                    }
                    pending_status.clear();
                } else {
                    pending_status = std::move(line);
                }
            } else if (!pending_status.empty() &&
                       line.find("TargetV=") != std::string::npos) {
                PwrGateSnapshot snap;
                if (pwrgate::parse(pending_status, line, snap) && cb_) {
                    cb_(snap);
                    auto now_s = std::chrono::steady_clock::now();
                    last_snap_time = now_s;
                    stale_reconnects_since_data = 0; // data is flowing, reset escalation counter
                    process_charger_logic(snap, now_s);
                }
                pending_status.clear();
            }
        }

    }
}
