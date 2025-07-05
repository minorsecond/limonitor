#pragma once
#include "battery_data.hpp"
#include "pwrgate.hpp"
#include "system_events.hpp"
#include <mutex>
#include <string>
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

    const std::string& path() const { return path_; }

    // Platform default:
    //   macOS: ~/Library/Application Support/limonitor/limonitor.db
    //   Linux: $XDG_DATA_HOME/limonitor/limonitor.db  (or ~/.local/share/...)
    static std::string default_path();

private:
    std::string        path_;
    void*              db_{nullptr};   // sqlite3*
    mutable std::mutex mu_;

    bool migrate();
    bool exec(const char* sql);
};
