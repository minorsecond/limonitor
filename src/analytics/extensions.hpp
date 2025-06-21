#pragma once
#include "anomaly_detection.hpp"
#include "health_score.hpp"
#include "resistance_estimator.hpp"
#include "solar_simulation.hpp"
#include "weather_forecast.hpp"
#include "../config.hpp"
#include <chrono>
#include <deque>
#include <memory>

struct BatterySnapshot;
struct PwrGateSnapshot;
struct AnalyticsSnapshot;

class Database;

class AnalyticsExtensions {
public:
    explicit AnalyticsExtensions(const Config& cfg, Database* db = nullptr);

    void set_config(const Config& cfg);

    void update(const BatterySnapshot& bat, const PwrGateSnapshot* chg,
                const AnalyticsSnapshot& an,
                const std::vector<BatterySnapshot>& history,
                const std::vector<TxEvent>& tx_events);

    const AnomalyDetector& anomaly_detector() const { return *anomaly_; }
    const ResistanceEstimator& resistance_estimator() const { return *resistance_; }
    const SolarSimulator& solar_simulator() const { return *solar_; }

    // Effective max charge amps: current target_a, or historical max when 0
    double effective_max_charge_a() const { return effective_max_charge_a_; }
    const WeatherForecast& weather_forecast() const { return *weather_; }
    const HealthScorer& health_scorer() const { return *health_; }

    // Latest solar performance coefficient (0 = no data yet)
    double latest_solar_coefficient() const { return latest_solar_coeff_; }

private:
    std::unique_ptr<AnomalyDetector> anomaly_;
    std::unique_ptr<ResistanceEstimator> resistance_;
    std::unique_ptr<SolarSimulator> solar_;
    std::unique_ptr<WeatherForecast> weather_;
    std::unique_ptr<HealthScorer> health_;

    SolarSimConfig solar_cfg_;
    WeatherConfig weather_cfg_;

    // Rolling max of charger target_a for fallback
    static constexpr size_t MAX_CHARGE_HISTORY = 72;
    std::deque<double> max_charge_history_;
    double effective_max_charge_a_{10.0};

    // Solar performance tracking
    Database* db_{nullptr};
    double panel_watts_{0};
    double system_efficiency_{0.85};
    double latest_solar_coeff_{0};
    std::chrono::steady_clock::time_point last_solar_perf_time_{};
    static constexpr int SOLAR_PERF_INTERVAL_SEC = 900; // 15 minutes

    void maybe_record_solar_performance(const BatterySnapshot& bat, const PwrGateSnapshot* chg);
};
