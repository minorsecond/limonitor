#include "analytics.hpp"
#include "battery_data.hpp"
#include "database.hpp"
#include "pwrgate.hpp"
#include "logger.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/statvfs.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

int AnalyticsEngine::unix_day(double ts) {
    time_t tt = static_cast<time_t>(ts);
    struct tm tm_buf {};
    localtime_r(&tt, &tm_buf);
    return tm_buf.tm_year * 1000 + tm_buf.tm_yday;
}

static int local_hour_id(double ts) {
    time_t tt = static_cast<time_t>(ts);
    struct tm tm_buf {};
    localtime_r(&tt, &tm_buf);
    int day = tm_buf.tm_year * 1000 + tm_buf.tm_yday;
    return day * 24 + tm_buf.tm_hour;
}

void AnalyticsEngine::reset_daily_if_needed(int day, double ts) {
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
    last_discharge_integration_ts_ = ts;
}

void AnalyticsEngine::push_charger_power_sample(double watts) {
    chg_pwr_ring_[chg_pwr_head_] = watts;
    chg_pwr_head_ = (chg_pwr_head_ + 1) % CHG_PWR_N;
    if (chg_pwr_count_ < CHG_PWR_N) ++chg_pwr_count_;
}

double AnalyticsEngine::avg_charger_power_w() const {
    if (chg_pwr_count_ == 0) return 0;
    double sum = 0;
    for (size_t i = 0; i < chg_pwr_count_; ++i)
        sum += chg_pwr_ring_[i];
    return sum / static_cast<double>(chg_pwr_count_);
}

void AnalyticsEngine::integrate_discharge_wh(double ts, double power_w) {
    double ref_ts = last_discharge_integration_ts_ >= 0 ? last_discharge_integration_ts_ : ts;
    double dt_h = (ts - ref_ts) / 3600.0;
    if (dt_h > 0 && dt_h < (5.0 / 60.0)) {  // skip gaps > 5 min
        energy_discharged_wh_ += power_w * dt_h;
        discharge_wh_this_hour_ += power_w * dt_h;
    }
    last_discharge_integration_ts_ = ts;
}

