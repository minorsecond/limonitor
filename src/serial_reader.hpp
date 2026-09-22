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

    SerialReader(std::string device, int baud = 115200, bool read_only = true);
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
    // When true, never proactively write to the PWRgate serial menu (no 'S' to
    // reconfigure or to restart a stuck charge cycle). Set from cfg.serial_readonly;
    // defaults true. The device is charge-configured in hardware (jumpers out) or
    // once over USB, so limonitor only needs to read. It will still respond to a
    // menu prompt to escape back to streaming if it ever finds the unit in one.
    bool read_only_{true};
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

    // Silence recovery, cheapest first.
    //
    // A silent PwrGate is usually not a broken link. It is the device sitting in
    // its settings menu waiting for a keystroke, which it will do forever. A
    // bare CR answers the prompt and costs nothing if the device is simply idle,
    // so try that well before reopening ports or resetting USB.
    static constexpr long NUDGE_AFTER_S      = 20;   // silent this long → send CR
    static constexpr long NUDGE_INTERVAL_S   = 15;   // and again this often
    static constexpr int  NUDGES_BEFORE_RECONNECT = 4;

    static constexpr long STALE_RECONNECT_S  = 90;   // nudges failed → reconnect
    // After this many serial reconnects still produce no data, escalate to USB reset
    static constexpr int  USB_RESET_AFTER_RECONNECTS = 2;

    int  open_port();
    void read_loop();
};
