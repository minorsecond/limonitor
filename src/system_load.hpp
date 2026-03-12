#pragma once
#include <string>
#include <vector>

// One TX power level for a transmitting component (e.g. a radio)
// dc_in_a / dc_in_w store the TOTAL SYSTEM draw from the battery during TX.
struct TxLevel {
    std::string label;      // "5W RF", "10W RF", "55W RF"
    double rf_out_w{0};     // RF output power (watts)
    double dc_in_a{0};      // Total system DC amps from battery during TX (measured)
    double dc_in_w{0};      // Total system DC watts from battery during TX (measured)

    // Radio efficiency: RF out / (total_tx_draw - other_system_idle)
    // base_idle_w = total_idle_w of all OTHER components (not this one)
    double efficiency_pct(double base_idle_w = 0.0) const;
};

// A named system component (PwrGate, Pi, radio, …)
struct SystemComponent {
    std::string name;           // "PwrGate EPG2"
    std::string purpose;        // "AC-to-DC PSU and battery charger"
    double idle_a{0};           // Component idle current (amps, individual contribution)
    double idle_w{0};           // Component idle power  (watts, individual contribution)
    std::vector<TxLevel> tx_levels;   // empty = no TX capability
};

// Full system load configuration — a collection of components.
struct SystemLoadConfig {
    std::vector<SystemComponent> components;

    bool empty() const { return components.empty(); }

    double total_idle_w() const;
    double total_idle_a() const;

    // Effective average load given TX duty cycle at one specific TX level.
    //   comp_idx  — index into components[] that has TX levels (-1 = no TX)
    //   level_idx — which TxLevel in that component (0-based)
    // Formula: total_idle_w * (1 - duty/100) + tx_dc_in_w * (duty/100)
    double effective_load_w(double tx_duty_pct,
                            int comp_idx = -1,
                            int level_idx = 0) const;

    // Runtime estimate in receive-only mode (no TX)
    double runtime_receive_h(double usable_wh) const;

    // Runtime estimate with TX duty cycle
    double runtime_with_tx_h(double usable_wh,
                              double tx_duty_pct,
                              int comp_idx,
                              int level_idx) const;

    // JSON round-trip
    std::string to_json() const;
    static SystemLoadConfig from_json(const std::string& json);

    // Factory: pre-populated with user's calibrated measurements
    // PwrGate(0.042A/0.55W) + Pi(0.413A/5.45W) + IC-2100H Radio(0.290A/3.80W)
    // Total idle: 0.745A / 9.80W
    static SystemLoadConfig default_config();
};