void AnalyticsEngine::push_load_sample(double ts, double watts) {
    constexpr double MIN_LOAD_INTERVAL = 2.5;
    if (last_load_push_ts_ >= 0 && (ts - last_load_push_ts_) < MIN_LOAD_INTERVAL)
        return;
    last_load_push_ts_ = ts;
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
    // Treat the charger as idle only when the state is explicitly Idle/unknown
    // AND no current is flowing. This handles firmware variants that omit the
    // state keyword (state inferred in pwrgate::parse) as well as future formats.
    bool current_flowing = bat_a > 0.05;
    bool explicitly_idle = (state != "Charging" && state != "Float");
    if (explicitly_idle && !current_flowing) {
        snap_.charging_stage = "Idle";
        return;
    }

    // Float: explicit state or very low current near target voltage
    if (state == "Float" ||
        (target_v > 0 && bat_v >= target_v - 0.05 && bat_a < 0.5)) {
        snap_.charging_stage = "Float";
        return;
    }
    // Absorption: near target voltage (within 0.15 V)
    bool near_target = (target_v > 0) && (bat_v >= target_v - 0.15);
    bool cur_falling  = (prev_chg_a_ >= 0) && (bat_a < prev_chg_a_ - 0.05);
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

void AnalyticsEngine::set_calibrated_idle_w(double w) {
    calibrated_idle_w_ = w;
}

void AnalyticsEngine::push_voltage_soc_sample(double ts, double v, double soc) {
    if (voltage_ring_count_ == VOLTAGE_N) {
        // Remove oldest sample from sums
        double old_ts = voltage_ring_ts_[voltage_ring_head_];
        double old_v = voltage_ring_[voltage_ring_head_];
        double old_soc = soc_ring_[voltage_ring_head_];
        v_sum_x_ -= old_ts; v_sum_y_ -= old_v;
        v_sum_x2_ -= old_ts * old_ts; v_sum_xy_ -= old_ts * old_v;
        s_sum_x_ -= old_ts; s_sum_y_ -= old_soc;
        s_sum_x2_ -= old_ts * old_ts; s_sum_xy_ -= old_ts * old_soc;
    }

    voltage_ring_[voltage_ring_head_] = v;
    soc_ring_[voltage_ring_head_] = soc;
    voltage_ring_ts_[voltage_ring_head_] = ts;

    // Add new sample to sums
    v_sum_x_ += ts; v_sum_y_ += v;
    v_sum_x2_ += ts * ts; v_sum_xy_ += ts * v;
    s_sum_x_ += ts; s_sum_y_ += soc;
    s_sum_x2_ += ts * ts; s_sum_xy_ += ts * soc;

    voltage_ring_head_ = (voltage_ring_head_ + 1) % VOLTAGE_N;
    if (voltage_ring_count_ < VOLTAGE_N) ++voltage_ring_count_;
}

void AnalyticsEngine::compute_voltage_trend() {
    if (voltage_ring_count_ < 6) {
        snap_.voltage_trend = "—";
        return;
    }
    const double n = static_cast<double>(voltage_ring_count_);
    const double denominator = n * v_sum_x2_ - v_sum_x_ * v_sum_x_;
    if (denominator < 1e-6) { // Sum of squares is always non-negative
        snap_.voltage_trend = "stable";
        return;
    }
    const double slope = (n * v_sum_xy_ - v_sum_x_ * v_sum_y_) / denominator;
    if (slope > 1e-6)
        snap_.voltage_trend = "up";
    else if (slope < -1e-6)
        snap_.voltage_trend = "down";
    else
        snap_.voltage_trend = "stable";
}

void AnalyticsEngine::compute_soc_trend() {
    if (voltage_ring_count_ < 6) {
        snap_.soc_trend = "—";
        snap_.soc_rate_pct_per_h = 0;
        return;
    }
    const double n = static_cast<double>(voltage_ring_count_);
    const double denominator = n * s_sum_x2_ - s_sum_x_ * s_sum_x_;
    if (denominator < 1e-6) {
        snap_.soc_trend = "stable";
        snap_.soc_rate_pct_per_h = 0;
        return;
    }
    const double slope = (n * s_sum_xy_ - s_sum_x_ * s_sum_y_) / denominator; // %/s
    snap_.soc_rate_pct_per_h = slope * 3600.0;
    if (slope > 1e-5)
        snap_.soc_trend = "up";
    else if (slope < -1e-5)
        snap_.soc_trend = "down";
    else
        snap_.soc_trend = "stable";
}

void AnalyticsEngine::compute_extended_analytics(const BatterySnapshot& bat,
                                                const PwrGateSnapshot* chg) {
    snap_.health_alerts.clear();

    // Voltage and SoC trend
    compute_voltage_trend();
    compute_soc_trend();

    // Charge rate history (for slowdown detection) — warning cleared/set in block below
    snap_.charge_rate_recent_pct_per_h = 0;
    snap_.charge_rate_baseline_pct_per_h = 0;
    if (bat.current_a < -0.1 && bat.valid) {
        double ts = static_cast<double>(
            std::chrono::system_clock::to_time_t(bat.timestamp));
        rate_ring_[rate_ring_head_] = snap_.soc_rate_pct_per_h;
        rate_ring_ts_[rate_ring_head_] = ts;
        rate_ring_head_ = (rate_ring_head_ + 1) % RATE_N;
        if (rate_ring_count_ < RATE_N) ++rate_ring_count_;

        // Compare recent (~1.5 min) vs baseline (~3 min ago) — wider windows for stability
        if (rate_ring_count_ >= 54) {
            double recent_sum = 0, baseline_sum = 0;
            size_t recent_n = 0, baseline_n = 0;
            for (size_t i = 0; i < rate_ring_count_; ++i) {
                size_t idx = (rate_ring_head_ + RATE_N - 1 - i) % RATE_N;
                if (i < 18)
                    { recent_sum += rate_ring_[idx]; ++recent_n; }
                else if (i >= 36 && i < 54)
                    { baseline_sum += rate_ring_[idx]; ++baseline_n; }
            }
            double recent_avg = recent_n > 0 ? recent_sum / recent_n : 0;
            double baseline_avg = baseline_n > 0 ? baseline_sum / baseline_n : 0;
            // EMA smoothing for stable display (alpha=0.75 → ~4 samples to settle)
            charge_rate_recent_smoothed_ = 0.75 * charge_rate_recent_smoothed_ + 0.25 * recent_avg;
            charge_rate_baseline_smoothed_ = 0.75 * charge_rate_baseline_smoothed_ + 0.25 * baseline_avg;
            snap_.charge_rate_recent_pct_per_h = charge_rate_recent_smoothed_;
            snap_.charge_rate_baseline_pct_per_h = charge_rate_baseline_smoothed_;
            // Flag if recent is significantly lower than baseline — debounce to avoid flicker
            bool slowdown_now = (baseline_avg > 0.05 && recent_avg < baseline_avg * 0.5);
            if (slowdown_now) {
                charge_slowdown_clear_ = 0;
                if (++charge_slowdown_confirm_ >= 6) {
                    char buf[120];
                    std::snprintf(buf, sizeof(buf),
                        "Charge rate has slowed: %.2f%%/h now vs %.2f%%/h earlier",
                        recent_avg, baseline_avg);
                    snap_.charge_slowdown_warning = buf;
                }
            } else {
                charge_slowdown_confirm_ = 0;
                if (!snap_.charge_slowdown_warning.empty()) {
                    if (++charge_slowdown_clear_ >= 6)
                        snap_.charge_slowdown_warning.clear();
                } else {
                    charge_slowdown_clear_ = 0;
                }
            }
        }
    } else {
        rate_ring_count_ = 0;
        charge_slowdown_confirm_ = 0;
        charge_slowdown_clear_ = 0;
        charge_rate_recent_smoothed_ = 0;
        charge_rate_baseline_smoothed_ = 0;
        snap_.charge_slowdown_warning.clear();
    }

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
        bool charging = (chg->state && (*chg->state == "Charging" || *chg->state == "Float"));
        if (charging && chg->bat_a < 0.3f && chg->target_a > 2.0f) {
            chg_low_current_count_++;
            if (chg_low_current_count_ >= 3)
                snap_.charger_abnormal_warning = "Charging but current extremely low";
        } else {
            chg_low_current_count_ = 0;
        }
        if (prev_chg_bat_v_ >= 0 && charging) {
            double delta = std::abs(chg->bat_v - prev_chg_bat_v_);
            chg_bat_v_delta_ema_ = chg_bat_v_delta_ema_ * 0.8 + delta * 0.2;
            if (chg_bat_v_delta_ema_ > 0.5)
                snap_.charger_abnormal_warning = "Voltage unstable";
        }
        prev_chg_bat_v_ = chg->bat_v;
    } else {
        prev_chg_bat_v_ = -1;
        chg_bat_v_delta_ema_ = 0;
    }

    // PSU limitation heuristic: charger maxed but voltage won't rise toward target
    if (chg && chg->valid && chg->target_v > 0.5) {
        bool in_bulk = (snap_.charging_stage == "Bulk");
        bool below_target = (chg->bat_v < chg->target_v - 0.15f);
        bool charger_maxed = (chg->pwm > 800) ||
                            (chg->target_a > 0.5f && chg->bat_a >= chg->target_a * 0.85f);
        bool voltage_not_rising = (snap_.voltage_trend != "up");
        bool charging = (chg->state && (*chg->state == "Charging" || *chg->state == "Float"));

        bool psu_limited_now = false;
        if (in_bulk && below_target && charger_maxed && voltage_not_rising && charging) {
            double ts = static_cast<double>(
                std::chrono::system_clock::to_time_t(chg->timestamp));
            if (psu_plateau_start_ts_ < 0)
                psu_plateau_start_ts_ = ts;
            double elapsed = ts - psu_plateau_start_ts_;
            if (elapsed >= 900.0)  // 15 min plateau
                psu_limited_now = true;
        } else {
            psu_plateau_start_ts_ = -1;
        }
        // Debounce to avoid flicker
        if (psu_limited_now) {
            psu_limited_clear_ = 0;
            if (++psu_limited_confirm_ >= 4) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                    "Charger may be PSU-limited: voltage %.2f V plateaued below target %.2f V "
                    "for 15+ min while charger at %d%% PWM",
                    chg->bat_v, chg->target_v, chg->pwm * 100 / 1023);
                snap_.psu_limited_warning = buf;
            }
        } else {
            psu_limited_confirm_ = 0;
            if (!snap_.psu_limited_warning.empty()) {
                if (++psu_limited_clear_ >= 4)
                    snap_.psu_limited_warning.clear();
            } else {
                psu_limited_clear_ = 0;
            }
        }
    } else {
        psu_plateau_start_ts_ = -1;
        psu_limited_confirm_ = 0;
        psu_limited_clear_ = 0;
        snap_.psu_limited_warning.clear();
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
    if (!snap_.psu_limited_warning.empty())
        snap_.health_alerts.push_back(snap_.psu_limited_warning);
    if (!snap_.charge_slowdown_warning.empty())
        snap_.health_alerts.push_back(snap_.charge_slowdown_warning);

    // Runtime: prefer battery discharge; fallback to charger power when charging.
    // 1) 24h discharge  2) 1h load profile (BMS+charger discharge)  3) charger power when charging
    constexpr double LIFEPO4_CUTOFF_PCT = 10.0;
    constexpr double MIN_LOAD_W = 0.01;   // avoid div-by-zero
    constexpr size_t MIN_LOAD_SAMPLES = 36;
    constexpr double MAX_RUNTIME_H = 1000.0;
    snap_.runtime_from_full_h = 0;
    snap_.runtime_from_current_h = 0;
    snap_.runtime_from_charger = false;
    snap_.runtime_from_calibrated = false;
    snap_.avg_discharge_24h_w = 0;
    double load_w = 0;
    if (discharge_hour_count_ >= 6) {
        double sum_wh = 0;
        for (size_t i = 0; i < discharge_hour_count_; ++i)
            sum_wh += discharge_wh_per_hour_[i];
        int span = discharge_hour_count_;
        if (discharge_hour_count_ > 1) {
            int first_h = discharge_hour_ts_[0];
            int last_h = discharge_hour_ts_[discharge_hour_count_ - 1];
            span = std::max(1, last_h - first_h + 1);
        }
        load_w = sum_wh / static_cast<double>(span);
        snap_.avg_discharge_24h_w = load_w;
    }
    // Do not use charger power as load proxy — it measures power into the battery,
    // not system load. Fall back to 1h load profile when discharge data is sparse.
    if (load_w < MIN_LOAD_W && load_count_ >= MIN_LOAD_SAMPLES && snap_.avg_load_watts >= MIN_LOAD_W) {
        load_w = snap_.avg_load_watts;  // fallback: 1h load profile (BMS + charger discharge)
        snap_.avg_discharge_24h_w = load_w;
    }
    // Require load high enough for plausible runtime (avoid >1000 h on typical 100Ah pack)
    constexpr double MIN_LOAD_FOR_PLAUSIBLE_H = 1.2;  // 1200 Wh / 1.2 W = 1000 h
    // Final fallback: user's calibrated idle load (from system_load config)
    if (load_w < MIN_LOAD_W && calibrated_idle_w_ >= MIN_LOAD_FOR_PLAUSIBLE_H) {
        load_w = calibrated_idle_w_;
        snap_.avg_discharge_24h_w = load_w;
        snap_.runtime_from_calibrated = true;
    }
    if (load_w >= MIN_LOAD_FOR_PLAUSIBLE_H && bat.nominal_ah > 1.0) {
        // Use nominal voltage (3.2V/cell for LiFePO4) for stable runtime estimates.
        // Derive cell count from instantaneous voltage, then use 3.2V * cells.
        double measured_v = bat.total_voltage_v > 1 ? bat.total_voltage_v : 51.2;
        int cells = std::max(1, static_cast<int>(std::round(measured_v / 3.3)));
        double pack_v = cells * 3.2;
        double total_wh = bat.nominal_ah * pack_v;
        double usable_from_full_wh = total_wh * (100.0 - LIFEPO4_CUTOFF_PCT) / 100.0;
        double usable_from_current_wh = total_wh * (bat.soc_pct - LIFEPO4_CUTOFF_PCT) / 100.0;
        if (usable_from_current_wh > 0) {
            double rf = usable_from_full_wh / load_w;
            double rn = usable_from_current_wh / load_w;
            snap_.runtime_full_exceeds_cap = (rf > MAX_RUNTIME_H);
            snap_.runtime_current_exceeds_cap = (rn > MAX_RUNTIME_H);
            snap_.runtime_from_full_h = std::min(rf, MAX_RUNTIME_H);
            snap_.runtime_from_current_h = std::min(rn, MAX_RUNTIME_H);
        }
    }

    compute_system_status(bat, chg);
}

