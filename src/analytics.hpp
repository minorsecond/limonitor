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
    std::string soc_trend{"—"};           // "up", "down", "stable"
    double soc_rate_pct_per_h{0};         // observed SoC change rate (%/h)
    double charger_runtime_today_sec{0};
    double energy_used_week_wh{0};
    std::string battery_stress{"—"};     // "low", "moderate", "high"
    std::string charger_abnormal_warning{};
    std::string psu_limited_warning{};   // heuristic: charger may be PSU-limited
    std::string charge_slowdown_warning{};  // charge rate has declined recently
    double charge_rate_recent_pct_per_h{0};   // recent avg
    double charge_rate_baseline_pct_per_h{0}; // earlier avg (for comparison)
    std::string system_status{"—"};
    std::string solar_readiness{};
    double charge_rate_w{0};
    double charge_rate_pct_per_h{0};
    double runtime_from_full_h{0};      // 100% → cutoff, based on historical load
    double runtime_from_current_h{0};   // current SoC → cutoff, based on historical load
    bool   runtime_full_exceeds_cap{false};   // display "> 1000 h" for full
    bool   runtime_current_exceeds_cap{false}; // display "> 1000 h" for current
    bool   runtime_from_charger{false}; // true when load used charger power (charging) fallback
    bool   runtime_from_historical{false}; // true when load from 7d DB profile (on grid)
    bool   runtime_from_calibrated{false}; // true when load used user's calibrated idle
    double avg_discharge_24h_w{0};      // 24h average discharge (W), 0 = insufficient data
    std::vector<std::string> health_alerts;

    // Self-monitoring — process
    int64_t process_rss_kb{0};
    int64_t process_vsz_kb{0};
    double  process_cpu_pct{0};
    int64_t db_size_bytes{0};
    std::vector<std::pair<std::string, int64_t>> db_table_sizes;

    // Self-monitoring — system health
    double  sys_load_1m{-1};           // 1-min load average (-1 = unavailable)
    double  sys_load_5m{-1};
    double  sys_load_15m{-1};
    int64_t sys_mem_total_kb{0};       // total system RAM
    int64_t sys_mem_available_kb{0};   // available system RAM
    int64_t disk_free_bytes{-1};       // free bytes on storage device
    int64_t disk_total_bytes{-1};      // total bytes on storage device
    int64_t cpu_freq_mhz{-1};         // current CPU frequency (-1 = unavailable)

    // Self-monitoring — SSD health (via smartctl, cached)
    int     ssd_wear_pct{-1};          // % used 0–100, -1 = unavailable
    int64_t ssd_power_on_hours{-1};    // total power-on hours
    int64_t ssd_data_written_gb{-1};   // total lifetime data written (GB)
};

// Forward declarations (avoid pulling in full headers)
class Database;
struct BatterySnapshot;
struct PwrGateSnapshot;

class AnalyticsEngine {
public:
    explicit AnalyticsEngine(double rated_capacity_ah = 100.0);

    void set_rated_capacity(double ah);            // 0 = auto-detect from BMS
    void set_purchase_date(const std::string& d);  // "YYYY-MM-DD"
    void set_calibrated_idle_w(double w);          // user-measured idle load fallback

    void on_battery(const BatterySnapshot& snap, const PwrGateSnapshot* chg = nullptr);
    void on_charger(const PwrGateSnapshot& snap, const BatterySnapshot* bat = nullptr);

    const AnalyticsSnapshot& snapshot() const { return snap_; }
    void update_self_monitor(Database* db);

private:
    AnalyticsSnapshot snap_;

    double rated_capacity_ah_{100.0};
    bool   rated_override_{false};   // true = user set it explicitly
    std::string purchase_date_;
    double calibrated_idle_w_{0.0};  // user-measured idle load (system_load config)

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
    double last_load_push_ts_{-1};

    // Discharge capacity tracking (for health estimation)
    bool   was_discharging_{false};
    double discharge_start_ah_{0};
    double best_discharge_ah_{0};

    // Charger state for stage detection
    double prev_chg_a_{-1};

    // Voltage and SoC trend (last N samples)
    static constexpr size_t VOLTAGE_N = 12;
    double voltage_ring_[VOLTAGE_N]{};
    double soc_ring_[VOLTAGE_N]{};
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
    double chg_bat_v_delta_ema_{0};
    size_t chg_low_current_count_{0};

    // PSU limitation heuristic
    double psu_plateau_start_ts_{-1};

    // 24h discharge rate (for runtime estimates)
    static constexpr size_t DISCHARGE_HOURS = 24;
    double discharge_wh_per_hour_[DISCHARGE_HOURS]{};
    int discharge_hour_ts_[DISCHARGE_HOURS]{};
    size_t discharge_hour_count_{0};
    double discharge_wh_this_hour_{0};
    int last_discharge_hour_{-1};
    double last_discharge_integration_ts_{-1};  // shared by BMS+charger to avoid double-count

    // Charge rate history (for slowdown detection)
    static constexpr size_t RATE_N = 72;  // ~6 min at 5s polls
    double rate_ring_[RATE_N]{};
    double rate_ring_ts_[RATE_N]{};
    size_t rate_ring_count_{0};
    size_t rate_ring_head_{0};

    // Cached sums for O(1) trend calculation
    double v_sum_x_{0}, v_sum_y_{0}, v_sum_x2_{0}, v_sum_xy_{0};
    double s_sum_x_{0}, s_sum_y_{0}, s_sum_x2_{0}, s_sum_xy_{0};

    int charge_slowdown_confirm_{0};   // debounce: require 6 consecutive to show
    int charge_slowdown_clear_{0};     // debounce: require 6 consecutive to clear
    int psu_limited_confirm_{0};       // debounce: require 4 consecutive to show
    int psu_limited_clear_{0};        // debounce: require 4 consecutive to clear
    double charge_rate_recent_smoothed_{0};   // EMA for stable display
    double charge_rate_baseline_smoothed_{0};

    // System status debounce (Charging/Idle/Discharging)
    int system_mode_{0};           // 0=idle, 1=charging, 2=discharging
    int system_mode_confirm_{0};   // consecutive samples in pending mode

    // Charger power when charging — fallback for runtime when no battery discharge data
    static constexpr size_t CHG_PWR_N = 720;  // ~1 h at 5s polls
    double chg_pwr_ring_[CHG_PWR_N]{};
    size_t chg_pwr_head_{0};
    size_t chg_pwr_count_{0};

    void push_voltage_soc_sample(double ts, double v, double soc);
    void compute_voltage_trend();
    void compute_soc_trend();
    void compute_extended_analytics(const BatterySnapshot& bat,
                                   const PwrGateSnapshot* chg);
    void compute_system_status(const BatterySnapshot& bat,
                              const PwrGateSnapshot* chg);

    // Helpers (use local time for day/hour boundaries)
    static int unix_day(double ts);
    void reset_daily_if_needed(int day, double ts);
    void push_load_sample(double ts, double watts);
    void recompute_load_stats();
    void push_charger_power_sample(double watts);
    double avg_charger_power_w() const;
    void integrate_discharge_wh(double ts, double power_w);
    void update_health_and_age();
    void update_charging_stage(double bat_v, double target_v,
                               double bat_a, const std::string& state);
};
