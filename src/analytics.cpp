#include "analytics.hpp"
#include "battery_data.hpp"
#include "pwrgate.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

int AnalyticsEngine::unix_day(double ts) {
    return static_cast<int>(ts / 86400.0);
}

void AnalyticsEngine::reset_daily_if_needed(int day) {
    if (current_day_ == day) return;
    // Archive completed day's energy for weekly rollup
    if (current_day_ >= 0) {
        double day_wh = energy_discharged_wh_;
        if (energy_days_count_ < 8) {
            energy_by_day_[energy_days_count_] = day_wh;
            days_by_day_[energy_days_count_] = current_day_;
            ++energy_days_count_;
        } else {
            for (size_t i = 0; i < 7; ++i) {
                energy_by_day_[i] = energy_by_day_[i + 1];
                days_by_day_[i] = days_by_day_[i + 1];
            }
            energy_by_day_[7] = day_wh;
            days_by_day_[7] = current_day_;
        }
    }
    current_day_ = day;
    energy_charged_wh_    = 0;
    energy_discharged_wh_ = 0;
    solar_energy_wh_      = 0;
    max_soc_  = 0;
    min_soc_  = 100;
    soc_init_ = false;
    charger_runtime_sec_  = 0;
    charger_runtime_day_  = day;
    prev_chg_minutes_     = -1;
}

void AnalyticsEngine::push_load_sample(double watts) {
    load_ring_[load_head_] = watts;
    load_head_ = (load_head_ + 1) % LOAD_N;
    if (load_count_ < LOAD_N) ++load_count_;
}

void AnalyticsEngine::recompute_load_stats() {
    if (load_count_ == 0) return;
    double sum = 0, peak = 0, idle_sum = 0;
    size_t idle_n = 0;
    // Walk backwards from most-recent sample
    for (size_t i = 0; i < load_count_; ++i) {
        double w = load_ring_[i];
        sum += w;
        if (w > peak) peak = w;
        if (w < 15.0) { idle_sum += w; ++idle_n; } // <15 W = idle threshold
    }
    snap_.avg_load_watts  = sum / static_cast<double>(load_count_);
    snap_.peak_load_watts = peak;
    snap_.idle_load_watts = idle_n > 0 ? idle_sum / static_cast<double>(idle_n)
                                       : snap_.avg_load_watts;
}

// Battery age, health from capacity observations, and replacement estimate.
void AnalyticsEngine::update_health_and_age() {
    if (best_discharge_ah_ > 1.0 && rated_capacity_ah_ > 0) {
        snap_.usable_capacity_ah  = best_discharge_ah_;
        snap_.rated_capacity_ah   = rated_capacity_ah_;
        snap_.capacity_health_pct = std::min(100.0,
            best_discharge_ah_ / rated_capacity_ah_ * 100.0);
    } else {
        snap_.usable_capacity_ah  = 0;
        snap_.rated_capacity_ah   = rated_capacity_ah_;
        snap_.capacity_health_pct = -1;
    }

    double age = -1;
    if (!purchase_date_.empty()) {
        int yr = 0, mo = 0, dy = 0;
        if (std::sscanf(purchase_date_.c_str(), "%d-%d-%d", &yr, &mo, &dy) == 3) {
            struct tm t{};
            t.tm_year = yr - 1900;
            t.tm_mon  = mo - 1;
            t.tm_mday = dy;
            time_t tp = std::mktime(&t);
            time_t now = std::time(nullptr);
            if (tp >= 0 && now > tp)
                age = static_cast<double>(now - tp) / (365.25 * 86400.0);
            else
                age = 0;
        }
    }
    snap_.battery_age_years = age;

    // Estimated health — age-based degradation model
    // LiFePO4: ~2.5 % / yr (typically 8-10 yr service life)
    double health = -1;
    if (age >= 0) {
        health = std::max(0.0, 100.0 - age * 2.5);
    }
    // Prefer observed capacity health if available
    if (snap_.capacity_health_pct >= 0) {
        health = snap_.capacity_health_pct;
    }
    snap_.battery_health_pct = health;

    // Replacement estimate
    if (age >= 0 && health >= 0) {
        // Years until health falls below 80 % at current rate
        // Rate: (100 - health) / age pct/yr, or default 2.5 pct/yr
        double rate = (age > 0.1) ? (100.0 - health) / age : 2.5;
        if (rate <= 0) rate = 0.5;
        double years_to_80 = (health - 80.0) / rate;
        // Also cap to maximum lifespan 10 yr
        double years_to_cap = 10.0 - age;
        double yrem = std::min(std::max(0.0, years_to_80),
                               std::max(0.0, years_to_cap));
        snap_.years_remaining_low  = std::floor(std::max(0.0, yrem - 0.5));
        snap_.years_remaining_high = std::ceil(yrem + 0.5);
        snap_.battery_replace_warn = (health < 80.0) || (age > 8.0);
    } else {
        snap_.years_remaining_low  = -1;
        snap_.years_remaining_high = -1;
        snap_.battery_replace_warn = false;
    }
}

