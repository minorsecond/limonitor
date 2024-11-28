#pragma once
#include <chrono>
#include <string>

// Snapshot of one EpicPowerGate 2 status tick (two serial lines).
struct PwrGateSnapshot {
    std::chrono::system_clock::time_point timestamp;
    bool valid{false};

    std::string state;      // "Charging", "Float", "Idle", ...
    double ps_v{0.0};       // Power-supply / charger input voltage
    double bat_v{0.0};      // Battery voltage (charger-measured)
    double bat_a{0.0};      // Charge current (A)
    double sol_v{0.0};      // Solar panel voltage
    int    minutes{0};      // Elapsed charge time (min)
    int    pwm{0};          // PWM duty cycle (0-1023)
    int    adc{0};          // ADC raw reading
    double target_v{0.0};   // Target (absorption) voltage
    double target_a{0.0};   // Max charge current
    double stop_a{0.0};     // Tail-current threshold (charge stop)
    int    temp{0};         // Temperature sensor raw
    int    pss{0};          // Power-supply-state flag
};

namespace pwrgate {
// Parse a matched pair of serial lines into snap.
// Line 1 (status):  " Charging  PS=13.86V Bat=13.08V,  5.36A  Sol= 0.04V ..."
// Line 2 (targets): "TargetV=14.59V  TargetI=10.00A   Stop= 0.15A  Temp=95  PSS=1"
// Returns true on success.
bool parse(const std::string& status_line,
           const std::string& target_line,
           PwrGateSnapshot& snap);
} // namespace pwrgate
