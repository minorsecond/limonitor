#pragma once
#include "battery_data.hpp"
#include "ops_events.hpp"
#include "pwrgate.hpp"
#include "system_events.hpp"
#include "testing/types.hpp"
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// SQLite persistence
struct UsageSlotProfile {
    int slot;           // 0-7 (3-hour slot: 0=00:00-03:00, 1=03:00-06:00, ...)
    double avg_w{0};
    double stddev_w{0};
    int sample_count{0};
};

// Charge acceptance by SoC bucket: what fraction of max current does the charger
// actually deliver at a given state-of-charge, derived from historical data.
struct ChargeAcceptanceBucket {
    int soc_lo{0};              // SoC range lower bound (%)
    int soc_hi{0};              // SoC range upper bound (%)
    double acceptance_ratio{1}; // avg(bat_a) / avg(tgt_a), 0..1
    double avg_current_a{0};    // average observed charge current (A)
    double avg_target_a{0};     // average charger target current (A)
    double stddev_ratio{0};     // standard deviation of the ratio
    int sample_count{0};
};

class Database {
public:
    explicit Database(std::string path);
    ~Database();

    // Open (or create) the database and run schema migrations.
    bool open();
    void close();
    bool is_open() const;

    void insert_battery(const BatterySnapshot& s);
    void insert_charger(const PwrGateSnapshot& p);
    void insert_system_event(const SystemEvent& e);
    std::vector<SystemEvent> load_system_events(size_t n = 200) const;

    void insert_ops_event(const OpsEvent& e);
    std::vector<OpsEvent> load_ops_events(size_t n = 200, const std::string& type_filter = "") const;
    bool update_ops_event_subtype(int64_t id, const std::string& subtype);

    // Checkpoint WAL into main db (updates main file mtime; normally writes go to -wal).
    void checkpoint();

    // Load historical rows (oldest first) to pre-populate DataStore on startup.
    std::vector<BatterySnapshot>  load_battery_history(size_t n = 2880) const;
    std::vector<PwrGateSnapshot>  load_charger_history(size_t n = 2880) const;

    // Average power consumption by 3-hour slot (0-7), from recent battery discharge data.
    std::vector<UsageSlotProfile> get_usage_profile(int days_back = 7) const;

    // Charge acceptance curve: for each 10% SoC bucket, what fraction of
    // target_a does the charger actually deliver? Built from time-correlated
    // charger_readings (bat_a, tgt_a) and battery_readings (soc).
    std::vector<ChargeAcceptanceBucket> get_charge_acceptance_profile(int days_back = 14) const;

    std::string get_setting(const std::string& key) const;
    void set_setting(const std::string& key, const std::string& value);

    // Weather cache: raw API JSON responses, survives restarts
    void save_weather_cache(const std::string& key, const std::string& json);
    std::string load_weather_cache(const std::string& key, int64_t max_age_sec = 7200) const;

    // Weather daily: parsed per-day forecast data for historical analysis
    struct WeatherDailyRow {
        std::string date;       // YYYY-MM-DD
        double cloud_cover{0};
        double kwh_forecast{0};
        double sun_hours{0};
        std::string source;     // "3h", "daily"
        int64_t updated_at{0};
    };
    void upsert_weather_daily(const WeatherDailyRow& row);
    std::vector<WeatherDailyRow> load_weather_daily(int days_back = 90) const;

    // Solar performance: actual vs theoretical output coefficient
    struct SolarPerfRow {
        int64_t ts{0};
        double cloud_cover{0};   // 0-1 from forecast
        double actual_w{0};      // bat_v * bat_a (charge power)
        double theoretical_w{0}; // panel_watts * (1 - cloud)
        double coefficient{0};   // actual / theoretical
        double sol_v{0};         // panel voltage
        double bat_v{0};         // battery voltage
        double bat_a{0};         // charge current
        double soc{0};           // battery SoC %
    };
    void insert_solar_performance(const SolarPerfRow& row);
    std::vector<SolarPerfRow> load_solar_performance(int days_back = 30) const;
    double get_avg_solar_coefficient(int days_back = 30) const;

    // Testing framework
    struct TestRunRow {
        int64_t id{0};
        std::string test_type;
        int64_t start_time{0};
        int64_t end_time{0};
        int duration_seconds{0};
        std::string result;
        double initial_soc{0};
        double initial_voltage{0};
        double average_load{0};
        std::string metadata_json;
        std::string user_notes;
    };
    int64_t insert_test_run(const TestRunRow& row);
    bool update_test_run(int64_t id, int64_t end_time, int duration_seconds,
                        const std::string& result, const std::string& metadata_json);
    bool update_test_run_notes(int64_t id, const std::string& notes);
    std::vector<TestRunRow> load_test_runs(size_t limit = 100) const;
    std::optional<TestRunRow> load_test_run(int64_t id) const;