void AnalyticsEngine::update_charging_stage(double bat_v, double target_v,
                                            double bat_a,
                                            const std::string& state) {
    if (state != "Charging" && state != "Float") {
        snap_.charging_stage = "Idle";
        return;
    }
    // Float: explicit state or very low current near target voltage
    if (state == "Float" ||
        (target_v > 0 && bat_v >= target_v - 0.05 && bat_a < 0.5)) {
        snap_.charging_stage = "Float";
        return;
    }
    // Absorption: near target AND current is decreasing
    bool near_target = (target_v > 0) && (bat_v >= target_v - 0.15);
    bool cur_falling = (prev_chg_a_ >= 0) && (bat_a < prev_chg_a_ - 0.05);
    if (near_target && cur_falling) {
        snap_.charging_stage = "Absorption";
    } else if (target_v > 0 && bat_v < target_v - 0.15) {
        snap_.charging_stage = "Bulk";
    } else if (near_target) {
        snap_.charging_stage = "Absorption";
    } else {
        snap_.charging_stage = "Bulk";
    }
}

AnalyticsEngine::AnalyticsEngine(double rated_capacity_ah)
    : rated_capacity_ah_(rated_capacity_ah) {
    snap_.rated_capacity_ah = rated_capacity_ah_;
}

void AnalyticsEngine::set_rated_capacity(double ah) {
    if (ah > 0) {
        rated_capacity_ah_   = ah;
        rated_override_      = true;
        snap_.rated_capacity_ah = ah;
        update_health_and_age();
    }
}

void AnalyticsEngine::set_purchase_date(const std::string& d) {
    purchase_date_ = d;
    update_health_and_age();
}

void AnalyticsEngine::push_voltage_sample(double ts, double v) {
    voltage_ring_[voltage_ring_head_] = v;
    voltage_ring_ts_[voltage_ring_head_] = ts;
    voltage_ring_head_ = (voltage_ring_head_ + 1) % VOLTAGE_N;
    if (voltage_ring_count_ < VOLTAGE_N) ++voltage_ring_count_;
}

void AnalyticsEngine::compute_voltage_trend() {
    if (voltage_ring_count_ < 6) {
        snap_.voltage_trend = "—";
        return;
    }
    // Linear regression slope on last N samples
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
    size_t n = voltage_ring_count_;
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (voltage_ring_head_ + VOLTAGE_N - 1 - i) % VOLTAGE_N;
        double x = voltage_ring_ts_[idx];
        double y = voltage_ring_[idx];
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }
    double denom = n * sum_xx - sum_x * sum_x;
    if (std::fabs(denom) < 1e-12) {
        snap_.voltage_trend = "stable";
        return;
    }
    double slope = (n * sum_xy - sum_x * sum_y) / denom;
    // Slope in V/s — threshold ~1e-6 V/s (0.036 V/h) for "trend"
    if (slope > 1e-6)
        snap_.voltage_trend = "up";
    else if (slope < -1e-6)
        snap_.voltage_trend = "down";
    else
        snap_.voltage_trend = "stable";
}

