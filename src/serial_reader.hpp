#pragma once
#include "pwrgate.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

// Free functions extracted for unit-testability.
namespace serial_prompts {

// If `line` is a prompt (ends with ':' or '?' after trimming trailing
// whitespace), returns the bytes to write back to the device.  Returns nullptr
// if the line is not a prompt.
const char* prompt_response(const std::string& line);

// Returns true when `partial` (an unterminated line still accumulating in the
// read buffer) looks like an 'S'-menu prompt waiting for a reply.  The
// heuristic: after stripping trailing whitespace the buffer must end with ">:"
// or ">?" — the '>' comes from the "<default>" field printed before every
// settings prompt.
bool is_unterminated_prompt(const std::string& partial);

// Returns true when the device needs to be reconfigured via the 'S' menu
// (i.e. target_a is below the desired 10 A set-point).
inline bool needs_reconfigure(double target_a) { return target_a < 9.9; }

} // namespace serial_prompts

// EpicPowerGate 2 serial reader, background thread
class SerialReader {
public:
    using SnapCb     = std::function<void(const PwrGateSnapshot&)>;
    using RecoveryCb = std::function<void(const std::string& reason)>;

    SerialReader(std::string device, int baud = 115200);
    ~SerialReader();

    bool start();
    void stop();

    void set_callback(SnapCb cb)         { cb_          = std::move(cb); }
    void set_recovery_callback(RecoveryCb cb) { recovery_cb_ = std::move(cb); }

    bool        connected()    const { return fd_ >= 0; }
    const std::string& device() const { return device_; }

    // Recovery stats (reset on stop/start)
    int  charger_recovery_count()  const { return charger_recovery_count_.load(); }
    int  reconnect_count()         const { return reconnect_count_.load(); }

private:
    void process_charger_logic(const PwrGateSnapshot& snap,
                               std::chrono::steady_clock::time_point now);

    std::string       device_;
    int               baud_;
    int               fd_{-1};
    std::atomic<bool> running_{false};
    std::thread       thread_;
    SnapCb            cb_;
    RecoveryCb        recovery_cb_;

    std::atomic<int>  charger_recovery_count_{0};
    std::atomic<int>  reconnect_count_{0};

    // State for process_charger_logic
    bool reconfigure_sent_{false};
    bool charger_was_stuck_{false};
    std::chrono::steady_clock::time_point charger_stuck_since_;
    std::chrono::steady_clock::time_point last_charger_recovery_;

    // Last seen target values to persist when status updates omit them
    double last_target_v_{0.0};
    double last_target_a_{0.0};
    double last_stop_a_{0.0};
    int    last_temp_{0};
    int    last_pss_{0};

    static constexpr long STUCK_TIMEOUT_S    = 900;  // 15 min before first recovery
    static constexpr long RECOVERY_COOLDOWN_S = 1800; // 30 min between attempts
    static constexpr long STALE_RECONNECT_S  = 300;  // 5 min without any data → reconnect
    // After this many serial reconnects still produce no data, escalate to USB reset
    static constexpr int  USB_RESET_AFTER_RECONNECTS = 2;

    int  open_port();
    void read_loop();
};