void AnalyticsEngine::compute_system_status(const BatterySnapshot& bat,
                                            const PwrGateSnapshot* /*chg*/) {
    // Aggregate status from existing metrics
    if (!bat.valid) {
        snap_.system_status = "Waiting for BMS data";
        system_mode_confirm_ = 0;
        return;
    }
    // Debounce mode (Charging/Idle/Discharging) — require 4 consecutive to switch
    int pending = (bat.current_a < -0.1) ? 1 : (bat.current_a > 0.1) ? 2 : 0;
    if (pending == system_mode_) {
        system_mode_confirm_ = 0;
    } else {
        if (++system_mode_confirm_ >= 4) {
            system_mode_ = pending;
            system_mode_confirm_ = 0;
        }
    }
    std::string status;
    if (system_mode_ == 1)
        status = "Charging normally";
    else if (system_mode_ == 2)
        status = "Discharging";
    else
        status = "Idle";
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

// Extract the first integer following "key": in a JSON string.
// Returns -1 if the key is not found.
static int64_t json_extract_int(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        needle = "\"" + key + "\": ";
        pos = json.find(needle);
        if (pos == std::string::npos) return -1;
    }
    pos += needle.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    if (pos >= json.size() || (!std::isdigit(json[pos]) && json[pos] != '-')) return -1;
    try { return std::stoll(json.substr(pos)); } catch (...) { return -1; }
}

