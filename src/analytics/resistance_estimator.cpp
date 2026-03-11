#include "resistance_estimator.hpp"
#include "../battery_data.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

ResistanceEstimator::ResistanceEstimator() = default;

void ResistanceEstimator::push_sample(double ts, double r_mohm) {
    if (r_mohm <= 0 || r_mohm > 500) return;
    r_ring_.push_back(r_mohm);
    r_ts_.push_back(ts);
    while (r_ring_.size() > ROLLING_N) {
        r_ring_.pop_front();
        r_ts_.pop_front();
    }
}

void ResistanceEstimator::update(const std::deque<BatterySnapshot>& history,
                                 const std::deque<TxEvent>& tx_events) {
    if (history.empty()) return;

    for (const auto& ev : tx_events) {
        if (ev.peak_current < 0.5 || ev.duration < 0.5) continue;
        if (processed_end_times_.count(ev.end_time) > 0) continue;
        processed_end_times_.insert(ev.end_time);
        while (processed_end_times_.size() > 200) {
            processed_end_times_.erase(processed_end_times_.begin());
        }

        double start_ts = ev.start_time;
        double end_ts = ev.end_time;

        const BatterySnapshot* pre = nullptr;
        const BatterySnapshot* during = nullptr;
        double pre_ts = -1, during_ts = -1;

        for (const auto& s : history) {
            if (!s.valid) continue;
            double ts = static_cast<double>(std::chrono::system_clock::to_time_t(s.timestamp));
            if (ts <= start_ts && (pre == nullptr || ts > pre_ts)) {
                pre = &s;
                pre_ts = ts;
            }
            if (ts >= start_ts && ts <= end_ts && (during == nullptr || ts < during_ts)) {
                during = &s;
                during_ts = ts;
            }
        }

        if (!pre || !during) continue;

        double v_idle = pre->total_voltage_v;
        double v_tx = during->total_voltage_v;
        double i_idle = pre->current_a > 0 ? pre->current_a : 0;
        double i_tx = ev.peak_current;

        double dV = v_idle - v_tx;
        double dI = i_tx - i_idle;

        if (dI < 0.3) continue;

        double r_ohm = dV / dI;
        double r_mohm = r_ohm * 1000.0;

        push_sample(ev.end_time, r_mohm);
    }
}

double ResistanceEstimator::internal_resistance_mohm() const {
    if (r_ring_.empty()) return 0;
    double sum = 0;
    for (double r : r_ring_) sum += r;
    return sum / static_cast<double>(r_ring_.size());
}

std::string ResistanceEstimator::trend() const {
    if (r_ring_.empty()) return "—";
    if (r_ring_.size() < 6) return "collecting";
    size_t half = r_ring_.size() / 2;
    double first_half = 0, second_half = 0;
    for (size_t i = 0; i < half; ++i) first_half += r_ring_[i];
    for (size_t i = half; i < r_ring_.size(); ++i) second_half += r_ring_[i];
    first_half /= half;
    second_half /= (r_ring_.size() - half);
    double pct = (second_half - first_half) / (first_half > 0 ? first_half : 1);
    if (pct > 0.1) return "increasing";
    if (pct < -0.1) return "decreasing";
    return "stable";
}

std::vector<ResistanceSample> ResistanceEstimator::time_series(size_t n) const {
    std::vector<ResistanceSample> out;
    size_t start = n < r_ring_.size() ? r_ring_.size() - n : 0;
    out.reserve(r_ring_.size() - start);
    for (size_t i = start; i < r_ring_.size(); ++i) {
        out.push_back({r_ts_[i], r_ring_[i]});
    }
    return out;
}
