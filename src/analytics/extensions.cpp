#include "extensions.hpp"
#include "../battery_data.hpp"
#include "../database.hpp"
#include "../logger.hpp"
#include "../pwrgate.hpp"
#include "../tx_events.hpp"

AnalyticsExtensions::AnalyticsExtensions(const Config& cfg, Database* db)
    : db_(db)
{
    solar_cfg_.enabled = cfg.solar_enabled;
    solar_cfg_.panel_watts = cfg.solar_panel_watts;
    solar_cfg_.system_efficiency = cfg.solar_system_efficiency;
    solar_cfg_.zip_code = cfg.solar_zip_code;

    panel_watts_ = cfg.solar_panel_watts;
    system_efficiency_ = cfg.solar_system_efficiency;

    weather_cfg_.api_key = cfg.weather_api_key;
    weather_cfg_.zip_code = cfg.weather_zip_code.empty() ? cfg.solar_zip_code : cfg.weather_zip_code;

    anomaly_ = std::make_unique<AnomalyDetector>();
    resistance_ = std::make_unique<ResistanceEstimator>();
    solar_ = std::make_unique<SolarSimulator>(solar_cfg_);
    weather_ = std::make_unique<WeatherForecast>(weather_cfg_, db);
    health_ = std::make_unique<HealthScorer>();

    LOG_INFO("Extensions: solar=%s weather_api_key=%s zip=%s",
             solar_cfg_.enabled ? "on" : "off",
             weather_cfg_.api_key.empty() ? "(none)" : "set",
             weather_cfg_.zip_code.c_str());
}

void AnalyticsExtensions::set_config(const Config& cfg) {
    solar_cfg_.enabled = cfg.solar_enabled;
    solar_cfg_.panel_watts = cfg.solar_panel_watts;
    solar_cfg_.system_efficiency = cfg.solar_system_efficiency;
    solar_cfg_.zip_code = cfg.solar_zip_code;
    solar_->set_config(solar_cfg_);

    panel_watts_ = cfg.solar_panel_watts;
    system_efficiency_ = cfg.solar_system_efficiency;

    weather_cfg_.api_key = cfg.weather_api_key;
    weather_cfg_.zip_code = cfg.weather_zip_code.empty() ? cfg.solar_zip_code : cfg.weather_zip_code;
    weather_->set_config(weather_cfg_);
}

void AnalyticsExtensions::update(const BatterySnapshot& bat, const PwrGateSnapshot* chg,
                                 const AnalyticsSnapshot& an,
                                 const std::deque<BatterySnapshot>& history,
                                 const std::deque<TxEvent>& tx_events) {
    // Track max charge amps from charger for solar/battery calculations
    if (chg && chg->valid && chg->target_a > 0) {
        max_charge_history_.push_back(chg->target_a);
        while (max_charge_history_.size() > MAX_CHARGE_HISTORY)
            max_charge_history_.pop_front();
        effective_max_charge_a_ = chg->target_a;
    } else if (!max_charge_history_.empty()) {
        double hist_max = 0;
        for (double a : max_charge_history_) if (a > hist_max) hist_max = a;
        effective_max_charge_a_ = hist_max;
    }

    resistance_->update(history, tx_events);
    double r_mohm = resistance_->internal_resistance_mohm();
    anomaly_->update(bat, chg, an, r_mohm, effective_max_charge_a_);

    maybe_record_solar_performance(bat, chg);
}

void AnalyticsExtensions::maybe_record_solar_performance(
        const BatterySnapshot& bat, const PwrGateSnapshot* chg) {
    if (!db_ || !chg || !chg->valid) return;
    if (panel_watts_ <= 0) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_solar_perf_time_).count();
    if (elapsed < SOLAR_PERF_INTERVAL_SEC) return;

    // Only record when solar is genuinely active
    if (chg->sol_v < 5.0 || chg->bat_a < 0.5) return;

    double cloud = weather_->current_cloud_cover();
    if (cloud < 0) return; // no forecast data available

    double actual_w = chg->bat_v * chg->bat_a;
    double clear_sky_w = panel_watts_ * system_efficiency_;
    double theoretical_w = clear_sky_w * (1.0 - cloud);

    // Avoid division by tiny numbers (very overcast = unreliable denominator)
    if (theoretical_w < 5.0) return;

    double coeff = actual_w / theoretical_w;

    // Clamp outliers but still store them for analysis
    if (coeff > 3.0) coeff = 3.0;

    Database::SolarPerfRow row;
    row.ts = std::time(nullptr);
    row.cloud_cover = cloud;
    row.actual_w = actual_w;
    row.theoretical_w = theoretical_w;
    row.coefficient = coeff;
    row.sol_v = chg->sol_v;
    row.bat_v = chg->bat_v;
    row.bat_a = chg->bat_a;
    row.soc = bat.soc_pct;

    db_->insert_solar_performance(row);
    latest_solar_coeff_ = coeff;
    last_solar_perf_time_ = now;

    LOG_INFO("Solar perf: actual=%.1fW theoretical=%.1fW cloud=%.0f%% coeff=%.3f soc=%.0f%%",
             actual_w, theoretical_w, cloud * 100.0, coeff, bat.soc_pct);
}
