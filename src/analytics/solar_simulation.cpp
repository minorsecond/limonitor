#include "solar_simulation.hpp"
#include <chrono>
#include <cmath>
#include <ctime>

SolarSimulator::SolarSimulator(const SolarSimConfig& cfg) : cfg_(cfg) {}

void SolarSimulator::set_config(const SolarSimConfig& cfg) {
    cfg_ = cfg;
}

double SolarSimulator::peak_sun_hours(int month) {
    // Approximate peak sun hours for Denver/Colorado (80112)
    static const double psh[] = {
        4.5, 5.0, 5.8, 6.2, 6.5, 7.0, 6.8, 6.5, 6.0, 5.2, 4.5, 4.2
    };
    if (month >= 1 && month <= 12) return psh[month - 1];
    return 5.5;
}

SolarSimResult SolarSimulator::compute(double current_soc_pct, double cloud_cover,
                                       double max_charge_a, double battery_voltage,
                                       double nominal_ah) const {
    SolarSimResult r;
    r.panel_watts = cfg_.panel_watts;

    if (!cfg_.enabled) {
        r.battery_projection = "—";
        return r;
    }

    std::time_t now = std::time(nullptr);
    struct tm tm_buf{};
    localtime_r(&now, &tm_buf);
    int month = tm_buf.tm_mon + 1;

    double psh = peak_sun_hours(month);
    double wh = cfg_.panel_watts * psh * cfg_.system_efficiency;

    if (cloud_cover >= 0 && cloud_cover <= 1) {
        wh *= (1.0 - cloud_cover);
    }
    r.expected_today_wh = wh;
    r.cloud_adjusted_wh = wh;

    // Use charger max current to cap effective charge rate (W)
    double max_a = max_charge_a > 0 ? max_charge_a : 10.0;
    double v = battery_voltage > 1 ? battery_voltage : 51.2;
    double cap_ah = nominal_ah > 0 ? nominal_ah : 100.0;
    double max_charge_w = max_a * v;

    // Cap expected generation by what charger can absorb over sun hours
    double charger_capacity_wh = max_charge_w * psh;
    double effective_wh = std::min(wh, charger_capacity_wh);

    // Energy needed to reach 100% from current SoC (Wh)
    double energy_to_full_wh = (100.0 - current_soc_pct) / 100.0 * cap_ah * v;

    if (effective_wh >= energy_to_full_wh && current_soc_pct > 20)
        r.battery_projection = "100%";
    else if (effective_wh >= energy_to_full_wh * 0.8)
        r.battery_projection = "90–100%";
    else if (effective_wh >= energy_to_full_wh * 0.5 || effective_wh > 1000)
        r.battery_projection = "70–90%";
    else if (effective_wh > 500)
        r.battery_projection = "50–70%";
    else
        r.battery_projection = "<50%";

    return r;
}
