#pragma once
#include <string>
#include <vector>

struct AnalyticsSnapshot;
struct AnomalyEvent;

struct HealthScoreResult {
    int score{0};           // 0–100
    std::string battery;    // "excellent", "good", "fair", "poor"
    std::string cells;      // "balanced", "warning", "imbalanced"
    std::string charging;  // "optimal", "degraded", "poor"
    std::string temperature;
    std::string anomalies;
};

class HealthScorer {
public:
    HealthScorer();

    HealthScoreResult compute(const AnalyticsSnapshot& an,
                              size_t anomaly_count) const;
};