void AnalyticsEngine::update_self_monitor(Database* db) {
    // 1. Process memory usage
#ifdef __APPLE__
    task_basic_info_data_t info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
        snap_.process_rss_kb = info.resident_size / 1024;
        snap_.process_vsz_kb = info.virtual_size / 1024;
    }
#else
    {
        std::ifstream statm("/proc/self/statm");
        if (statm) {
            long vpages, rpages;
            if (statm >> vpages >> rpages) {
                long page_size = sysconf(_SC_PAGESIZE);
                snap_.process_vsz_kb = (vpages * page_size) / 1024;
                snap_.process_rss_kb = (rpages * page_size) / 1024;
            }
        }
    }
#endif

    // 2. Process CPU usage (delta between calls)
    {
        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            double utime = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1000000.0;
            double stime = usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1000000.0;
            double total_cpu_time = utime + stime;
            static double last_cpu_time = 0;
            static double last_ts = 0;
            double now = static_cast<double>(std::time(nullptr));
            if (last_ts > 0 && now > last_ts) {
                double delta_cpu = total_cpu_time - last_cpu_time;
                double delta_wall = now - last_ts;
                snap_.process_cpu_pct = (delta_cpu / delta_wall) * 100.0;
            }
            last_cpu_time = total_cpu_time;
            last_ts = now;
        }
    }

    // 3. Database size
    if (db) {
        snap_.db_size_bytes = db->file_size();
        snap_.db_table_sizes = db->table_sizes();
    }

    // 4. System load average (POSIX — works on Linux and macOS)
    {
        double loadavg[3] = {-1, -1, -1};
        if (getloadavg(loadavg, 3) == 3) {
            snap_.sys_load_1m  = loadavg[0];
            snap_.sys_load_5m  = loadavg[1];
            snap_.sys_load_15m = loadavg[2];
        }
    }

    // 5. System RAM