void AnalyticsEngine::compute_extended_analytics(const BatterySnapshot& bat,
                                                const PwrGateSnapshot* chg) {
    snap_.health_alerts.clear();

    // Voltage trend
    compute_voltage_trend();

    // Energy used this week (today + last 6 completed days)
    snap_.energy_used_week_wh = energy_discharged_wh_;
    for (size_t i = 0; i < energy_days_count_ && i < 6; ++i) {
        size_t idx = energy_days_count_ - 1 - i;
        if (idx < energy_days_count_)
            snap_.energy_used_week_wh += energy_by_day_[idx];
    }

    // Charge rate (when charging)
    if (bat.current_a < -0.1 && bat.valid) {
        snap_.charge_rate_w = std::abs(bat.power_w);
        if (bat.nominal_ah > 1.0) {
            double pack_v = bat.total_voltage_v > 1 ? bat.total_voltage_v : 51.2;
            double total_wh = bat.nominal_ah * pack_v;
            if (total_wh > 0)
                snap_.charge_rate_pct_per_h = snap_.charge_rate_w / total_wh * 100.0;
        }
    } else {
        snap_.charge_rate_w = 0;
        snap_.charge_rate_pct_per_h = 0;
    }

    // Battery stress: Low / Moderate / High
    double dod = snap_.depth_of_discharge_pct;
    double max_temp = std::max(snap_.temp1_c, snap_.temp2_c);
    bool high_temp = snap_.temp_valid && max_temp >= 45;
    bool deep_dod = dod >= 50;
    if (high_temp || deep_dod)
        snap_.battery_stress = "high";
    else if (dod >= 30 || (snap_.temp_valid && max_temp >= 40))
        snap_.battery_stress = "moderate";
    else if (dod < 30 && (!snap_.temp_valid || max_temp < 40))
        snap_.battery_stress = "low";
    else
        snap_.battery_stress = "—";

    // Solar readiness
    if (chg && chg->sol_v > 1.0) {
        if (chg->bat_a > 0.3) {
            double pwr = chg->sol_v * chg->bat_a;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Solar harvesting: %.0f W", pwr);
            snap_.solar_readiness = buf;
        } else {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "Solar detected: %.1f V — Waiting for charge conditions",
                          chg->sol_v);
            snap_.solar_readiness = buf;
        }
    } else {
        snap_.solar_readiness = "";
    }

    // Charger abnormal detection
    snap_.charger_abnormal_warning.clear();
    if (chg && chg->valid) {
        bool charging = (chg->state == "Charging" || chg->state == "Float");
        if (charging && chg->bat_a < 0.3 && chg->target_a > 2.0) {
            chg_low_current_count_++;
            if (chg_low_current_count_ >= 3)
                snap_.charger_abnormal_warning = "Charging but current extremely low";
        } else {
            chg_low_current_count_ = 0;
        }
        if (prev_chg_bat_v_ >= 0 && charging) {
            double delta = std::abs(chg->bat_v - prev_chg_bat_v_);
            chg_bat_v_variance_ = chg_bat_v_variance_ * 0.8 + delta * 0.2;
            if (chg_bat_v_variance_ > 0.5)
                snap_.charger_abnormal_warning = "Voltage unstable";
        }
        prev_chg_bat_v_ = chg->bat_v;
    } else {
        prev_chg_bat_v_ = -1;
        chg_bat_v_variance_ = 0;
    }

    // Health alerts
    if (snap_.cell_balance_status == "Imbalance" || snap_.cell_balance_status == "Warning")
        snap_.health_alerts.push_back("Cell imbalance warning");
    if (snap_.temp_status == "Warning" || snap_.temp_status == "Warm")
        snap_.health_alerts.push_back("High battery temperature");
    if (snap_.efficiency_valid && snap_.charger_efficiency >= 0 && snap_.charger_efficiency < 0.75)
        snap_.health_alerts.push_back("Low charger efficiency");
    if (snap_.voltage_trend == "down" && bat.soc_pct < 20)
        snap_.health_alerts.push_back("Abnormal voltage behavior");
    if (!snap_.charger_abnormal_warning.empty())
        snap_.health_alerts.push_back(snap_.charger_abnormal_warning);

    compute_system_status(bat, chg);
}

