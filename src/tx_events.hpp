#pragma once
#include <chrono>
#include <cstddef>

// Default TX detection threshold in amps (net positive battery current = discharging).
// Override at runtime via config key tx_threshold or CLI flag --tx-threshold.
// A value of 1.0 works well for 25–50 W VHF/UHF radios drawing 1.5–3 A net.
// Raise to 4–6 A for high-power HF rigs (100 W+).
static constexpr double TX_THRESHOLD_A = 1.0;

struct TxEvent {
    double start_time;   // Unix timestamp
    double end_time;     // Unix timestamp
    double duration;     // seconds
    double peak_current; // A
    double peak_power;   // W (peak_current * battery_voltage at peak)
};
