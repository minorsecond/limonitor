#pragma once
#include <chrono>
#include <cstddef>

static constexpr double TX_THRESHOLD_A = 6.0;
// TX when battery discharge > 6A OR charger output (bat_a) > 6A
// Covers both: battery-only (discharge spike) and PSU charging (charger ramp-up)

struct TxEvent {
    double start_time;   // Unix timestamp
    double end_time;     // Unix timestamp
    double duration;     // seconds
    double peak_current; // A
    double peak_power;   // W (peak_current * battery_voltage at peak)
};