void AnalyticsEngine::compute_system_status(const BatterySnapshot& bat,
                                            const PwrGateSnapshot* /*chg*/) {
    // Aggregate status from existing metrics
    if (!bat.valid) {
        snap_.system_status = "Waiting for BMS data";
        return;
    }
    std::string status;
    bool charging = bat.current_a < -0.1;
    if (charging) {
        status = "Charging normally";
    } else if (bat.current_a > 0.1) {
        status = "Discharging";
    } else {
        status = "Idle";
    }
    if (snap_.battery_health_pct >= 0) {
        const char* health_str = snap_.battery_health_pct >= 90 ? "Good" :
                                snap_.battery_health_pct >= 70 ? "Fair" : "Poor";
        status += " · Battery health: " + std::string(health_str);
    }
    if (bat.time_remaining_h > 0 && bat.time_remaining_h < 1000) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0fh", bat.time_remaining_h);
        status += " · Runtime remaining: " + std::string(buf);
    }
    status += " · Cell balance: " + snap_.cell_balance_status;
    status += " · Temperature: " + snap_.temp_status;
    snap_.system_status = status;
}

void AnalyticsEngine::on_battery(const BatterySnapshot& snap, const PwrGateSnapshot* chg) {
    if (!snap.valid) return;

    double ts = static_cast<double>(
        std::chrono::system_clock::to_time_t(snap.timestamp));
    int day = unix_day(ts);
    reset_daily_if_needed(day);

    // Auto-detect rated capacity from BMS unless user overrode it
    if (!rated_override_ && snap.nominal_ah > 1.0) {
        rated_capacity_ah_   = snap.nominal_ah;
        snap_.rated_capacity_ah = snap.nominal_ah;
    }

    if (bat_seen_ && ts > prev_bat_ts_) {
        double dt_h = (ts - prev_bat_ts_) / 3600.0;
        // Skip gaps > 5 min (data outage) to avoid inflating counters
        if (dt_h > 0 && dt_h < (5.0 / 60.0)) {
            double pwr = snap.power_w;
            if (pwr > 0)
                energy_discharged_wh_ += pwr * dt_h;
            else if (pwr < 0)
                energy_charged_wh_ += (-pwr) * dt_h;
        }
    }
    snap_.energy_discharged_today_wh = energy_discharged_wh_;
    snap_.energy_charged_today_wh    = energy_charged_wh_;
    snap_.net_energy_today_wh        = energy_charged_wh_ - energy_discharged_wh_;

    snap_.cell_delta_mv = snap.cell_delta_v * 1000.0;
    double dm = snap_.cell_delta_mv;
    if      (dm <  10) snap_.cell_balance_status = "Excellent";
    else if (dm <  25) snap_.cell_balance_status = "Good";
    else if (dm <  50) snap_.cell_balance_status = "Warning";
    else               snap_.cell_balance_status = "Imbalance";

    snap_.temp_valid = !snap.temperatures_c.empty();
    if (snap_.temp_valid) {
        snap_.temp1_c = snap.temperatures_c[0];
        snap_.temp2_c = snap.temperatures_c.size() > 1
                      ? snap.temperatures_c[1] : snap_.temp1_c;
        double mx = std::max(snap_.temp1_c, snap_.temp2_c);
        if      (mx < 40) snap_.temp_status = "Normal";
        else if (mx < 50) snap_.temp_status = "Warm";
        else              snap_.temp_status = "Warning";
    }

    if (!soc_init_) {
        max_soc_ = snap.soc_pct;
        min_soc_ = snap.soc_pct;
        soc_init_ = true;
    } else {
        if (snap.soc_pct > max_soc_) max_soc_ = snap.soc_pct;
        if (snap.soc_pct < min_soc_) min_soc_ = snap.soc_pct;
    }
    snap_.depth_of_discharge_pct = max_soc_ - min_soc_;
    double dod = snap_.depth_of_discharge_pct;
    if      (dod < 30) snap_.dod_status = "Low stress";
    else if (dod < 70) snap_.dod_status = "Normal";
    else               snap_.dod_status = "High stress";

    double load = (snap.current_a > 0) ? snap.power_w : 0.0;
    push_load_sample(load);
    recompute_load_stats();

    bool discharging = snap.current_a >  0.1;
    bool charging    = snap.current_a < -0.1;
    if (discharging && !was_discharging_) {
        discharge_start_ah_ = snap.remaining_ah;
        was_discharging_     = true;
    } else if (!discharging && was_discharging_) {
        double consumed = discharge_start_ah_ - snap.remaining_ah;
        if (consumed > 5.0 && consumed > best_discharge_ah_) {
            best_discharge_ah_ = consumed;
            update_health_and_age();
        }
        was_discharging_ = false;
    } else if (charging) {
        was_discharging_ = false;
    }

    push_voltage_sample(ts, snap.total_voltage_v);
    compute_extended_analytics(snap, chg);

    prev_bat_ts_ = ts;
    bat_seen_    = true;
}

