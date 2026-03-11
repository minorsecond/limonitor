#pragma once
#include <chrono>
#include <string>
#include <vector>

struct ChargeAcceptanceBucket;
class Database;

struct WeatherConfig {
    std::string api_key;
    std::string zip_code{"80112"};
};

struct WeatherForecastResult {
    bool valid{false};
    double tomorrow_generation_wh{0};
    double cloud_cover{0};
    std::string expected_battery_state;
    std::string error;
};

struct DailySolarForecast {
    std::string date;           // YYYY-MM-DD
    double kwh{0};
    double cloud_cover{0};
    std::string optimal_start;  // HH:MM
    std::string optimal_end;
    std::string optimal_reason; // Why blank: "Overcast", "No clear window", etc.
    double sun_hours_effective{0};
    bool is_daytime{true};
    // Max recovery (solar-only) with 95% CI
    double max_recovery_pct{0};
    double max_recovery_pct_lo{0};
    double max_recovery_pct_hi{0};
    double max_recovery_ah{0};
    double max_recovery_ah_lo{0};
    double max_recovery_ah_hi{0};
    double max_recovery_wh{0};
    double max_recovery_wh_lo{0};
    double max_recovery_wh_hi{0};
    bool is_extended{false};  // from 16-day API, hidden by default
};

struct SolarForecastSlot {
    int64_t timestamp{0};
    double kwh{0};
    double kwh_lo{0};
    double kwh_hi{0};
    double cloud_cover{0};
    bool daytime{false};
};

struct SolarForecastWeekResult {
    bool valid{false};
    std::string error;
    std::vector<DailySolarForecast> daily;
    std::vector<SolarForecastSlot> slots;
    double week_total_kwh{0};
    double recovery_wh{0};       // max solar-only recovery over week
    int days_to_full{0};        // days to reach 100% from current SoC (solar only)
    std::string best_day;       // date with highest kWh
    double nominal_ah{0};
    double battery_voltage{0};
    bool realistic{false};       // true if charge acceptance taper was applied
    bool realistic_from_history{false}; // true if taper came from measured data
};

class WeatherForecast {
public:
    explicit WeatherForecast(const WeatherConfig& cfg, Database* db = nullptr);

    void set_config(const WeatherConfig& cfg);

    void invalidate_cache() const;

    double current_cloud_cover() const;

    // Uses 30-minute cache. panel_watts and efficiency for solar estimate.
    WeatherForecastResult get_forecast(double panel_watts, double efficiency,
                                      double max_charge_a = 0, double battery_voltage = 0,
                                      double nominal_ah = 0, double current_soc_pct = 0) const;

    // Week forecast (5 days from API, aggregated to daily). Same cache.
    // When charge_profile is non-empty, applies a realistic SoC-dependent charge
    // acceptance taper derived from historical charger data.
    SolarForecastWeekResult get_forecast_week(double panel_watts, double efficiency,
                                             double max_charge_a = 0, double battery_voltage = 0,
                                             double nominal_ah = 0, double current_soc_pct = 0,
                                             const std::vector<ChargeAcceptanceBucket>* charge_profile = nullptr) const;

    // Parsed entry from the 16-day daily API
    struct DailyEntry {
        int64_t dt{0};
        double cloud_cover{0};   // 0-1
        double daylight_h{12};   // hours from sunrise to sunset
        int64_t sunrise{0};
        int64_t sunset{0};
    };

    static std::vector<DailyEntry> parse_daily_entries(const std::string& json);

private:
    WeatherConfig cfg_;
    Database* db_{nullptr};
    mutable std::string cached_json_;
    mutable std::chrono::steady_clock::time_point cache_time_{};
    mutable std::string cached_daily_json_;
    mutable std::chrono::steady_clock::time_point cache_daily_time_{};
    static constexpr int CACHE_SEC = 1800;
    static constexpr int DB_CACHE_MAX_AGE = 7200; // 2 hours: use DB cache if API unreachable

    std::string fetch_api() const;
    std::string fetch_daily_api() const;
    double parse_cloud_cover(const std::string& json) const;
    void parse_forecast_list(const std::string& json,
                            std::vector<std::tuple<int64_t, double, bool>>& out) const;
};
