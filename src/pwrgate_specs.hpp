#pragma once
// Epic PWRgate 2 specifications and LED state reference.
// Source: West Mountain Radio Epic PWRgate manual (www.westmountainradio.com)

namespace pwrgate_specs {

// ── Electrical limits ────────────────────────────────────────────────────────
constexpr double MAX_OUTPUT_CURRENT_A  = 40.0;   // continuous
constexpr double MAX_CHARGE_CURRENT_A  = 10.0;
constexpr double MAX_PSU_VOLTAGE_V     = 16.0;
constexpr double MAX_SOLAR_VOLTAGE_V   = 30.0;
constexpr double VOLTAGE_DROP_V        = 0.05;   // across unit at full load
constexpr double MAX_OPERATING_TEMP_F  = 110.0;

// ── Recommended PSU output voltage per battery type ─────────────────────────
constexpr double PSU_V_GEL     = 13.9;
constexpr double PSU_V_AGM     = 14.3;
constexpr double PSU_V_LIFEPO4 = 14.5;

// ── Default charge parameters ────────────────────────────────────────────────
struct ChargeDefaults {
    const char* battery_type;
    double charge_v;    // absorption voltage (V)
    double stop_a;      // tail-current threshold (A)
    double trickle_a;   // trickle/float current (A)
    double recharge_v;  // recharge trigger voltage (V)
    int    max_time_h;  // maximum charge time (hours)
    int    retry_h;     // retry after abort (hours)
};
constexpr ChargeDefaults CHARGE_DEFAULTS[] = {
    { "Gel",     13.85, 0.25, 0.15, 12.1, 25, 4 },
    { "AGM",     14.40, 0.25, 0.15, 12.1, 25, 4 },
    { "LiFePO4", 14.50, 0.50, 0.00, 12.6, 25, 4 },
};

// ── LED state descriptions ────────────────────────────────────────────────────
// Power Supply LED
// Green          : power supply good and active
// Red            : low voltage or internal fault
// Off            : no power supply detected
//
// Battery LED
// Green solid    : powering load from battery (PSU absent or below battery voltage)
// Green quick flash : battery detected but charger off
// Green flash    : warning — battery < 12 V
// Red flash      : warning — battery < 11.7 V
// Red solid      : battery bad or charger damaged
// Blue solid     : battery fully charged
// Blue flash     : battery charging
// Blue periodic flicker : trickle charging
// Red/Blue alt   : battery < 11.7 V while solar charging
// Green/Red alt  : charging stopped due to temperature limit
// Off            : no battery detected
//
// Solar LED
// Green solid    : solar voltage good and charging
// Green flash    : solar voltage present but not in use

// Interpret Battery LED meaning from a PwrGateSnapshot.
// Returns a human-readable string.
inline const char* battery_led_meaning(double bat_v, double bat_a,
                                        double target_v, int pwm,
                                        bool psu_present) {
    if (bat_v <= 0.1)       return "No battery detected";
    if (bat_v < 11.7)       return "Critical: battery < 11.7V";
    if (bat_v < 12.0)       return "Warning: battery < 12V";
    if (bat_a > 0.05) {
        if (pwm < 50)       return "Trickle charging";
        if (bat_v >= target_v - 0.10) return "Float / absorption charging";
        return "Bulk charging";
    }
    if (!psu_present)       return "Powering load from battery";
    return "Battery standby (fully charged or charger off)";
}

// ── Suspend button behavior ───────────────────────────────────────────────────
// Short press (charging)         : suspend for 30 min
// Short press (suspended)        : resume charging
// Hold 5s (charging)             : terminate charge cycle
// Hold 5s (idle)                 : start new charge cycle
// Short press (no PSU/solar)     : turn off LEDs

} // namespace pwrgate_specs