void AnalyticsEngine::on_charger(const PwrGateSnapshot& snap, const BatterySnapshot* bat) {
    if (!snap.valid) return;

    double ts = static_cast<double>(
        std::chrono::system_clock::to_time_t(snap.timestamp));
    int day = unix_day(ts);
    reset_daily_if_needed(day);

    if (chg_seen_ && ts > prev_chg_ts_ && snap.sol_v > 1.0 && snap.bat_a > 0) {
        double dt_h = (ts - prev_chg_ts_) / 3600.0;
        if (dt_h > 0 && dt_h < (5.0 / 60.0)) {
            // Solar power = panel_v × charge_current (same current flows through PWM)
            double sol_pwr = snap.sol_v * snap.bat_a;
            solar_energy_wh_ += sol_pwr * dt_h;
        }
    }
    snap_.solar_energy_today_wh = solar_energy_wh_;

    update_charging_stage(snap.bat_v, snap.target_v, snap.bat_a, snap.state);
    prev_chg_a_ = snap.bat_a;

    // Efficiency = output power / input power = (bat_v × bat_a) / (ps_v × bat_a)
    //            = bat_v / ps_v   (valid only during active charging from PS)
    if ((snap.state == "Charging" || snap.state == "Float") &&
        snap.bat_a > 0.5 && snap.bat_v > 1.0) {
        double input_v = snap.ps_v > 0.5 ? snap.ps_v
                       : snap.sol_v > 0.5 ? snap.sol_v : 0;
        if (input_v > snap.bat_v) {
            snap_.charger_efficiency = snap.bat_v / input_v;
            snap_.efficiency_valid   = true;
        }
    }

    snap_.solar_voltage_v = snap.sol_v;
    snap_.solar_active    = snap.sol_v > 1.0;
    snap_.solar_power_w   = (snap_.solar_active && snap.bat_a > 0)
                           ? snap.sol_v * snap.bat_a : 0.0;
    snap_.solar_energy_today_wh = solar_energy_wh_;

    // Charger runtime today
    int chg_day = unix_day(ts);
    if (charger_runtime_day_ != chg_day) {
        charger_runtime_sec_ = 0;
        charger_runtime_day_ = chg_day;
        prev_chg_minutes_ = -1;
    }
    if (snap.state == "Charging" || snap.state == "Float") {
        int m = snap.minutes;
        if (prev_chg_minutes_ >= 0) {
            int delta = m - prev_chg_minutes_;
            if (delta >= 0 && delta <= 10)  // sanity: max 10 min per poll
                charger_runtime_sec_ += delta * 60;
        }
        prev_chg_minutes_ = m;
    } else {
        prev_chg_minutes_ = -1;
    }
    snap_.charger_runtime_today_sec = charger_runtime_sec_;

    if (bat)
        compute_extended_analytics(*bat, &snap);

    prev_chg_ts_ = ts;
    chg_seen_    = true;
}
