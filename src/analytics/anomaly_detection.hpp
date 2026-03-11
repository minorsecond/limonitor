#pragma once
#include <chrono>
#include <deque>
#include <string>
#include <vector>

struct BatterySnapshot;
struct PwrGateSnapshot;
struct AnalyticsSnapshot;

struct AnomalyEvent {
    std::string type;       // "load_spike", "slow_charging", "resistance_increase"
    std::chrono::system_clock::time_point timestamp;
    std::string message;
    double load_w{0};
    double baseline_w{0};
    double charge_current{0};
    double expected_current{0};
    double r_internal_today{0};
    double r_internal_30day_avg{0};
};

class AnomalyDetector {
public:
    AnomalyDetector();

    // Call under DataStore mutex. Uses rolling windows: 10–30 min for load baseline.
    // effective_max_charge_a: from charger target_a or historical max when unavailable.
    void update(const BatterySnapshot& bat, const PwrGateSnapshot* chg,
                const AnalyticsSnapshot& an, double r_internal_mohm,
                double effective_max_charge_a = 10.0);

    const std::deque<AnomalyEvent>& anomalies() const;
    std::vector<AnomalyEvent> anomalies_vec() const;

private:
    static constexpr size_t LOAD_WINDOW = 360;
    static constexpr double LOAD_SPIKE_THRESHOLD = 1.8;
    static constexpr double SLOW_CHARGE_THRESHOLD = 0.6;
    static constexpr double RESISTANCE_INCREASE_THRESHOLD = 1.5;

    std::deque<double> load_ring_;
    size_t load_count_{0};

    std::deque<double> r_internal_ring_;
    std::deque<int> r_internal_day_;
    static constexpr size_t R_DAYS = 35;

    std::deque<AnomalyEvent> anomalies_;
    static constexpr size_t ANOMALIES_MAX = 50;

    double rolling_avg_load() const;
    void r_internal_avgs(double& avg_30d, double& avg_today) const;
};
