#pragma once
#include <chrono>
#include <string>

#include <memory>

// EpicPowerGate R2 (West Mountain Radio) status snapshot.
// Telemetry arrives at ~1 Hz on USB CDC serial (/dev/ttyACM0, 115200 8N1).
// USB interface stays alive on battery power even when PS is absent.
//
// Charger state machine (confirmed via reverse-engineering 2026-03-12/13):
//   "Chrg Off"  PS present, charger intentionally idle (bat full, idle timer, etc.)
//   "Charging"  CC-CV charge cycle active; PSS=1, current flowing, PWM ~600-970
//   "Charged"   Taper complete, current <= stop_a; PSS=0
//   "PS Off"    PS input absent (ps_v == 0.00); telemetry continues on battery
struct PwrGateSnapshot {
    std::chrono::system_clock::time_point timestamp;
    bool valid{false};

    std::shared_ptr<std::string> state; // "Chrg Off" | "Charging" | "Charged" | "PS Off"
    float ps_v{0.0f};       // Power-supply input voltage (0.00 when PS absent)
    float bat_v{0.0f};      // Battery voltage (charger-measured)
    float bat_a{0.0f};      // Charge current (A); positive = charging into battery
    float sol_v{0.0f};      // Solar panel input voltage (0.00 if no panel)
    int    minutes{0};      // State timer: Chrg Off=wall-clock MM, Charging/Charged=elapsed min,
                            //              PS Off=elapsed min since PS lost
    int    pwm{0};          // PWM duty counter: 0=off, ~600-970=actively charging
    int    adc{0};          // Current-sense ADC (~47 counts/A at 1A); 0 when not charging,
                            //   blank/0 when PS absent
    float target_v{0.0f};   // Target (absorption) voltage (from extended 'g' telemetry line)
    float target_a{0.0f};   // Max charge current
    float stop_a{0.0f};     // Tail-current threshold (charge terminates when bat_a <= stop_a)
    int    temp{0};         // Temperature sensor (°F; observed ~74°F at room temp)
    int    pss{0};          // Charge-cycle active: 1=CC-CV running, 0=idle/charged, absent=PS Off
};

namespace pwrgate {
// parse status + target lines into snap
bool parse(const std::string& status_line,
           const std::string& target_line,
           PwrGateSnapshot& snap);
} // namespace pwrgate