    void insert_test_telemetry_batch(const std::vector<testing::TestTelemetrySample>& samples);
    std::vector<testing::TestTelemetrySample> load_test_telemetry(int64_t test_id,
                                                                  size_t limit = 10000) const;
    double get_test_min_voltage(int64_t test_id) const;

    // Mark any test_runs with result='running' as 'aborted' (recovery from crashes).
    void abort_interrupted_tests();

    // Grid connectivity events — for outage/maintenance classification
    struct GridEventRow {
        int64_t id{0};
        int64_t start_ts{0};
        int64_t end_ts{0};
        int duration_s{0};
        double soc_start{0};
        double soc_end{0};
        std::string classification;  // "unclassified", "outage", "maintenance", "false_alarm"
        std::string user_notes;
        int64_t classified_ts{0};
    };
    int64_t insert_grid_event(int64_t start_ts, double soc_start);
    bool close_grid_event(int64_t id, int64_t end_ts, int duration_s, double soc_end);
    bool classify_grid_event(int64_t id, const std::string& classification, const std::string& notes);
    std::vector<GridEventRow> load_grid_events(size_t limit = 100) const;
    std::vector<GridEventRow> load_unclassified_grid_events() const;

    // Test schedules (automated tests)
    struct TestScheduleRow {
        int64_t id{0};
        std::string test_type;
        std::string frequency;   // "daily", "weekly", "monthly"
        int run_hour{2};
        int run_minute{0};
        int day_of_month{1};     // for monthly
        int64_t next_run_ts{0};
        bool enabled{true};
        int64_t last_skip_ts{0};
        std::string last_skip_reason;
    };
    std::vector<TestScheduleRow> load_test_schedules() const;
    int64_t insert_test_schedule(const TestScheduleRow& row);
    bool update_test_schedule_next_run(int64_t id, int64_t next_run_ts);
    bool update_test_schedule_skip(int64_t id, int64_t ts, const std::string& reason);
    bool delete_test_schedule(int64_t id);

    // Battery analytics persistence
    void insert_battery_resistance(int64_t ts, double resistance_ohms, double load_current,
                                   double voltage_drop, int64_t test_id, const std::string& source);
    std::vector<std::pair<int64_t, double>> load_battery_resistance_history(int days_back = 365) const;

    void insert_battery_capacity_test(int64_t ts, double measured_wh, double soc_start,
                                      double soc_end, double energy_wh, double health_pct,
                                      int64_t test_id);
    std::vector<std::pair<int64_t, double>> load_battery_capacity_history(int days_back = 365) const;

    void insert_voltage_sag(int64_t ts, double v_before, double v_min, double load_a,
                            double sag_v, const std::string& source);
    std::vector<std::pair<int64_t, double>> load_voltage_sag_history(int days_back = 90) const;

    void insert_charger_efficiency(int64_t ts, double efficiency, double input_w, double battery_w);
    std::vector<std::pair<int64_t, double>> load_charger_efficiency_history(int days_back = 90) const;

    const std::string& path() const { return path_; }
    int64_t file_size() const;
    std::vector<std::pair<std::string, int64_t>> table_sizes() const;

    /**
     * @brief Maintenance routine to prevent the database from growing indefinitely.
     * Deletes high-resolution telemetry (battery/charger) older than max_age_days.
     * Keeps events and test runs for longer.
     * @param max_age_days - Delete telemetry data older than this many days.
     */
    void cleanup(int max_age_days, int system_event_max_age_days, int ops_event_max_age_days);

    // Platform default:
    //   macOS: ~/Library/Application Support/limonitor/limonitor.db
    //   Linux: $XDG_DATA_HOME/limonitor/limonitor.db  (or ~/.local/share/...)
    static std::string default_path();

private:
    std::string        path_;
    void*              db_{nullptr};   // sqlite3*
    mutable std::mutex mu_;

    mutable std::unordered_map<std::string, std::string> settings_cache_;
    mutable std::chrono::steady_clock::time_point settings_cache_time_{};
    static constexpr int SETTINGS_CACHE_TTL_MS = 2000;

    mutable std::vector<UsageSlotProfile> usage_profile_cache_;
    mutable std::time_t usage_profile_cache_ts_{0};
    static constexpr int USAGE_PROFILE_CACHE_TTL_S = 60;
    void invalidate_settings_cache() const;

    bool migrate();
    bool exec(const char* sql);
};