#ifdef __APPLE__
    {
        int64_t total = 0;
        size_t sz = sizeof(total);
        if (sysctlbyname("hw.memsize", &total, &sz, nullptr, 0) == 0)
            snap_.sys_mem_total_kb = total / 1024;

        mach_port_t host = mach_host_self();
        vm_size_t page_size_vm = 0;
        host_page_size(host, &page_size_vm);
        vm_statistics64_data_t vm_stat;
        mach_msg_type_number_t vm_count = HOST_VM_INFO64_COUNT;
        if (host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm_stat, &vm_count) == KERN_SUCCESS)
            snap_.sys_mem_available_kb =
                (int64_t)(vm_stat.free_count + vm_stat.inactive_count) * page_size_vm / 1024;
    }
#else
    {
        std::ifstream meminfo("/proc/meminfo");
        std::string line;
        int64_t total = 0, available = 0;
        while (std::getline(meminfo, line)) {
            std::istringstream iss(line.substr(line.find(':') + 1));
            if (line.rfind("MemTotal:", 0) == 0) iss >> total;
            else if (line.rfind("MemAvailable:", 0) == 0) iss >> available;
        }
        snap_.sys_mem_total_kb     = total;
        snap_.sys_mem_available_kb = available;
    }
#endif

    // 6. Disk free (statvfs on DB path or root)
    {
        const char* check_path = (db && !db->path().empty()) ? db->path().c_str() : "/";
        struct statvfs sv;
        if (statvfs(check_path, &sv) == 0) {
            snap_.disk_free_bytes  = (int64_t)sv.f_bavail * (int64_t)sv.f_frsize;
            snap_.disk_total_bytes = (int64_t)sv.f_blocks * (int64_t)sv.f_frsize;
        }
    }

    // 7. CPU frequency
