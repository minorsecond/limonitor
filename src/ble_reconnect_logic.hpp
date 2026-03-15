#pragma once
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Pure BLE reconnect/matching logic — no GLib dependency, fully unit-testable
// ---------------------------------------------------------------------------

// Case-insensitive substring search: returns true if `hay` contains `needle`.
inline bool ble_str_icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
        [](char a, char b){
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != hay.end();
}

// Exact case-insensitive MAC address match.
inline bool ble_is_address_match(const std::string& device_addr, const std::string& target) {
    if (device_addr.empty() || target.empty()) return false;
    return ::strcasecmp(device_addr.c_str(), target.c_str()) == 0;
}

// Case-insensitive bidirectional name match.
// Checks both directions so that a truncated advertisement name (shorter than
// the stored target) still matches — e.g., advertised "L-12100BNNA70" matches
// stored target "L-12100BNNA70-B06374".
inline bool ble_is_name_match(const std::string& device_name, const std::string& target) {
    if (device_name.empty() || target.empty()) return false;
    return ble_str_icontains(device_name, target) || ble_str_icontains(target, device_name);
}

// ---------------------------------------------------------------------------
// Reconnect / hard-reset escalation state
// Pure counter logic; thresholds expressed as named constants.
// ---------------------------------------------------------------------------
struct BleReconnectState {
    static constexpr int SCAN_STUCK_TIMEOUT_S   = 90;
    static constexpr int CONNECT_FAIL_THRESHOLD = 3;
    static constexpr int POWER_CYCLE_THRESHOLD  = 4;

    int connect_fail_count{0};
    int hard_reset_count{0};

    // Call on each failed Device.Connect().
    // Returns true when the threshold is reached and a hard reset should fire.
    bool on_connect_failure() {
        return ++connect_fail_count >= CONNECT_FAIL_THRESHOLD;
    }

    // Call at the start of a hard-reset sequence.
    // Clears connect_fail_count, increments hard_reset_count.
    // Returns true when the power-cycle threshold is reached.
    bool begin_hard_reset() {
        connect_fail_count = 0;
        ++hard_reset_count;
        return hard_reset_count >= POWER_CYCLE_THRESHOLD;
    }

    // Call after a successful adapter power-cycle so escalation restarts.
    void on_power_cycled() {
        hard_reset_count = 0;
    }

    // Call when the connection fully succeeds (StartNotify OK).
    void on_connect_success() {
        connect_fail_count = 0;
        hard_reset_count   = 0;
    }
};
