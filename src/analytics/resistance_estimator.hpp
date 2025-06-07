#pragma once
#include "../tx_events.hpp"
#include <chrono>
#include <deque>
#include <set>
#include <string>
#include <vector>

struct BatterySnapshot;

struct ResistanceSample {
    double timestamp;
    double resistance_mohm;
};

class ResistanceEstimator {
public:
    ResistanceEstimator();

    // Call on each update. Processes any new TX events from tx_events using history.
    void update(const std::vector<BatterySnapshot>& history,
                const std::vector<TxEvent>& tx_events);

    double internal_resistance_mohm() const;
    std::string trend() const;  // "stable", "increasing", "decreasing"
    std::vector<ResistanceSample> time_series(size_t n = 100) const;

private:
    static constexpr size_t ROLLING_N = 20;
    std::deque<double> r_ring_;
    std::deque<double> r_ts_;
    std::set<double> processed_end_times_;

    void push_sample(double ts, double r_mohm);
};