#ifdef __APPLE__
    {
        int64_t freq = 0;
        size_t sz = sizeof(freq);
        // Not available on Apple Silicon; returns -1 gracefully
        if (sysctlbyname("hw.cpufrequency", &freq, &sz, nullptr, 0) == 0 && freq > 0)
            snap_.cpu_freq_mhz = freq / 1000000;
    }
#else
    {
        std::ifstream cpufreq("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq");
        int64_t khz = 0;
        if (cpufreq >> khz && khz > 0)
            snap_.cpu_freq_mhz = khz / 1000;
    }
#endif

    // 8. SSD health via smartctl (cached — runs at most once per 5 minutes)
    {
        static auto last_smart     = std::chrono::steady_clock::now() - std::chrono::minutes(6);
        static int     cached_wear = -1;
        static int64_t cached_hours = -1;
        static int64_t cached_written_gb = -1;

        auto now_tp = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::minutes>(now_tp - last_smart).count() >= 5) {
            last_smart = now_tp;

            const char* devs[] = {"/dev/nvme0", "/dev/nvme0n1", "/dev/sda", nullptr};
            for (const char** dev = devs; *dev; ++dev) {
                std::string cmd = std::string("smartctl --json=c -a ") + *dev + " 2>/dev/null";
                FILE* fp = popen(cmd.c_str(), "r");
                if (!fp) continue;

                std::string json;
                char buf[4096];
                while (fgets(buf, sizeof(buf), fp)) json += buf;
                pclose(fp);

                if (json.size() < 10) continue;

                // NVMe: "percentage_used" at top level
                int64_t wear = json_extract_int(json, "percentage_used");
                // SATA wear: look for attribute id 177 (Wear_Leveling_Count) value field,
                // or id 202 (Percent_Lifetime_Remaining) — approximated via "value" after id
                if (wear < 0) {
                    // Try common SATA remaining-life attribute (202 value = % remaining → invert)
                    auto id202 = json.find("\"id\": 202");
                    if (id202 == std::string::npos) id202 = json.find("\"id\":202");
                    if (id202 != std::string::npos) {
                        auto vpos = json.find("\"value\":", id202);
                        if (vpos != std::string::npos && vpos < id202 + 300) {
                            vpos += 8;
                            while (vpos < json.size() && json[vpos] == ' ') ++vpos;
                            try { wear = 100 - std::stoll(json.substr(vpos)); } catch (...) {}
                        }
                    }
                }

                int64_t hours = json_extract_int(json, "hours");          // power_on_time.hours
                int64_t duw   = json_extract_int(json, "data_units_written"); // NVMe (units of 512000 bytes)
                int64_t written_gb = -1;
                if (duw >= 0) written_gb = duw * 512000LL / 1000000000LL;

                // SATA fallback for total writes: attribute 241 raw value (LBAs written)
                if (written_gb < 0) {
                    auto id241 = json.find("\"id\": 241");
                    if (id241 == std::string::npos) id241 = json.find("\"id\":241");
                    if (id241 != std::string::npos) {
                        auto rpos = json.find("\"value\":", id241);
                        if (rpos != std::string::npos && rpos < id241 + 400) {
                            rpos += 8;
                            while (rpos < json.size() && json[rpos] == ' ') ++rpos;
                            try {
                                int64_t lbas = std::stoll(json.substr(rpos));
                                written_gb = lbas * 512LL / 1000000000LL;
                            } catch (...) {}
                        }
                    }
                }

                if (hours >= 0 || wear >= 0) {
                    cached_wear       = (int)wear;
                    cached_hours      = hours;
                    cached_written_gb = written_gb;
                    break;
                }
            }
        }

        snap_.ssd_wear_pct      = cached_wear;
        snap_.ssd_power_on_hours = cached_hours;
        snap_.ssd_data_written_gb = cached_written_gb;
    }
}

