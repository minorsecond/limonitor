#pragma once
#include "analytics.hpp"
#include "analytics/anomaly_detection.hpp"
#include "analytics/health_score.hpp"
#include "analytics/resistance_estimator.hpp"
#include "analytics/solar_simulation.hpp"
#include "analytics/weather_forecast.hpp"
#include "data_store.hpp"
#include "database.hpp"
#include "ops_events.hpp"
#include "shelly_client.hpp"
#include "system_events.hpp"
#include "tx_events.hpp"
#include "testing/runner.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// HTTP/1.0 server. GET /, /api/status, /api/cells, /api/history, /metrics
class HttpServer {
public:
    HttpServer(DataStore& store, Database* db, const std::string& bind_addr, uint16_t port,
               int poll_interval_s = 5, testing::TestRunner* test_runner = nullptr);
    ~HttpServer();

    bool start();
    void stop();

    uint16_t port() const { return port_; }

private:
    DataStore&   store_;
    Database*    db_{nullptr};
    testing::TestRunner* test_runner_{nullptr};
    std::string  bind_addr_;
    uint16_t     port_;
    int          listen_fd_{-1};
    int          poll_interval_s_{5};
    std::atomic<bool> running_{false};
    std::thread  thread_;

    void serve_loop();
    void handle(int client_fd);

    static std::string status_json(const BatterySnapshot& s, const std::string& ble_state);
    static std::string cells_json(const BatterySnapshot& s);
    static std::string history_json(const std::vector<BatterySnapshot>& snaps);
    static std::string prometheus(const BatterySnapshot& s);
    static std::string prometheus_charger(const PwrGateSnapshot& pg);
    static std::string svg_history_chart(const std::vector<BatterySnapshot>& hist);
    static std::string svg_charger_chart(const std::vector<PwrGateSnapshot>& hist);
    static std::string charger_json(const PwrGateSnapshot& pg);
    static std::string charger_history_json(const std::vector<PwrGateSnapshot>& snaps);
    static std::string flow_json(const BatterySnapshot& bat, const PwrGateSnapshot& chg,
                                  const shelly::Status& shelly = {});
    shelly::Status fetch_shelly_status();  // reads from cache — zero latency
    void shelly_poll_loop();               // background thread — polls Shelly every N seconds
    shelly::Status shelly_cache_{};
    mutable std::mutex shelly_cache_mu_;
    std::thread shelly_poll_thread_;
    static std::string tx_events_json(const std::vector<TxEvent>& events);
    static std::string system_events_json(const std::vector<SystemEvent>& events);
    static std::string analytics_json(const AnalyticsSnapshot& a,
                                      const DataStore* store = nullptr);
    static std::string anomalies_json(const std::vector<AnomalyEvent>& anomalies);
    static std::string solar_simulation_json(const SolarSimResult& r);
    static std::string battery_resistance_json(double mohm, const std::string& trend,
                                              const std::vector<ResistanceSample>& series);
    static std::string solar_forecast_json(const WeatherForecastResult& r);
    struct UsageCache {
        std::vector<UsageSlotProfile> usage;
        double fallback_w{0};
        double measured_avg_w{0};  // from analytics 24h discharge (0 = unavailable)
        double avg_coeff{0};
        int perf_count{0};
    };
    UsageCache compute_usage_cache() const;
    std::string solar_forecast_week_json(const SolarForecastWeekResult& r,
                                         const UsageCache* cache = nullptr) const;
    static std::string system_health_json(const HealthScoreResult& r);
    static std::string html_dashboard(const BatterySnapshot& s, const std::string& ble_state,
                                      const PwrGateSnapshot& pg,
                                      const std::string& purchase_date,
                                      double hours,
                                      int poll_interval_s,
                                      const std::string& init_settings = "",
                                      const std::string& theme = "");
    static std::string html_solar_page(const std::string& init_settings = "",
                                       const std::string& theme = "");
    static std::string html_settings_page(Database* db);
    static std::string html_ops_log_page(Database* db, const std::string& theme);
    static std::string html_testing_page(Database* db, testing::TestRunner* runner,
                                        const std::string& theme);

    static std::string ops_log_json(const std::vector<OpsEvent>& events);
    static std::string maintenance_status_json(Database* db);

    static void send_response(int fd, int code, const char* content_type, const std::string& body);
};
