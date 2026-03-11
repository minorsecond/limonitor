#include "health_score.hpp"
#include "anomaly_detection.hpp"
#include "../analytics.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

HealthScorer::HealthScorer() = default;

HealthScoreResult HealthScorer::compute(const AnalyticsSnapshot& an,
                                        size_t anomaly_count) const {
    HealthScoreResult r;
    int score = 100;

    // Battery health (weight 30)
    double health = an.battery_health_pct >= 0 ? an.battery_health_pct : an.capacity_health_pct;
    if (health >= 0) {
        if (health >= 90) { r.battery = "excellent"; }
        else if (health >= 80) { r.battery = "good"; score -= 5; }
        else if (health >= 70) { r.battery = "fair"; score -= 15; }
        else { r.battery = "poor"; score -= 25; }
    } else {
        r.battery = "unknown";
    }

    // Cell balance (weight 20)
    double delta_mv = an.cell_delta_mv;
    if (delta_mv > 50) { r.cells = "imbalanced"; score -= 20; }
    else if (delta_mv > 25) { r.cells = "warning"; score -= 10; }
    else if (delta_mv > 10) { r.cells = "balanced"; score -= 5; }
    else { r.cells = "balanced"; }

    // Charge efficiency (weight 20)
    if (an.efficiency_valid && an.charger_efficiency >= 0) {
        if (an.charger_efficiency >= 0.9) { r.charging = "optimal"; }
        else if (an.charger_efficiency >= 0.8) { r.charging = "degraded"; score -= 10; }
        else { r.charging = "poor"; score -= 20; }
    } else {
        r.charging = "—";
    }

    // Temperature safety (weight 10)
    double max_temp = std::max(an.temp1_c, an.temp2_c);
    if (an.temp_valid) {
        if (max_temp >= 50) { r.temperature = "critical"; score -= 20; }
        else if (max_temp >= 40) { r.temperature = "warm"; score -= 10; }
        else { r.temperature = "normal"; }
    } else {
        r.temperature = "—";
    }

    // Anomalies present (weight 20)
    if (anomaly_count > 0) {
        score -= 15;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%zu detected", anomaly_count);
        r.anomalies = buf;
    } else {
        r.anomalies = "none";
    }

    r.score = std::max(0, std::min(100, score));
    return r;
}