void AnalyticsEngine::on_battery(const BatterySnapshot& snap, const PwrGateSnapshot* chg) {
    if (!snap.valid) return;

    double ts = static_cast<double>(
        std::chrono::system_clock::to_time_t(snap.timestamp));
    int day = unix_day(ts);
    reset_daily_if_needed(day, ts);

    // Auto-detect rated capacity from BMS unless user overrode it
    if (!rated_override_ && snap.nominal_ah > 1.0) {
        rated_capacity_ah_   = snap.nominal_ah;
        snap_.rated_capacity_ah = snap.nominal_ah;
    }

    if (bat_seen_ && ts > prev_bat_ts_) {
        double dt_h = (ts - prev_bat_ts_) / 3600.0;
        if (dt_h > 0 && dt_h < (5.0 / 60.0)) {
            double pwr = snap.power_w;
            if (pwr > 0) {
                integrate_discharge_wh(ts, pwr);
            } else if (pwr < 0) {
                energy_charged_wh_ += (-pwr) * dt_h;
            }
        }
    }
    // Hourly discharge archive (local time)
    int hour = local_hour_id(ts);
    if (last_discharge_hour_ >= 0 && hour != last_discharge_hour_) {
        double wh = discharge_wh_this_hour_;
        if (discharge_hour_count_ < DISCHARGE_HOURS) {
            discharge_wh_per_hour_[discharge_hour_count_] = wh;
            discharge_hour_ts_[discharge_hour_count_] = last_discharge_hour_;
            ++discharge_hour_count_;
        } else {
            for (size_t i = 0; i < DISCHARGE_HOURS - 1; ++i) {
                discharge_wh_per_hour_[i] = discharge_wh_per_hour_[i + 1];
                discharge_hour_ts_[i] = discharge_hour_ts_[i + 1];
            }
            discharge_wh_per_hour_[DISCHARGE_HOURS - 1] = wh;
            discharge_hour_ts_[DISCHARGE_HOURS - 1] = last_discharge_hour_;
        }
        discharge_wh_this_hour_ = 0;
    }
    last_discharge_hour_ = hour;
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

    // Guard: skip clearly invalid SoC readings (0.0 can appear when BMS
    // hasn't responded yet, or from d[86]-override bugs in old DB records).
    if (snap.soc_pct > 0.5) {
        if (!soc_init_) {
            max_soc_ = snap.soc_pct;
            min_soc_ = snap.soc_pct;
            soc_init_ = true;
        } else {
            if (snap.soc_pct > max_soc_) max_soc_ = snap.soc_pct;
            if (snap.soc_pct < min_soc_) min_soc_ = snap.soc_pct;
        }
    }
    snap_.depth_of_discharge_pct = max_soc_ - min_soc_;
    double dod = snap_.depth_of_discharge_pct;
    if      (dod < 30) snap_.dod_status = "Low stress";
    else if (dod < 70) snap_.dod_status = "Normal";
    else               snap_.dod_status = "High stress";

    // Only push a BMS load sample when the battery is actually discharging.
    // Do NOT push 0 W when idle/charging — that floods the ring buffer and
    // contaminates the historical average, breaking the off-grid load estimate.
    if (snap.current_a > 0.1) {
        push_load_sample(ts, snap.power_w);
        recompute_load_stats();
    }

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

    push_voltage_soc_sample(ts, snap.total_voltage_v, snap.soc_pct);
    compute_extended_analytics(snap, chg);

    prev_bat_ts_ = ts;
    bat_seen_    = true;
}

