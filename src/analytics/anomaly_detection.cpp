#include "anomaly_detection.hpp"
#include "../analytics.hpp"
#include "../battery_data.hpp"
#include "../pwrgate.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>

AnomalyDetector::AnomalyDetector() = default;

double AnomalyDetector::rolling_avg_load() const {
    if (load_count_ == 0) return 0;
    double sum = 0;
    for (size_t i = 0; i < load_count_; ++i)
        sum += load_ring_[i];
    return sum / static_cast<double>(load_count_);
}

void AnomalyDetector::r_internal_avgs(double& avg_30d, double& avg_today) const {
    avg_30d = 0;
    avg_today = 0;
    if (r_internal_ring_.empty()) return;
    int today = static_cast<int>(std::time(nullptr) / 86400);
    double sum_30d = 0, sum_today = 0;
    size_t n_30d = 0, n_today = 0;
    for (size_t i = 0; i < r_internal_ring_.size(); ++i) {
        double r = r_internal_ring_[i];
        if (r <= 0) continue;
        int d = r_internal_day_[i];
        if (today - d <= 30) { sum_30d += r; ++n_30d; }
        if (d == today) { sum_today += r; ++n_today; }
    }
    if (n_30d > 0) avg_30d = sum_30d / n_30d;
    if (n_today > 0) avg_today = sum_today / n_today;
}

void AnomalyDetector::update(const BatterySnapshot& bat, const PwrGateSnapshot* chg,
                            const AnalyticsSnapshot& an, double r_internal_mohm,
                            double effective_max_charge_a) {
    if (!bat.valid) return;

    double load_w = bat.power_w > 0 ? bat.power_w : 0;
    if (load_w > 0.1) {
        load_ring_.push_back(load_w);
        if (load_ring_.size() > LOAD_WINDOW)
            load_ring_.pop_front();
    }
    load_count_ = load_ring_.size();

    if (r_internal_mohm > 0) {
        int today = static_cast<int>(std::time(nullptr) / 86400);
        r_internal_ring_.push_back(r_internal_mohm);
        r_internal_day_.push_back(today);
        if (r_internal_ring_.size() > R_DAYS * 100) {
            r_internal_ring_.pop_front();
            r_internal_day_.pop_front();
        }
    }

    double baseline = rolling_avg_load();
    auto now = std::chrono::system_clock::now();
    double now_ts = static_cast<double>(std::chrono::system_clock::to_time_t(now));

    // Debounce: avoid duplicate load_spike within 2 min
    bool recent_load_spike = false;
    for (auto it = anomalies_.rbegin(); it != anomalies_.rend() && !recent_load_spike; ++it) {
        if (it->type != "load_spike") break;
        double ts = static_cast<double>(std::chrono::system_clock::to_time_t(it->timestamp));
        if (now_ts - ts < 120) recent_load_spike = true;
    }

    // Load spike: current_load > rolling_avg_load * 1.8
    if (!recent_load_spike && load_count_ >= 72 && baseline > 5 && load_w > baseline * LOAD_SPIKE_THRESHOLD) {
        AnomalyEvent ev;
        ev.type = "load_spike";
        ev.timestamp = now;
        ev.load_w = load_w;
        ev.baseline_w = baseline;
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "Unexpected load spike detected\nCurrent: %.0fW\nBaseline: %.0fW",
            load_w, baseline);
        ev.message = buf;
        anomalies_.push_back(ev);
        while (anomalies_.size() > ANOMALIES_MAX) anomalies_.pop_front();
    }

    // Slow charging: charger_state == BULK AND charge_current < expected_current * 0.6
    // Debounce: avoid duplicate within 15 min
    bool recent_slow_charging = false;
    for (auto it = anomalies_.rbegin(); it != anomalies_.rend() && !recent_slow_charging; ++it) {
        if (it->type != "slow_charging") break;
        double ts = static_cast<double>(std::chrono::system_clock::to_time_t(it->timestamp));
        if (now_ts - ts < 900) recent_slow_charging = true;
    }

    if (!recent_slow_charging && chg && chg->valid && an.charging_stage == "Bulk") {
        double soc = bat.soc_pct / 100.0;
        double max_a = chg->target_a > 0 ? chg->target_a : effective_max_charge_a;
        double expected = max_a * (1.0 - soc);
        if (expected < 0.5) expected = 0.5;
        double actual = std::abs(chg->bat_a);
        if (actual < expected * SLOW_CHARGE_THRESHOLD) {
            AnomalyEvent ev;
            ev.type = "slow_charging";
            ev.timestamp = now;
            ev.charge_current = actual;
            ev.expected_current = expected;
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                "Slow charging detected\nCurrent: %.2fA\nExpected: %.2fA",
                actual, expected);
            ev.message = buf;
            anomalies_.push_back(ev);
            while (anomalies_.size() > ANOMALIES_MAX) anomalies_.pop_front();
        }
    }

    // Battery internal resistance increase: R_internal_today > R_internal_30day_avg * 1.5
    // Debounce: avoid duplicate within 24 h
    bool recent_resistance = false;
    for (auto it = anomalies_.rbegin(); it != anomalies_.rend() && !recent_resistance; ++it) {
        if (it->type != "resistance_increase") break;
        double ts = static_cast<double>(std::chrono::system_clock::to_time_t(it->timestamp));
        if (now_ts - ts < 86400) recent_resistance = true;
    }

    double r_today = 0, r_30d = 0;
    r_internal_avgs(r_30d, r_today);
    if (!recent_resistance && r_today > 0 && r_30d > 0 && r_today > r_30d * RESISTANCE_INCREASE_THRESHOLD) {
        AnomalyEvent ev;
        ev.type = "resistance_increase";
        ev.timestamp = now;
        ev.r_internal_today = r_today;
        ev.r_internal_30day_avg = r_30d;
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "Battery internal resistance increase\nToday: %.2f mΩ\n30-day avg: %.2f mΩ",
            r_today, r_30d);
        ev.message = buf;
        anomalies_.push_back(ev);
        while (anomalies_.size() > ANOMALIES_MAX) anomalies_.pop_front();
    }
}

const std::deque<AnomalyEvent>& AnomalyDetector::anomalies() const {
    return anomalies_;
}

std::vector<AnomalyEvent> AnomalyDetector::anomalies_vec() const {
    return std::vector<AnomalyEvent>(anomalies_.begin(), anomalies_.end());
}
