#pragma once
#include <string>

struct SolarSimConfig {
    bool enabled{false};
    double panel_watts{400.0};
    double system_efficiency{0.75};
    std::string zip_code{"80112"};
};

struct SolarSimResult {
    double panel_watts{0};
    double expected_today_wh{0};
    std::string battery_projection;
    double cloud_adjusted_wh{0};
};

class SolarSimulator {
public:
    explicit SolarSimulator(const SolarSimConfig& cfg);

    void set_config(const SolarSimConfig& cfg);
    const SolarSimConfig& config() const { return cfg_; }

    // cloud_cover 0–1 from weather (optional). If < 0, uses no cloud adjustment.
    // max_charge_a from charger target_a; battery_voltage and nominal_ah for projection.
    SolarSimResult compute(double current_soc_pct, double cloud_cover = -1,
                          double max_charge_a = 0, double battery_voltage = 0,
                          double nominal_ah = 0) const;

private:
    SolarSimConfig cfg_;

    // Peak sun hours by month (approximate for 80112 / Denver area)
    static double peak_sun_hours(int month);
};