void AnalyticsEngine::on_charger(const PwrGateSnapshot& snap, const BatterySnapshot* bat) {
    if (!snap.valid) return;

    double ts = static_cast<double>(
        std::chrono::system_clock::to_time_t(snap.timestamp));
    int day = unix_day(ts);
    reset_daily_if_needed(day, ts);

    if (chg_seen_ && ts > prev_chg_ts_ && snap.sol_v > 1.0 && snap.bat_a > 0) {
        double dt_h = (ts - prev_chg_ts_) / 3600.0;
        if (dt_h > 0 && dt_h < (5.0 / 60.0)) {
            // Solar power harvested = battery voltage × charge current (actual power into battery)
            double sol_pwr = snap.bat_v * snap.bat_a;
            solar_energy_wh_ += sol_pwr * dt_h;
        }
    }
    snap_.solar_energy_today_wh = solar_energy_wh_;

    update_charging_stage(static_cast<double>(snap.bat_v), static_cast<double>(snap.target_v),
                          static_cast<double>(snap.bat_a), snap.state ? *snap.state : "");
    prev_chg_a_ = static_cast<double>(snap.bat_a);

    // Efficiency approximation: bat_v / ps_v. Valid for linear/series-pass chargers
    // where I_in ≈ I_out. For switching converters this is a voltage ratio, not
    // true efficiency (would need input current measurement).
    if (snap.bat_a > 0.5 && snap.bat_v > 1.0) {
        double input_v = snap.ps_v > 0.5 ? snap.ps_v
                       : snap.sol_v > 0.5 ? snap.sol_v : 0;
        if (input_v > snap.bat_v) {
            snap_.charger_efficiency = snap.bat_v / input_v;
            snap_.efficiency_valid   = true;
        }
    }

    if (snap.bat_a < -0.05) {
        double load_w = snap.bat_v * (-snap.bat_a);
        push_load_sample(ts, load_w);
        recompute_load_stats();
        integrate_discharge_wh(ts, load_w);
    }
    if (snap.bat_a > 0.05) {
        push_charger_power_sample(snap.bat_v * snap.bat_a);
    }

    snap_.solar_voltage_v = snap.sol_v;
    snap_.solar_active    = snap.sol_v > 1.0;
    snap_.solar_power_w   = (snap_.solar_active && snap.bat_a > 0)
                           ? snap.bat_v * snap.bat_a : 0.0;
    snap_.solar_energy_today_wh = solar_energy_wh_;

    // Charger runtime today
    int chg_day = unix_day(ts);
    if (charger_runtime_day_ != chg_day) {
        charger_runtime_sec_ = 0;
        charger_runtime_day_ = chg_day;
        prev_chg_minutes_ = -1;
    }
    if (snap.bat_a > 0.05f || (snap.state && (*snap.state == "Charging" || *snap.state == "Float"))) {
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


