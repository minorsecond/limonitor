#pragma once
#include <chrono>
#include <string>

#include <memory>

// EpicPowerGate 2 status snapshot
struct PwrGateSnapshot {
    std::chrono::system_clock::time_point timestamp;
    bool valid{false};

    std::shared_ptr<std::string> state; // Shared string for memory efficiency
    float ps_v{0.0f};       // Power-supply / charger input voltage
    float bat_v{0.0f};      // Battery voltage (charger-measured)
    float bat_a{0.0f};      // Charge current (A)
    float sol_v{0.0f};      // Solar panel voltage
    int    minutes{0};      // Elapsed charge time (min)
    int    pwm{0};          // PWM duty cycle (0-1023)
    int    adc{0};          // ADC raw reading
    float target_v{0.0f};   // Target (absorption) voltage
    float target_a{0.0f};   // Max charge current
    float stop_a{0.0f};     // Tail-current threshold (charge stop)
    int    temp{0};         // Temperature sensor raw
    int    pss{0};          // Power-supply-state flag
};

namespace pwrgate {
// parse status + target lines into snap
bool parse(const std::string& status_line,
           const std::string& target_line,
           PwrGateSnapshot& snap);
} // namespace pwrgate
