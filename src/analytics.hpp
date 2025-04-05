#pragma once
#include <string>
#include <vector>

struct AnalyticsSnapshot {
    double energy_charged_today_wh{0};
    double energy_discharged_today_wh{0};
    double solar_energy_today_wh{0};
    double net_energy_today_wh{0};       // charged - discharged

    double battery_age_years{-1};        // -1 = no purchase date set
    double battery_health_pct{-1};       // -1 = insufficient data
    double years_remaining_low{-1};      // estimated range low
    double years_remaining_high{-1};     // estimated range high
    bool   battery_replace_warn{false};  // health < 80% OR age > 8 yr

    std::string charging_stage{"Idle"};  // Bulk / Absorption / Float / Idle

    double usable_capacity_ah{0};
    double rated_capacity_ah{100};
    double capacity_health_pct{-1};      // -1 = not enough discharge data yet

    double cell_delta_mv{0};
    std::string cell_balance_status{"—"};

    double temp1_c{0};
    double temp2_c{0};
    bool   temp_valid{false};
    std::string temp_status{"—"};

    double charger_efficiency{-1};       // 0–1 ratio, -1 = unknown
    bool   efficiency_valid{false};

    double solar_voltage_v{0};
    double solar_power_w{0};
    bool   solar_active{false};

    double depth_of_discharge_pct{0};
    std::string dod_status{"—"};

    double avg_load_watts{0};
    double peak_load_watts{0};
    double idle_load_watts{0};

    // Extended analytics (system intelligence)
    std::string voltage_trend{"—"};       // "up", "down", "stable"
    double charger_runtime_today_sec{0};
    double energy_used_week_wh{0};
    std::string battery_stress{"—"};     // "low", "moderate", "high"
    std::string charger_abnormal_warning{};
    std::string system_status{"—"};
    std::string solar_readiness{};
    double charge_rate_w{0};
    double charge_rate_pct_per_h{0};
    std::vector<std::string> health_alerts;
};

// Forward declarations (avoid pulling in full headers)
struct BatterySnapshot;
struct PwrGateSnapshot;

// Incremental analytics engine. All methods must be called while the owning
// DataStore mutex is held — no internal locking.
class AnalyticsEngine {
public:
    explicit AnalyticsEngine(double rated_capacity_ah = 100.0);

    // Configuration — call before feeding data or at any time
    void set_rated_capacity(double ah);           // 0 = auto-detect from BMS
    void set_purchase_date(const std::string& d); // "YYYY-MM-DD"

    // Update hooks (called by DataStore under its mutex).
    // Optional other snapshot for extended analytics (voltage trend, charge rate, etc.).
    void on_battery(const BatterySnapshot& snap, const PwrGateSnapshot* chg = nullptr);
    void on_charger(const PwrGateSnapshot& snap, const BatterySnapshot* bat = nullptr);

    const AnalyticsSnapshot& snapshot() const { return snap_; }

private:
    AnalyticsSnapshot snap_;

    double rated_capacity_ah_{100.0};
    bool   rated_override_{false};   // true = user set it explicitly
    std::string purchase_date_;

    // Daily energy counters (midnight reset)
    double energy_charged_wh_{0};
    double energy_discharged_wh_{0};
    double solar_energy_wh_{0};
    int    current_day_{-1};

    // Delta-time integration state
    double prev_bat_ts_{0};
    double prev_chg_ts_{0};
    bool   bat_seen_{false};
    bool   chg_seen_{false};

    // SoC extremes for DoD (today)
    double max_soc_{0};
    double min_soc_{100};
    bool   soc_init_{false};

    // Load profile ring buffer (~1 h of 5 s samples)
    static constexpr size_t LOAD_N = 720;
    double load_ring_[LOAD_N]{};
    size_t load_head_{0};
    size_t load_count_{0};

    // Discharge capacity tracking (for health estimation)
    bool   was_discharging_{false};
    double discharge_start_ah_{0};
    double best_discharge_ah_{0};

    // Charger state for stage detection
    double prev_chg_a_{-1};

    // Voltage trend (last N samples)
    static constexpr size_t VOLTAGE_N = 12;
    double voltage_ring_[VOLTAGE_N]{};
    double voltage_ring_ts_[VOLTAGE_N]{};
    size_t voltage_ring_count_{0};
    size_t voltage_ring_head_{0};

    // Charger runtime today (seconds)
    double charger_runtime_sec_{0};
    int charger_runtime_day_{-1};
    int prev_chg_minutes_{-1};

    // Weekly energy (last 7 completed days)
    double energy_by_day_[8]{};
    int days_by_day_[8]{};
    size_t energy_days_count_{0};

    // Charger behavior (for abnormal detection)
    double prev_chg_bat_v_{-1};
    double chg_bat_v_variance_{0};
    size_t chg_low_current_count_{0};

    void push_voltage_sample(double ts, double v);
    void compute_voltage_trend();
    void compute_extended_analytics(const BatterySnapshot& bat,
                                   const PwrGateSnapshot* chg);
    void compute_system_status(const BatterySnapshot& bat,
                              const PwrGateSnapshot* chg);

    // Helpers
    static int unix_day(double ts);
    void reset_daily_if_needed(int day);
    void push_load_sample(double watts);
    void recompute_load_stats();
    void update_health_and_age();
    void update_charging_stage(double bat_v, double target_v,
                               double bat_a, const std::string& state);
};
